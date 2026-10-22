#include "features/streaming/domain/route_snapshot_schedule.hpp"
#include <cassert>
#include <iostream>

int main() {
    stream::RouteSnapshotSchedule schedule;
    assert(schedule.due(0));
    schedule.queried(0);
    // Identical and no-pair results use the same bounded retry cadence.
    for (std::uint64_t now = 0; now < 1000; ++now) assert(!schedule.due(now));
    assert(schedule.due(1000)); schedule.queried(1000);
    assert(!schedule.due(1999) && schedule.due(2000));
    schedule.queried(2000);
    schedule.invalidate(); assert(schedule.due(2001));
    // A failed query retains invalidation, but retries at most once per second.
    schedule.failed(2001);
    assert(!schedule.due(2002) && !schedule.due(3000) && schedule.due(3001));
    schedule.invalidate(); assert(schedule.due(2002));
    schedule.queried(2002); assert(!schedule.due(2002));
    assert(schedule.due(2003, true)); schedule.queried(2003);
    assert(!schedule.due(2004) && schedule.due(3003));
    // A replacement peer has no inherited deadline or route authorization.
    schedule = {}; assert(schedule.due(3004)); schedule.queried(3004);
    assert(schedule.due(1)); schedule.queried(1);
    assert(!schedule.due(1000) && schedule.due(1001));
    std::cout << "Route snapshot schedule: repeated/no-pair cadence, exact boundary, ICE/proof invalidation, failed-query retry, force, fresh peer, clock rewind PASS\n";
}
