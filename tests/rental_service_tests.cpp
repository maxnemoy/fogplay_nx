#include "features/session/application/checkout.hpp"
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {
int assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}

struct Rental : fog::RentalService {
    std::string account = std::string(64, 'a');
    std::uint64_t generation = 1;
    fog::Json active = fog::Json::array();
    int creates = 0;
    bool loggedIn() const noexcept override { return true; }
    std::uint64_t authGeneration() const noexcept override { return generation; }
    const std::string& accountIdentity() const noexcept override { return account; }
    fog::Json activeSessions(bool) override { return active; }
    bool finishSession(const std::string& hash) override {
        require(hash == "rental-fixture", "finish only attributed rental");
        active = fog::Json::array();
        return true;
    }
    fog::Json createOrder(const fog::CreateOrderRequest&) override {
        ++creates;
        active = fog::Json::array({{{"hash", "rental-fixture"}}});
        return {{"orderId", 1}};
    }
    fog::Json orderStatus(std::int64_t id) override {
        require(id == 1, "poll only recorded order");
        return {{"id", 1}, {"status", 2}, {"hash", "rental-fixture"}};
    }
};
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("fog-boundary-port-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    try {
        Rental rental;
        fog::SessionGuard sessions(rental, (directory / "session.json").string());
        fog::CheckoutGuard checkout(rental, sessions, (directory / "checkout.json").string());
        require(checkout.create({1, 2, 3, false}).state == fog::CheckoutState::Pending, "create through injected service");
        require(rental.creates == 1, "create sent exactly once");
        for (int i = 0; i < 3; ++i) checkout.poll();
        require(checkout.result().state == fog::CheckoutState::Ready, "three readiness observations permit connection");
        require(sessions.ownsSession(), "ready rental adopted");
        ++rental.generation;
        require(!sessions.ownsSession(), "authentication change invalidates ownership");
        require(checkout.stop().state == fog::CheckoutState::Idle, "stop through injected service");
        require(!checkout.unresolved() && sessions.pendingHash().empty(), "confirmed stop clears recovery markers");
        std::filesystem::remove_all(directory);
        std::cout << "Rental service boundary tests passed: " << assertions << " assertions. No HTTP adapter linked.\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << "Rental service boundary test failed: " << error.what() << '\n';
        return 1;
    }
}
