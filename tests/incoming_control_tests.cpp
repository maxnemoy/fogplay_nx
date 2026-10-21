#include "features/streaming/infrastructure/incoming_control.hpp"
#include <cassert>
#include <iostream>

using Json = nlohmann::json;
using Incoming = stream::IncomingControl;
int main() {
    Incoming incoming;
    stream::quality::Telemetry snapshot;
    // Larger than the observed 702-message burst, with sparse independent fields.
    incoming.receive(Incoming::classify(Json{{"id","stats"},{"preset","MID"},
        {"target_bitrate",18000000},{"fps",30},{"autoquality",true},{"server_codec","h264"}}.dump()),100);
    for (unsigned n=0; n<1000; ++n) {
        incoming.receive(Incoming::classify(Json{{"id","ignored-private-id"},{"payload","private"}}.dump()),101+n);
        incoming.receive(Incoming::classify(Json{{"id","stats"},{"fps",60}}.dump()),101+n);
    }
    incoming.receive(Incoming::classify(Json{{"id","stats"},{"preset","private"},{"fps",-1},
        {"target_bitrate","private"},{"autoquality",0},{"server_codec",Json::array()}}.dump()),1200);
    assert(incoming.takeStats(snapshot,1500));
    assert(snapshot.count == 1002 && snapshot.at == 1200);
    assert(snapshot.profile.value == stream::quality::Profile::Middle);
    assert(snapshot.profile.at == 100 && snapshot.profile.revision == 1);
    assert(snapshot.targetMbps.value == 18 && snapshot.targetMbps.at == 100);
    assert(snapshot.fps.value == 60 && snapshot.fps.at == 1100 && snapshot.fps.revision == 1001);
    assert(snapshot.automatic.value && snapshot.automatic.at == 100 && snapshot.codec.value == "H264");
    assert(!incoming.takeStats(snapshot,1501));
    // Taking a snapshot must not reset revisions or refresh stale optional fields.
    incoming.receive(Incoming::classify(R"({"id":"stats","preset":"HIGH"})"),2000);
    assert(incoming.takeStats(snapshot,2100));
    assert(snapshot.profile.revision == 2 && snapshot.profile.at == 2000);
    assert(snapshot.fps.revision == 1001 && snapshot.fps.at == 1100);
    assert(incoming.diagnostics().find("incomingBurstHigh=2002") != std::string::npos);
    assert(incoming.diagnostics().find("incomingDelayHighMs=1400") != std::string::npos);
    assert(incoming.diagnostics().find("statsCoalesced=1001") != std::string::npos);
    assert(incoming.diagnostics().find("private") == std::string::npos);
    auto ping = Incoming::classify(R"({"id":"ping","ts":123})");
    assert(ping.kind == Incoming::Kind::Ping && Json::parse(ping.pong).at("ts") == 123);
    for (const auto* value : {"{bad", "[]", R"({"id":17})"})
        assert(Incoming::classify(value).kind == Incoming::Kind::Invalid);
    assert(Incoming::classify(std::string(65537,' ')).kind == Incoming::Kind::Invalid);
    assert(Incoming::classify(R"({"id":"close"})").kind == Incoming::Kind::Close);
    assert(Incoming::classify(R"({"id":"error"})").kind == Incoming::Kind::Error);
    assert(Incoming::classify("{}").kind == Incoming::Kind::Ignored);
    std::cout << "Incoming control classification, >702 burst, sparse stats times/revisions, ping, sanitization passed\n";
}
