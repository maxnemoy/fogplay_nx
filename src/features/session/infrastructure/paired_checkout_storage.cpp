#include "features/session/infrastructure/paired_checkout_storage.hpp"
#include "core/storage/storage.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>

namespace fog {
namespace {
using Json = nlohmann::json;
constexpr std::size_t maxBytes = 4096;
bool textId(const std::string& value, bool optional = false) {
    return (optional || !value.empty()) && value.size() <= 1024 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c != 127; });
}
const char* providerName(ProviderId provider) {
    switch (provider) {
    case ProviderId::Mts: return "mts";
    case ProviderId::FogHub: return "foghub";
    }
    throw JournalError("Invalid paired journal provider");
}
ProviderId parseProvider(const std::string& value) {
    if (value == "mts") return ProviderId::Mts;
    if (value == "foghub") return ProviderId::FogHub;
    throw JournalError("Invalid paired journal provider");
}
const char* phaseName(PairedCheckoutPhase phase) {
    switch (phase) {
    case PairedCheckoutPhase::Intent: return "intent";
    case PairedCheckoutPhase::RentalKnown: return "rental-known";
    case PairedCheckoutPhase::Pairing: return "pairing";
    case PairedCheckoutPhase::Streaming: return "streaming";
    case PairedCheckoutPhase::StopRequested: return "stop-requested";
    }
    throw JournalError("Invalid paired journal phase");
}
PairedCheckoutPhase parsePhase(const std::string& value) {
    if (value == "intent") return PairedCheckoutPhase::Intent;
    if (value == "rental-known") return PairedCheckoutPhase::RentalKnown;
    if (value == "pairing") return PairedCheckoutPhase::Pairing;
    if (value == "streaming") return PairedCheckoutPhase::Streaming;
    if (value == "stop-requested") return PairedCheckoutPhase::StopRequested;
    throw JournalError("Invalid paired journal phase");
}
void validate(const PairedCheckoutRecord& record) {
    providerName(record.provider);
    phaseName(record.phase);
    if (record.account.provider != record.provider || record.computer.provider != record.provider ||
        (!record.rental.empty() && record.rental.provider != record.provider) ||
        !textId(record.account.value) || !textId(record.computer.value) || !textId(record.rental.value, true) ||
        (record.phase == PairedCheckoutPhase::Intent && !record.rental.empty()) ||
        (record.rental.empty() && record.phase != PairedCheckoutPhase::Intent && record.phase != PairedCheckoutPhase::StopRequested) ||
        (record.phase == PairedCheckoutPhase::StopRequested && !record.stopRequested) ||
        (record.stopAttempted && (!record.stopRequested || record.rental.empty() || record.phase != PairedCheckoutPhase::StopRequested)))
        throw JournalError("Invalid paired rental journal record");
}
class FilePairedCheckoutJournal final : public PairedCheckoutJournal {
public:
    explicit FilePairedCheckoutJournal(std::string path) : path_(std::move(path)) {
        if (path_.empty() || path_.find('\0') != std::string::npos)
            throw JournalError("Invalid paired journal path");
    }
    PairedCheckoutLoad load() override {
        PairedCheckoutLoad result;
        try {
            recoverStorage(path_);
            struct stat info{};
            if (lstat(path_.c_str(), &info) != 0 && errno == ENOENT) return result;
            result.pending = result.invalid = true;
            if (!S_ISREG(info.st_mode) || info.st_size < 1 || info.st_size > static_cast<off_t>(maxBytes)) return result;
            FILE* file = std::fopen(path_.c_str(), "rb");
            if (!file) return result;
            char bytes[maxBytes + 1];
            const auto count = std::fread(bytes, 1, sizeof(bytes), file);
            const bool failed = std::ferror(file) != 0;
            const bool closed = std::fclose(file) == 0;
            if (failed || !closed || count > maxBytes) return result;
            const auto data = Json::parse(bytes, bytes + count, nullptr, false);
            if (!data.is_object() || data.size() != 8 || !data.at("version").is_number_integer() || data.at("version") != 1)
                return result;
            PairedCheckoutRecord record;
            record.provider = parseProvider(data.at("provider").get<std::string>());
            record.account = {record.provider, data.at("account").get<std::string>()};
            record.computer = {record.provider, data.at("computer").get<std::string>()};
            record.rental = {record.provider, data.at("rental").get<std::string>()};
            record.phase = parsePhase(data.at("phase").get<std::string>());
            record.stopRequested = data.at("stopRequested").get<bool>();
            record.stopAttempted = data.at("stopAttempted").get<bool>();
            validate(record);
            result.record = std::move(record);
            result.invalid = false;
        } catch (...) { result.pending = result.invalid = true; }
        return result;
    }
    void write(const PairedCheckoutRecord& record) override {
        validate(record);
        const auto data = Json{{"version", 1}, {"provider", providerName(record.provider)},
            {"account", record.account.value}, {"computer", record.computer.value}, {"rental", record.rental.value},
            {"phase", phaseName(record.phase)}, {"stopRequested", record.stopRequested}, {"stopAttempted", record.stopAttempted}};
        const auto bytes = data.dump() + "\n";
        if (bytes.size() > maxBytes) throw JournalError("Paired journal size limit exceeded");
        try { replaceStorage(path_, bytes); }
        catch (...) { throw JournalError("Paired rental journal write failed"); }
    }
    void clear() override {
        try { clearStorage(path_); }
        catch (...) { throw JournalError("Paired rental journal clear failed"); }
    }
private:
    std::string path_;
};
}
std::unique_ptr<PairedCheckoutJournal> makePairedCheckoutJournal(std::string path) {
    return std::make_unique<FilePairedCheckoutJournal>(std::move(path));
}
}
