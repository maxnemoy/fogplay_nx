#include "features/session/infrastructure/api.hpp"
#include <iostream>
int main() {
    try {
        fog::Api api("11111111111141118111111111111111");
        const auto first = api.games("", 1, 3);
        if (first.empty()) throw std::runtime_error("Catalog is empty");
        const auto next = api.games("", 2, 3);
        const auto search = api.games(first[0]["name"].get<std::string>(), 1, 3);
        const auto pcs = api.computers(first[0]["id"].get<int>(), 1, 3);
        std::cout << "Anonymous GET passed: first games=" << first.size()
            << ", second page=" << next.size() << ", search=" << search.size()
            << ", computers=" << pcs["computers"].size() << ". No login or orders.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Live probe failed: " << error.what() << '\n';
        return 1;
    }
}