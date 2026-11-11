#include "features/streaming/infrastructure/bounded_network_events.hpp"
#include <cassert>
#include <iostream>

int main() {
    using Journal = stream::telemetry::network::EventJournal<unsigned>;
    Journal journal;
    Journal::Entry entries[Journal::Capacity + 1]{};
    entries[Journal::Capacity].sequence = 999;
    assert(journal.peekBatch(nullptr, 32) == 0);
    assert(journal.peekBatch(entries, 0) == 0);
    assert(journal.peekBatch(entries, 33) == 0);
    for (unsigned i = 1; i <= 40; ++i) journal.push(i);
    assert(journal.peekBatch(entries, 33) == 32);
    assert(entries[32].sequence == 999);
    assert(journal.size() == 32 && journal.cursor() == 0);
    for (unsigned i = 0; i < 32; ++i) {
        assert(entries[i].value == i + 9);
        assert(entries[i].sequence == i + 9);
        assert(entries[i].missed == 8);
    }
    // Enqueue happens outside the lock. Concurrent producer overflow must not
    // make retirement consume an event outside the captured immutable prefix.
    for (unsigned i = 41; i <= 44; ++i) journal.push(i);
    for (unsigned i = 0; i < 32; ++i)
        assert(journal.retire(entries[i].sequence, true) == (i >= 4));
    assert(journal.cursor() == 40 && journal.size() == 4);
    assert(entries[0].value == 9 && entries[0].missed == 8);
    assert(journal.peekBatch(entries, 2) == 2);
    assert(entries[0].value == 41 && entries[1].value == 42);
    assert(entries[0].missed == 12 && entries[1].missed == 12);
    // An unhealthy-sink admission is uncertain, never retried as a rejection.
    assert(journal.retire(entries[0].sequence, false));
    assert(journal.retire(entries[1].sequence, false));
    assert(journal.uncertain() == 2 && journal.lost() == 14);
    assert(journal.peekBatch(entries, 32) == 2);
    assert(entries[0].value == 43 && entries[0].missed == 14);
    assert(entries[1].value == 44 && entries[1].missed == 14);
    std::cout << "Event batch immutable prefix, bounded copy, overflow retirement and uncertainty PASS\n";
}
