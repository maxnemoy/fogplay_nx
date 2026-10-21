#include "features/streaming/infrastructure/transport_history.hpp"
#include <cassert>
#include <iostream>

int main() {
    using H = stream::TransportHistory;
    H history;
    assert(history.diagnostics().find("firstTerminal=none") != std::string::npos);
    history.record(H::Kind::Ice, 3, 100);
    history.record(H::Kind::Peer, 2, 101);
    history.record(H::Kind::Peer, 4, 200, true, 2, 5, 17000);
    history.record(H::Kind::Peer, 5, 201, true, 3, 6, 0);
    history.record(H::Kind::Ice, 6, 202, true, 4, 7, 1);
    const auto details = history.diagnostics();
    assert(details.find("firstTerminal=peer:4@200") != std::string::npos);
    assert(details.find("terminalRtpAgeMs=2 terminalAudioAgeMs=5 terminalControlAgeMs=17000") != std::string::npos);
    assert(details.find("transportHistory=ice:3@100,peer:2@101,peer:4@200,peer:5@201,ice:6@202") != std::string::npos);
    for (unsigned i = 0; i < 1000; ++i) history.record(H::Kind::Peer, 2, 300 + i);
    const auto bounded = history.diagnostics();
    assert(bounded.find("firstTerminal=peer:4@200") != std::string::npos);
    assert(bounded.find("transportHistory=peer:2@1284,") != std::string::npos);
    assert(bounded.find("peer:2@1283") == std::string::npos);
    assert(bounded.size() < 1024);
    history = {};
    assert(history.diagnostics().find("firstTerminal=none") != std::string::npos);
    history.record(H::Kind::Failure, 2, 0, true);
    assert(history.diagnostics().find("firstTerminal=callback-failure:2@0") != std::string::npos);
    assert(history.diagnostics().find("terminalControlAgeMs=-1") != std::string::npos);
    std::cout << "Transport history: first failure preserved through close, receive ages, bounded ordering and reset PASS\n";
}
