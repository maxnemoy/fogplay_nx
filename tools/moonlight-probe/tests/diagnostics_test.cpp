#include "Diagnostics.hpp"
#include <cassert>
#include <stdexcept>
#include <string>

std::string received;
void capture(const std::string& message) { received = message; }
void failingSink(const std::string&) { throw std::runtime_error("sink failed"); }

int main() {
    using namespace moonlight_diagnostics;
    const std::string sensitive = "secret-value-must-not-be-logged";
    const std::string host = "https://192.168.1.2:47984";
    assert(std::string(operation(host + "/pair?uniqueid=" + sensitive + "&phrase=pairchallenge")) == "pair.https_confirm");
    assert(std::string(operation(host + "/pair?clientpairingsecret=" + sensitive)) == "pair.client_secret");
    assert(std::string(operation(host + "/pair?phrase=getservercert&clientcert=" + sensitive)) == "pair.wait_pin");
    assert(std::string(operation(host + "/unknown?" + sensitive)) == "other");
    sink.store(capture);
    emit(operation(host + "/pair?clientchallenge=" + sensitive));
    assert(received == "pair.client_challenge");
    assert(received.find(sensitive) == std::string::npos);
    sink.store(failingSink);
    emit("must not interrupt cleanup");
    sink.store(nullptr);
    emit("disabled");
}
