#include "features/settings/infrastructure/deferred_settings.hpp"
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
int assertions = 0;
void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}
}
int main() {
    using fog::settings::DeferredSettings;
    using fog::settings::Key;
    using namespace std::chrono_literals;
    try {
        const auto owner = std::this_thread::get_id();
        {
            DeferredSettings writes(1h, false);
            bool savedOnOwner = false, completed = false;
            writes.submit(Key::Video, [&] { savedOnOwner = std::this_thread::get_id() == owner; },
                [&](bool saved) { completed = saved; });
            require(savedOnOwner && completed, "baseline writes synchronously on owner and reports success before return");
            writes.submit(Key::Video, [] { throw std::runtime_error("synthetic baseline save failure"); },
                [&](bool saved) { completed = saved; });
            require(!completed, "baseline storage failure retains normal completion reporting");
            writes.finish();
            bool refused = false;
            try { writes.submit(Key::Video, [] {}, [](bool) {}); }
            catch (const std::logic_error&) { refused = true; }
            require(refused, "baseline refuses writes after owner shutdown");
        }
        {
            DeferredSettings writes(1h, true);
            std::vector<int> saved;
            int completed = 0;
            for (int value = 1; value <= 50; ++value) {
                writes.submit(Key::Video, [&, value] {
                    require(std::this_thread::get_id() != owner, "disk work runs off the owner thread");
                    saved.push_back(value);
                }, [&](bool ok) {
                    require(std::this_thread::get_id() == owner, "completion runs on owner thread");
                    require(ok, "latest coalesced preference saved");
                    ++completed;
                });
            }
            writes.submit(Key::Rumble, [&] { saved.push_back(99); }, [&](bool ok) {
                require(ok, "independent preference also saved"); ++completed;
            });
            require(saved.empty(), "debounce postpones all filesystem work");
            writes.flush();
            require(saved == std::vector<int>({50, 99}), "rapid edits coalesce per preference without losing another key");
            require(completed == 2, "only latest revision of each key reports completion");
        }
        {
            DeferredSettings writes(0ms, true);
            std::mutex mutex;
            std::condition_variable changed;
            bool started = false, release = false;
            int staleCompletions = 0, latestCompletions = 0, latestValue = 0;
            writes.submit(Key::Video, [&] {
                std::unique_lock<std::mutex> lock(mutex);
                started = true;
                changed.notify_one();
                changed.wait(lock, [&] { return release; });
                throw std::runtime_error("old disk operation failed");
            }, [&](bool) { ++staleCompletions; });
            bool observed;
            {
                std::unique_lock<std::mutex> lock(mutex);
                observed = changed.wait_for(lock, 2s, [&] { return started; });
            }
            // These submissions complete while the first filesystem call is
            // blocked. Waiting for disk here would deadlock the test.
            writes.submit(Key::Video, [&] { latestValue = 2; }, [&](bool) { ++staleCompletions; });
            writes.submit(Key::Video, [&] { latestValue = 3; }, [&](bool ok) {
                require(ok, "newer write succeeds after stale failure"); ++latestCompletions;
            });
            {
                std::lock_guard<std::mutex> lock(mutex);
                release = true;
            }
            changed.notify_one();
            writes.flush();
            require(observed, "slow filesystem operation started on worker");
            require(latestValue == 3 && staleCompletions == 0 && latestCompletions == 1,
                "in-flight old failure cannot overwrite or misreport newer preferences");
        }
        {
            DeferredSettings writes(1h, true);
            bool completed = false, reportedSaved = true;
            writes.submit(Key::Connection, [] { throw std::runtime_error("synthetic storage error"); },
                [&](bool saved) { completed = true; reportedSaved = saved; });
            writes.finish();
            require(completed && !reportedSaved, "shutdown bypasses debounce and exposes final write failure");
            writes.finish();
        }
        {
            bool completed = false;
            int disk = 0;
            {
                DeferredSettings writes(1h, true);
                writes.submit(Key::Controller, [&] { disk = 42; }, [&](bool saved) { completed = saved; });
            }
            require(disk == 42 && completed, "destruction drains queued immutable snapshot and reports it");
        }
        {
            bool laterCompletion = false;
            {
                DeferredSettings writes(1h, true);
                writes.submit(Key::Video, [] {}, [](bool) { throw std::runtime_error("UI status failed"); });
                writes.submit(Key::Rumble, [] {}, [&](bool saved) { laterCompletion = saved; });
            }
            require(laterCompletion, "throwing optional status cannot terminate shutdown or suppress other completions");
        }
        std::cout << "Deferred settings: " << assertions << " assertions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Deferred settings failed: " << error.what() << '\n';
        return 1;
    }
}
