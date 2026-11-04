#include "features/streaming/domain/recovery_pli_schedule.hpp"
#include <cassert>
#include <iostream>

using Schedule = stream::RecoveryPliSchedule;
using std::chrono::milliseconds;

int main() {
    const auto t0 = Schedule::Clock::time_point{} + std::chrono::seconds(100);
    {
        // Within one episode: 250/500/1000/2000/4000 ms, then capped.
        Schedule schedule;
        schedule.update(true, true, t0);
        for (const int expected : {250, 500, 1000, 2000, 4000, 4000}) {
            assert(schedule.interval() == milliseconds(expected));
            schedule.requested();
            schedule.update(true, true, t0);
        }
    }
    {
        // A storm of short episodes: each first request waits 250/500/1000/2000
        // (cap 2000); later requests in an episode continue the in-episode backoff.
        Schedule schedule;
        auto now = t0;
        for (const int expected : {250, 500, 1000, 2000, 2000}) {
            schedule.update(true, true, now);
            assert(schedule.interval() == milliseconds(expected));
            schedule.requested();
            schedule.update(true, true, now);
            assert(schedule.interval() == milliseconds(500));
            now += milliseconds(300);
            schedule.update(false, true, now);
        }
        assert(schedule.episodeLevel() == Schedule::MaxEpisodeShift);
    }
    {
        // An episode that never requested a keyframe does not raise the level.
        Schedule schedule;
        schedule.update(true, true, t0);
        schedule.update(false, true, t0 + milliseconds(10));
        schedule.update(true, true, t0 + milliseconds(20));
        assert(schedule.interval() == milliseconds(250) && schedule.episodeLevel() == 0);
    }
    {
        // 30 s of continuous video without a keyframe need resets the level;
        // a video stall restarts that stability period.
        Schedule schedule;
        schedule.update(true, true, t0);
        schedule.requested();
        schedule.update(false, true, t0 + milliseconds(100));
        assert(schedule.episodeLevel() == 1);
        schedule.update(false, true, t0 + milliseconds(29000));
        schedule.update(false, false, t0 + milliseconds(29500));
        schedule.update(false, true, t0 + milliseconds(30200));
        schedule.update(false, true, t0 + milliseconds(59000));
        assert(schedule.episodeLevel() == 1);
        schedule.update(false, true, t0 + milliseconds(60200));
        assert(schedule.episodeLevel() == 0);
        schedule.update(true, true, t0 + milliseconds(60300));
        assert(schedule.interval() == milliseconds(250));
    }
    {
        // reset() (new transport or Auto quality) starts from the first interval.
        Schedule schedule;
        for (int episode = 0; episode < 3; ++episode) {
            schedule.update(true, true, t0); schedule.requested(); schedule.requested();
            schedule.update(false, true, t0);
        }
        assert(schedule.episodeLevel() == 3);
        schedule.reset();
        schedule.update(true, true, t0);
        assert(schedule.interval() == milliseconds(250) && schedule.episodeLevel() == 0);
    }
    {
        // Worst case before the 10-s video recovery deadline: at the episode
        // cap the requests still go out at 2, 2.5, 3.5, 5.5 and 9.5 s.
        Schedule schedule;
        for (int episode = 0; episode < 4; ++episode) {
            schedule.update(true, true, t0); schedule.requested(); schedule.update(false, true, t0);
        }
        schedule.update(true, true, t0);
        int total = 0, requests = 0;
        for (int step = 0; step < 5; ++step) {
            total += static_cast<int>(schedule.interval().count());
            if (total < 10000) ++requests;
            schedule.requested();
        }
        assert(requests == 5 && total == 9500);
    }
    std::cout << "Recovery PLI schedule: in-episode backoff, cross-episode storm cap, stability reset and deadline budget passed\n";
}
