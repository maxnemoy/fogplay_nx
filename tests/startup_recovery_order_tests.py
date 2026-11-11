"""Production startup worker uses REAL coordinator's joined recovery scope; no billing."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/app/application.cpp').read_text()
start=source.index('RestoredProviders ApplicationController::restoreProviders(')
brace=source.index('{',start);depth,end=1,brace+1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
method=source[start:end]
harness=r'''
#include "rental_session_fakes.hpp"
#include <iostream>
using Owner=fog::RentalSessionCoordinator;
std::vector<std::string> calls;
struct RestoredProviders {bool mts=false,foghub=false;std::optional<int> mtsProfile,foghubProfile;bool mtsFailed=false,foghubFailed=false;};
struct Restored {bool restored;};
struct Auth {
    std::string provider;bool restored=true,fail=false;
    Restored restore(){calls.push_back(provider+".restore");if(fail)throw std::runtime_error("synthetic auth outage");return {restored};}
};
struct Account {std::string provider;int profile(){calls.push_back(provider+".profile");return 1;}};
struct Provider {Auth auth_{"FH"};Account account_{"FH"};Auth& auth(){return auth_;}Account& account(){return account_;}};
struct ApplicationController {
    Auth auth_{"MTS"};Account account_{"MTS"};
    std::unique_ptr<Provider> pairedServices_=std::make_unique<Provider>();
    rental_test::Fixture rentals;
    void pairedPending(){rentals.disk.paired={{fog::ProviderId::FogHub,rental_test::id(fog::ProviderId::FogHub,"account"),rental_test::id(fog::ProviderId::FogHub,"computer"),rental_test::id(fog::ProviderId::FogHub,"rental"),fog::PairedCheckoutPhase::StopRequested,true,true},true,false};rentals.reset();}
    void sessionPending(){rentals.disk.session={rental_test::id(fog::ProviderId::Mts,"rental"),true,false,{}};rentals.reset();}
    void checkoutPending(){rentals.disk.checkout={fog::CheckoutJournalRecord{rental_test::id(fog::ProviderId::Mts,"account"),rentals.order(),rental_test::id(fog::ProviderId::Mts,"order"),{},{},false},true,false,{}};rentals.reset();}
    RestoredProviders restoreProviders(Owner::AuthScope scope);
    RestoredProviders restoreProviders(){return restoreProviders(rentals.owner->recoveryAuthScope());}
};
'''+method+r'''
void expect(const std::vector<std::string>& expected){assert(calls==expected);calls.clear();}
int main(){
    {ApplicationController app;auto result=app.restoreProviders();expect({"MTS.restore","FH.restore","MTS.profile","FH.profile"});assert(result.mts&&result.foghub&&result.mtsProfile&&result.foghubProfile);}
    {ApplicationController app;app.pairedPending();app.restoreProviders();expect({"FH.restore"});}
    {ApplicationController app;app.sessionPending();app.restoreProviders();expect({"MTS.restore"});}
    {ApplicationController app;app.checkoutPending();app.restoreProviders();expect({"MTS.restore"});}
    {ApplicationController app;app.pairedPending();app.checkoutPending();app.restoreProviders();expect({"FH.restore"});}
    {ApplicationController app;app.pairedPending();app.pairedServices_->auth_.restored=false;auto result=app.restoreProviders();expect({"FH.restore"});assert(!result.foghub&&!result.foghubProfile);}
    {ApplicationController app;app.pairedPending();app.pairedServices_->auth_.fail=true;auto result=app.restoreProviders();expect({"FH.restore"});assert(result.foghubFailed&&!result.mts);}
    {ApplicationController app;app.sessionPending();app.auth_.restored=false;auto result=app.restoreProviders();expect({"MTS.restore"});assert(!result.mts&&!result.mtsProfile);}
    {ApplicationController app;app.pairedPending();app.rentals.fh.finishAction=[]{throw std::runtime_error("synthetic stop outage");};auto result=app.restoreProviders();expect({"FH.restore"});assert(!result.foghubFailed&&app.rentals.owner->state().pairedPending&&app.rentals.fh.stops==0);}
    {ApplicationController app;app.pairedServices_.reset();app.rentals.reset(false);app.restoreProviders();expect({"MTS.restore","MTS.profile"});}
    std::cout<<"Startup production worker + REAL coordinator scope: provider-priority journals, no unbudgeted stops/unrelated profiles, both journals, auth failures and MTS-only PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-startup-order-') as directory:
    path=Path(directory);(path/'test.cpp').write_text(harness)
    sources=['session.cpp','checkout.cpp','paired_checkout.cpp','rental_session_coordinator.cpp']
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'src'),'-I'+str(root/'tests'),*[str(root/'src/features/session/application'/file) for file in sources],str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=20)