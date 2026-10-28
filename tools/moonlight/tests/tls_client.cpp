#include <Settings.hpp>
#include <Diagnostics.hpp>
#include <CryptoManager.hpp>
#include <gamestream/http.h>
#include <gamestream/errors.h>
#include <cassert>
#include <cstring>
#include <string>
#include <iostream>
#include <filesystem>

void randomGet(void* target, std::size_t size) { std::memset(target, 0, size); }
void gs_set_error(std::string) {}
std::string gs_error() { return "Host TLS test request failed"; }

int main(int argc, char** argv) {
    assert(argc == 6);
    Settings::instance().set_production(true);
    Settings::instance().set_key_dir(argv[1]);
    Settings::instance().reset_cancel(); Settings::instance().set_timeout_ms(10000);
    assert(CryptoManager::load_cert_key_pair());
    const auto firstCertificate = CryptoManager::cert_data().hex();
    assert(http_init(argv[1]) == GS_OK);
    Data response;
    assert(http_request(argv[3], &response, HTTPRequestTimeoutMedium) != GS_OK); // Fail closed without a pin.
    const auto certificate = Data::read_from_file(argv[2]);
    assert(http_pin_certificate(certificate) == GS_OK);
    moonlight_diagnostics::beginCommand(moonlight_diagnostics::CommandOperation::Resume);
    assert(http_request(argv[3], &response, HTTPRequestTimeoutMedium) == GS_OK);
    auto numeric = moonlight_diagnostics::commandSnapshot();
    assert(numeric.curl == 0 && numeric.http == 200 && numeric.dispatched && !numeric.cancelled);
    moonlight_diagnostics::finishCommand(GS_OK,false);
    assert(std::string(reinterpret_cast<char*>(response.bytes()), response.size()) == "ClientOne");
    moonlight_diagnostics::beginCommand(moonlight_diagnostics::CommandOperation::Resume);
    assert(http_request(std::string(argv[3])+"&unavailable=1", &response, HTTPRequestTimeoutMedium) != GS_OK);
    numeric = moonlight_diagnostics::commandSnapshot();
    assert(numeric.curl == 22 && numeric.http == 503 && numeric.dispatched);
    moonlight_diagnostics::finishCommand(GS_FAILED,false);
    moonlight_diagnostics::beginCommand(moonlight_diagnostics::CommandOperation::Resume);
    assert(http_request(argv[3], &response, HTTPRequestTimeoutMedium) == GS_OK);
    assert(moonlight_diagnostics::commandSnapshot().http == 200); // Fresh transfer replaces the old 503.
    moonlight_diagnostics::finishCommand(GS_OK,false);
    // A different server identity on another port must fail with the same pin.
    assert(http_request(argv[4], &response, HTTPRequestTimeoutMedium) != GS_OK);
    Settings::instance().set_key_dir(argv[5]);
    assert(CryptoManager::load_cert_key_pair());
    const auto secondCertificate = CryptoManager::cert_data().hex();
    assert(firstCertificate.size() != secondCertificate.size() ||
        std::memcmp(firstCertificate.bytes(), secondCertificate.bytes(), firstCertificate.size()) != 0);
    assert(http_init(argv[5]) == GS_OK);
    assert(http_request(argv[3], &response, HTTPRequestTimeoutMedium) == GS_OK);
    assert(std::string(reinterpret_cast<char*>(response.bytes()), response.size()) == "ClientTwo");
    const auto generated = std::filesystem::path(argv[5]) / "generated";
    std::filesystem::create_directory(generated);
    Settings::instance().set_key_dir(generated.string());
    assert(!CryptoManager::load_cert_key_pair());
    assert(CryptoManager::generate_new_cert_key_pair());
    assert(!Data::read_from_file((generated / "client.pem").string()).is_empty());
    assert(!Data::read_from_file((generated / "key.pem").string()).is_empty());
    std::cout << "Moonlight actual mbedTLS mutual TLS/SPKI pin/identity rotation tests passed\n";
}
