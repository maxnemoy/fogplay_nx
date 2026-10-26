#include "features/session/infrastructure/paired_checkout_storage.hpp"
#include <nlohmann/json.hpp>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
std::string read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write(const std::string& path, const std::string& data) { std::ofstream(path, std::ios::binary) << data; }
}
int main() {
    char pattern[] = "/tmp/fog-paired-journal-XXXXXX";
    const char* temporary = mkdtemp(pattern);
    assert(temporary);
    const std::filesystem::path directory(temporary);
    const auto path = (directory / "pending.json").string();
    auto journal = fog::makePairedCheckoutJournal(path);
    assert(!journal->load().pending);
    fog::PairedCheckoutRecord record;
    record.provider = fog::ProviderId::FogHub;
    record.account = {record.provider, "login:fixture-user"};
    record.computer = {record.provider, "42"};
    journal->write(record);
    auto loaded = journal->load();
    assert(loaded.pending && !loaded.invalid && loaded.record.account == record.account && loaded.record.rental.empty());
    record.rental = {record.provider, "8401"};
    record.phase = fog::PairedCheckoutPhase::RentalKnown;
    journal->write(record);
    loaded = journal->load();
    assert(!loaded.invalid && loaded.record.rental == record.rental);
    record.phase = fog::PairedCheckoutPhase::StopRequested;
    record.stopRequested = record.stopAttempted = true;
    journal->write(record);
    assert(journal->load().record.stopAttempted);
    const auto valid = read(path);
    assert(valid.find("pin") == std::string::npos && valid.find("host") == std::string::npos);
    assert(valid.find("csrf") == std::string::npos && valid.find("endpoint") == std::string::npos);
    for (const auto& mutation : {std::string("{bad"), std::string(4097, 'x'), std::string("[]")}) {
        write(path, mutation);
        loaded = journal->load();
        assert(loaded.pending && loaded.invalid);
    }
    const auto data = nlohmann::json::parse(valid);
    for (const auto* field : {"version", "provider", "account", "computer", "rental", "phase", "stopRequested", "stopAttempted"}) {
        auto missing = data;
        missing.erase(field);
        write(path, missing.dump());
        assert(journal->load().invalid);
    }
    for (const auto* field : {"version", "stopRequested", "stopAttempted"}) {
        auto malformed = data;
        malformed[field] = "invalid";
        write(path, malformed.dump());
        assert(journal->load().invalid);
    }
    auto malformed = data;
    malformed["provider"] = "other";
    write(path, malformed.dump());
    assert(journal->load().invalid);
    malformed = data;
    malformed["pin"] = "0000";
    write(path, malformed.dump());
    assert(journal->load().invalid);
    malformed = data;
    malformed["phase"] = "intent";
    malformed["stopRequested"] = false;
    malformed["stopAttempted"] = false;
    write(path, malformed.dump());
    assert(journal->load().invalid);
    auto mixed = record;
    mixed.account.provider = fog::ProviderId::Mts;
    bool failed = false;
    try { journal->write(mixed); } catch (const fog::JournalError&) { failed = true; }
    assert(failed);
    journal->write(record);
    // Interrupted replacement: recover committed backup only, never temporary bytes.
    std::filesystem::rename(path, path + ".bak");
    write(path + ".tmp", "incomplete");
    loaded = journal->load();
    assert(loaded.pending && !loaded.invalid && loaded.record.stopAttempted);
    journal->clear();
    assert(!journal->load().pending && !std::filesystem::exists(path + ".bak"));
    write(path, "malformed-primary");
    write(path + ".bak", valid);
    assert(journal->load().invalid); // Existing corrupt primary must not resurrect old backup.
    journal->clear();
    // Read failure and invalid path types stay unresolved, never become an empty journal.
    std::filesystem::create_directory(path);
    assert(journal->load().invalid && journal->load().pending);
    failed = false;
    try { journal->write(record); } catch (const fog::JournalError&) { failed = true; }
    assert(failed);
    std::filesystem::remove_all(directory);
    std::cout << "Paired checkout storage tests passed\n";
}
