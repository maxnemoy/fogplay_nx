#include "features/providers/infrastructure/foghub_html.hpp"
#include "features/providers/infrastructure/foghub_review_html.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
// Explicit sanitized page inputs only. Never emit page content, IDs, or exception text.
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream manifest(argv[1]);
    if (!manifest) return 2;
    std::string line;
    unsigned count = 0, failed = 0;
    while (std::getline(manifest, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string kind, id, expectation, path;
        if (!std::getline(fields, kind, '\t') || !std::getline(fields, id, '\t') ||
            !std::getline(fields, expectation, '\t') || !std::getline(fields, path) ||
            (expectation != "accept" && expectation != "reject")) return 2;
        std::ifstream input(path, std::ios::binary);
        if (!input) return 2;
        std::ostringstream bytes; bytes << input.rdbuf();
        bool accepted = true;
        try {
            if (kind == "preview") (void)fog::foghub::parseComputerPreview(bytes.str(), id);
            else if (kind == "games") (void)fog::foghub::parseGames(bytes.str());
            else if (kind == "computers") (void)fog::foghub::parseComputers(bytes.str(), id);
            else if (kind == "history") (void)fog::foghub::parseHistory(bytes.str());
            else if (kind == "connect") (void)fog::foghub::parseConnectForm(bytes.str(), id);
            else if (kind == "review") (void)fog::foghub::parseSessionReviewForm(bytes.str(), id);
            else return 2;
        } catch (const std::exception&) { accepted = false; }
        ++count;
        if (accepted != (expectation == "accept")) ++failed;
        std::cout << "saved-page " << count << ' ' << kind << ' ' << (accepted ? "accepted" : "rejected") << '\n';
    }
    std::cout << "saved-pages checked=" << count << " failed=" << failed << '\n';
    return count && !failed ? 0 : 1;
}