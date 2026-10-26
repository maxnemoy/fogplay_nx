#include "features/auth/infrastructure/mts_auth_repository.hpp"
#include "features/account/infrastructure/mts_account_repository.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <unistd.h>

namespace {
template<class T,class=void> struct HasToken : std::false_type {};
template<class T> struct HasToken<T,std::void_t<decltype(std::declval<T>().token)>> : std::true_type {};
static_assert(!HasToken<fog::AuthData>::value && !HasToken<fog::AuthResult>::value);
static_assert(std::is_same_v<decltype(fog::AuthResult{}.auth),fog::AuthData>);
struct Temporary {
    std::filesystem::path directory;
    Temporary() {
        char pattern[]="/tmp/fogplay-repository-tests-XXXXXX";
        const auto created=mkdtemp(pattern);
        if(!created) throw std::runtime_error("Cannot create fixture directory");
        directory=created;
    }
    ~Temporary(){std::error_code ignored;std::filesystem::remove_all(directory,ignored);}
    std::string path() const {return (directory/"auth.json").string();}
};
std::string contents(const std::string& path) {
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
struct Transport {
    bool offline=false;
    unsigned phone=0,captcha=0,otp=0,profile=0,history=0;
    fog::HttpResponse profileResponse{200,R"({"authorized":true})"};
    fog::HttpResponse historyResponse{200,R"([{"comp_name":"fixture-pc","created":"2026-10-26 12:00:00","finished":null,"duration_minutes":12,"sum":37.27}])"};
    std::string lastToken;
    fog::HttpResponse operator()(const std::string& method,const std::string& path,
        const fog::Query&,const std::string& body,const std::string& token) {
        if(path=="/api/login/mts-phone/") {
            assert(method=="POST" && token.empty());++phone;
            const auto payload=fog::Json::parse(body);
            assert(payload.at("phone")=="+79991234567");
            return {200,R"({"form":"captcha","image":"data:image/png;base64,QQ=="})"};
        }
        if(path=="/api/login/mts-captcha/") {++captcha;return {200,R"({"form":"otp"})"};}
        if(path=="/api/login/mts-otp/") {++otp;return {200,R"({"success":true,"token":"fixture-secret-token"})"};}
        if(path=="/api/login/user-info/") {
            assert(method=="GET" && body.empty());++profile;lastToken=token;
            if(offline) throw std::runtime_error("PRIVATE transport details");
            return profileResponse;
        }
        if(path=="/api/order/history/") {assert(method=="GET");++history;return historyResponse;}
        throw std::runtime_error("Unexpected request: fixture forbids rentals and external networking");
    }
};
void signIn(fog::AuthRepository& auth) {
    assert(auth.capabilities().method==fog::LoginMethod::PhoneCode);
    const auto captcha=auth.begin({"8 (999) 123-45-67",{}});
    assert(captcha.step==fog::AuthStep::Captcha && captcha.captchaImage==std::vector<unsigned char>{'A'});
    assert(!captcha.auth.authenticated() && captcha.auth.account.provider==fog::ProviderId::Mts);
    const auto otp=auth.answer({fog::AuthStep::Captcha,"AB12"});
    assert(otp.step==fog::AuthStep::Otp && otp.captchaImage.empty() && !otp.auth.authenticated());
    const auto result=auth.answer({fog::AuthStep::Otp,"1111"});
    assert(result.step==fog::AuthStep::Success && result.auth.authenticated() && result.captchaImage.empty());
    assert(result.auth.account.value.size()==64 && result.auth.account.value.find("79991234567")==std::string::npos);
    assert(result.auth.account.value.find("fixture-secret-token")==std::string::npos);
}
template<class Operation> void rejects(Operation operation,fog::RepositoryErrorKind kind) {
    bool failed=false;
    try{operation();}catch(const fog::RepositoryError& error){failed=true;assert(error.kind()==kind);}
    assert(failed);
}
}
int main() {
    Temporary temporary;
    std::string account;
    {
        Transport transport;
        fog::Api api("11111111111141118111111111111111",[&](auto&&... args){return transport(args...);});
        api.configureAuthStorage(temporary.path());
        fog::MtsAuthRepository adapter(api);fog::AuthRepository& auth=adapter;
        const auto initial=auth.auth();
        assert(!initial.authenticated() && !auth.authenticated());
        auth.validateLogin({"8 (999) 123-45-67",{}});
        rejects([&]{auth.validateLogin({"invalid",{}});},fog::RepositoryErrorKind::InvalidRequest);
        assert(transport.phone==0 && auth.auth().generation==initial.generation && !auth.authenticated());
        signIn(auth);
        const auto authenticated=auth.auth();account=authenticated.account.value;
        assert(authenticated.generation>initial.generation && auth.authenticated());
        assert(transport.phone==1 && transport.captcha==1 && transport.otp==1);
        assert(std::filesystem::exists(temporary.path()) && auth.storageError().empty());
        fog::MtsAccountRepository accountAdapter(api);fog::AccountRepository& repository=accountAdapter;
        assert(repository.profile().account==authenticated.account && transport.lastToken=="fixture-secret-token");
        const auto history=repository.history();
        assert(history.size()==1 && history[0].computerName=="fixture-pc" && history[0].finishedAt.empty());
        assert(history[0].startedAt=="2026-10-26 12:00:00" && history[0].durationMinutes=="12");
        assert(history[0].total && history[0].total->minorUnits==3727 && history[0].total->currency=="RUB");
        transport.historyResponse={200,R"([{"comp_name":null,"duration_minutes":false,"sum":"unknown"}])"};
        const auto partial=repository.history();
        assert(partial[0].computerName.empty() && partial[0].durationMinutes=="false" && !partial[0].total);
        transport.historyResponse={200,R"([null])"};
        rejects([&]{repository.history();},fog::RepositoryErrorKind::InvalidResponse);
        transport.profileResponse={401,"private"};
        rejects([&]{repository.profile();},fog::RepositoryErrorKind::NotAuthenticated);
    }
    {
        const auto durable=contents(temporary.path());
        Transport transport;transport.offline=true;
        fog::Api api("11111111111141118111111111111111",[&](auto&&... args){return transport(args...);});
        api.configureAuthStorage(temporary.path());
        fog::MtsAuthRepository adapter(api);fog::AuthRepository& auth=adapter;
        const auto before=auth.auth().generation;
        assert(!auth.restore().restored && !auth.authenticated());
        assert(auth.auth().generation>before && auth.auth().account.value.empty());
        assert(contents(temporary.path())==durable && auth.storageError().find("PRIVATE")==std::string::npos);
        transport.offline=false;
        assert(auth.restore().restored && auth.authenticated() && auth.auth().account.value==account);
        const auto generation=auth.auth().generation;
        auth.forget();
        assert(!auth.authenticated() && auth.auth().generation>generation && !std::filesystem::exists(temporary.path()));
        const auto requests=transport.profile;
        assert(!auth.restore().restored && transport.profile==requests);
    }
    {
        fog::Api api("11111111111141118111111111111111",[](const std::string&,const std::string&,const fog::Query&,const std::string&,const std::string&) {
            return fog::HttpResponse{200,R"({"form":"captcha","image":"PRIVATE-invalid-base64"})"};
        });
        fog::MtsAuthRepository auth(api);
        rejects([&]{auth.begin({"89991234567",{}});},fog::RepositoryErrorKind::InvalidResponse);
        assert(!auth.authenticated() && auth.auth().account.value.size()==64);
        assert(auth.auth().account.value.find("PRIVATE")==std::string::npos);
    }
    std::cout<<"Typed MTS auth/account repositories: challenge, credential boundary, identity/generation, offline restore, profile/history mapping and errors PASS\n";
}
