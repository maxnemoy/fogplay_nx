"""Exercise the actual startup worker with synthetic repositories; no network/billing."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/app/application.cpp').read_text()
start = source.index('RestoredProviders ApplicationController::restoreProviders() {')
brace = source.index('{', start)
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
method = source[start:end]

harness = r"""
#include <cassert>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
std::vector<std::string> calls;
struct RestoredProviders {
    bool mts=false, foghub=false;
    std::optional<int> mtsProfile, foghubProfile;
    bool mtsFailed=false, foghubFailed=false;
};
struct Restored { bool restored; };
struct Auth {
    std::string provider;
    bool restored=true, fail=false;
    Restored restore() {
        calls.push_back(provider+".restore");
        if(fail) throw std::runtime_error("synthetic auth outage");
        return {restored};
    }
};
struct Account {
    std::string provider;
    int profile() { calls.push_back(provider+".profile"); return 1; }
};
struct Provider {
    Auth auth_{"FH"}; Account account_{"FH"};
    Auth& auth() { return auth_; }
    Account& account() { return account_; }
};
struct Paired {
    bool pending=false, failStop=false;
    bool unresolved() const { return pending; }
    void bindAuthenticatedAccount() { calls.emplace_back("FH.bind"); }
    void stop() {
        calls.emplace_back("FH.stop");
        if(failStop) throw std::runtime_error("synthetic stop outage");
    }
};
struct Checkout {
    bool pending=false;
    bool unresolved() const { return pending; }
    void stop() { calls.emplace_back("MTS.checkoutStop"); }
};
struct Guard {
    bool pending=false, owned=false, match=true;
    bool persistent() const { return pending; }
    std::string pendingRental() const { return pending ? "synthetic" : ""; }
    std::string journalError() const { return {}; }
    bool ownsSession() const { return owned; }
    bool recoverMatchingSession() { calls.emplace_back("MTS.recover"); return match; }
    void finishAttempt() { calls.emplace_back("MTS.stop"); }
};
struct ApplicationController {
    Auth auth_{"MTS"}; Account account_{"MTS"}; Checkout checkout_; Guard guard_;
    std::unique_ptr<Provider> pairedServices_=std::make_unique<Provider>();
    std::unique_ptr<Paired> paired_=std::make_unique<Paired>();
    RestoredProviders restoreProviders();
};
""" + method + r"""
void expect(const std::vector<std::string>& expected) { assert(calls==expected); calls.clear(); }
int main() {
    {
        ApplicationController app;
        const auto result=app.restoreProviders();
        expect({"MTS.restore","FH.restore","MTS.profile","FH.profile"});
        assert(result.mts && result.foghub && result.mtsProfile && result.foghubProfile);
    }
    {
        ApplicationController app;app.paired_->pending=true;
        app.restoreProviders();
        expect({"FH.restore"});
    }
    {
        ApplicationController app;app.guard_.pending=true;
        app.restoreProviders();
        expect({"MTS.restore"});
    }
    {
        ApplicationController app;app.checkout_.pending=true;
        app.restoreProviders();
        expect({"MTS.restore"});
    }
    {
        ApplicationController app;app.paired_->pending=true;app.checkout_.pending=true;
        app.restoreProviders();
        expect({"FH.restore"});
    }
    {
        ApplicationController app;app.paired_->pending=true;app.pairedServices_->auth_.restored=false;
        const auto result=app.restoreProviders();
        expect({"FH.restore"});
        assert(!result.foghub && !result.foghubProfile);
    }
    {
        ApplicationController app;app.paired_->pending=true;app.pairedServices_->auth_.fail=true;
        const auto result=app.restoreProviders();
        expect({"FH.restore"});
        assert(result.foghubFailed && !result.mts);
    }
    {
        ApplicationController app;app.guard_.pending=true;app.auth_.restored=false;
        const auto result=app.restoreProviders();
        expect({"MTS.restore"});
        assert(!result.mts && !result.mtsProfile);
    }
    {
        ApplicationController app;app.paired_->pending=true;app.paired_->failStop=true;
        const auto result=app.restoreProviders();
        expect({"FH.restore"});
        assert(!result.foghubFailed && app.paired_->unresolved());
    }
    {
        ApplicationController app;app.paired_.reset();app.pairedServices_.reset();
        app.restoreProviders();expect({"MTS.restore","MTS.profile"});
    }
    std::cout<<"Startup actual worker: recovery-provider auth only, no unbudgeted stops or unrelated profiles; failures/both journals/MTS-only PASS\n";
}
"""
with tempfile.TemporaryDirectory(prefix='fog-startup-order-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(path / 'test.cpp'),'-o',str(path / 'test')],check=True)
    subprocess.run([str(path / 'test')],check=True,timeout=15)
