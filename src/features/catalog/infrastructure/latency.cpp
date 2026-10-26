#include "core/constants/resources.hpp"
#include "features/catalog/infrastructure/latency.hpp"
#include <nlohmann/json.hpp>
#include "core/network/http.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>

namespace fog {
namespace {
constexpr std::uint64_t sampleLifetimeMs = 60000;
std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Response {
    std::string body;
    const LatencyService::Cancelled* cancelled = nullptr;
};
std::size_t receive(char* bytes, std::size_t size, std::size_t count, void* context) noexcept {
    auto& response = *static_cast<Response*>(context);
    if (size && count > 65536 / size) return 0;
    const auto length = size * count;
    if (length > 65536 - response.body.size()) return 0;
    try { response.body.append(bytes,length); } catch (...) { return 0; }
    return length;
}
int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    const auto& response = *static_cast<Response*>(context);
    try { return (*response.cancelled)() ? 1 : 0; } catch (...) { return 1; }
}
bool perform(const std::string& url, bool head, long timeout,
             const LatencyService::Cancelled& cancelled, std::string& body, int& milliseconds) {
    if (cancelled()) return false;
    std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> handle(curl_easy_init(),curl_easy_cleanup);
    if (!handle) return false;
    Response response{{},&cancelled};
    auto* curl = handle.get();
    // Endpoints are anonymous, redirects/proxies/credentials are never inherited.
    curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
    curl_easy_setopt(curl,CURLOPT_PROXY,"");
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,timeout);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,timeout);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
#ifdef __SWITCH__
    curl_easy_setopt(curl,CURLOPT_CAINFO,fog::constants::resources::CertificateBundle);
#endif
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"https");
#else
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS,CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl,CURLOPT_NOBODY,head ? 1L : 0L);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&response);
    curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,progress);
    curl_easy_setopt(curl,CURLOPT_XFERINFODATA,&response);
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
    if (result != CURLE_OK || cancelled() || status < 200 || status > 599) return false;
    double lookup = 0, connected = 0;
    if (curl_easy_getinfo(curl,CURLINFO_NAMELOOKUP_TIME,&lookup) != CURLE_OK ||
        curl_easy_getinfo(curl,CURLINFO_CONNECT_TIME,&connected) != CURLE_OK) return false;
    const auto measured = LatencyService::connectionMilliseconds(lookup,connected);
    if (!measured) return false;
    milliseconds = *measured;
    body = std::move(response.body);
    return true;
}
std::vector<std::string> loadEndpoints(const LatencyService::Cancelled& cancelled) {
    std::string body;
    int elapsed = 0;
    if (!perform("https://fogplay.mts.ru/stun/get/",false,5000,cancelled,body,elapsed)) return {};
    const auto data = nlohmann::json::parse(body,nullptr,false);
    std::vector<std::string> result;
    if (!data.is_array() || data.size()>64) return result;
    for (const auto& item : data)
        if (item.is_string() && LatencyService::validEndpoint(item.get_ref<const std::string&>()))
            result.push_back(item.get<std::string>());
    return result;
}
std::optional<int> probeEndpoint(const std::string& endpoint,const LatencyService::Cancelled& cancelled) {
    if (!LatencyService::validEndpoint(endpoint)) return {};
    const auto slash = endpoint.find('/');
    const bool legacy = slash != std::string::npos && endpoint.substr(slash)=="/version.js";
    const auto url = "https://" + (legacy ? endpoint.substr(0,slash) + "/" : endpoint);
    int elapsed = 0;
    std::string body;
    if (!perform(url,legacy,5000,cancelled,body,elapsed)) return {};
    return elapsed;
}
}

std::optional<int> LatencyService::connectionMilliseconds(double lookup, double connected) {
    if (!std::isfinite(lookup) || !std::isfinite(connected) || lookup < 0 || connected <= lookup) return {};
    const double milliseconds = (connected - lookup) * 1000;
    if (milliseconds > 3000) return {};
    return std::max(1,static_cast<int>(std::lround(milliseconds)));
}
bool LatencyService::validEndpoint(const std::string& endpoint) {
    if (endpoint.empty() || endpoint.size()>180) return false;
    const auto slash = endpoint.find('/');
    const auto authority = endpoint.substr(0,slash);
    const auto colon = authority.find(':');
    if (colon != std::string::npos && authority.substr(colon) != ":443") return false;
    const auto host = authority.substr(0,colon);
    if (host.empty() || host.front()=='.' || host.front()=='-' || host.back()=='-' || host.find("..")!=std::string::npos) return false;
    if (!std::all_of(host.begin(),host.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='.' || c=='-';
    })) return false;
    bool suffix = false;
    for (const std::string domain : {".remoteplay.mts.ru",".keenetic.link",".keenetic.pro",".netcraze.link",".crazedns.ru"})
        if (host.size()>domain.size() && host.compare(host.size()-domain.size(),domain.size(),domain)==0) suffix=true;
    if (!suffix) return false;
    if (slash == std::string::npos) return true;
    const auto path = endpoint.substr(slash);
    return path=="/" || path=="/version.js" || path=="/assets/images/favicon/favicon.ico";
}
std::string LatencyService::routeKey(const std::string& endpoint) {
    if (!validEndpoint(endpoint)) return {};
    auto host=endpoint.substr(0,endpoint.find_first_of(":/"));
    std::replace(host.begin(),host.end(),'.','_');
    return host;
}
LatencyService::LatencyService(EndpointLoader loader,Probe probe)
    : loader_(loader ? std::move(loader) : loadEndpoints), probe_(probe ? std::move(probe) : probeEndpoint) {
    HttpClient runtime; // Initializes process-wide libcurl before starting workers.
    worker_=std::thread(&LatencyService::run,this);
}
LatencyService::~LatencyService() { stop(); }
void LatencyService::request() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_.load() || busy_) return;
    ++generation_;
    pending_=busy_=true;
    wake_.notify_one();
}
void LatencyService::cancel() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    pending_=busy_=false;
    wake_.notify_one();
}
void LatencyService::stop() noexcept {
    stopped_.store(true);
    cancel();
    if (worker_.joinable()) worker_.join();
}
bool LatencyService::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}
std::optional<int> LatencyService::estimate(const Computer& computer) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<int> best;
    const auto now=nowMs();
    for (const auto& entry:samples_) {
        if (entry.second.milliseconds<=0 || now<entry.second.at || now-entry.second.at>sampleLifetimeMs) continue;
        for (const auto& route:computer.latencyRoutes) {
            if (route.routeKey!=entry.first || route.serverToNodeMs<0 || route.serverToNodeMs>3000) continue;
            const auto combined=entry.second.milliseconds+route.serverToNodeMs;
            if (!best || combined<*best) best=combined;
        }
    }
    return best;
}
void LatencyService::run() {
    while (!stopped_.load()) {
        std::uint64_t generation;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock,[this] { return stopped_.load() || pending_; });
            if (stopped_.load()) return;
            pending_=false;
            generation=generation_.load();
            const auto now=nowMs();
            for (auto entry=samples_.begin();entry!=samples_.end();)
                if (now-entry->second.at>sampleLifetimeMs) entry=samples_.erase(entry); else ++entry;
        }
        const Cancelled cancelled=[this,generation] { return stopped_.load() || generation_.load()!=generation; };
        try {
            auto endpoints=loader_(cancelled);
            if (endpoints.size()>64) endpoints.resize(64);
            std::sort(endpoints.begin(),endpoints.end());
            endpoints.erase(std::unique(endpoints.begin(),endpoints.end()),endpoints.end());
            std::atomic_size_t next{0};
            const auto check=[&] {
                try { while (!cancelled()) {
                    const auto index=next.fetch_add(1);
                    if (index>=endpoints.size()) return;
                    const auto& endpoint=endpoints[index];
                    const auto key=routeKey(endpoint);
                    if (key.empty()) continue;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        const auto old=samples_.find(key);
                        if (old!=samples_.end() && nowMs()-old->second.at<=(old->second.milliseconds>0 ? sampleLifetimeMs : 5000)) continue;
                    }
                    std::vector<int> values;
                    for (int attempt=0;attempt<3 && !cancelled();++attempt) {
                        try {
                            const auto result=probe_(endpoint,cancelled);
                            if (result && *result>0 && *result<=3000) values.push_back(*result);
                        } catch (...) {} // A failed node does not hide the other routes.
                    }
                    // A single valid response is useful; median limits connection scheduling outliers.
                    if (cancelled()) continue;
                    std::sort(values.begin(),values.end());
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!cancelled()) samples_[key]={values.empty() ? -1 : values[values.size()/2],nowMs()};
                } } catch (...) {}
            };
            // Two helpers plus this worker: never more than three active probes.
            std::vector<std::thread> helpers;
            try { for (int i=0;i<2;++i) helpers.emplace_back(check); } catch (...) {}
            check();
            for (auto& thread:helpers) thread.join();
        } catch (...) {} // HTTPS/list failure remains unknown and can be retried on return.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation_.load()==generation) busy_=false;
        }
    }
}
}
