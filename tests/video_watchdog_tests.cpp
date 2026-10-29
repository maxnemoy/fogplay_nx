#include "features/streaming/infrastructure/video_watchdog.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

using fog::detail::VideoFlight;
using fog::detail::VideoWatchdog;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        VideoWatchdog watchdog;
        std::vector<std::string> records;
        const auto report = [&](const std::string& value) { records.push_back(value); };
        const VideoFlight::Metadata metadata{1280, 720, 1, 2, 1280, 12345, 96, 321};
        watchdog.decoder.begin(VideoFlight::Stage::DecodeReceive, metadata, 1000);
        watchdog.inspect(1499, report);
        require(records.empty(), "short calls must not report");
        watchdog.inspect(1500, report);
        require(records.size() == 1 && records.back().find("stage=decode-receive") != std::string::npos &&
            records.back().find("bytes=12345 nalTypes=96 frame=321") != std::string::npos, "stalled operation metadata");
        watchdog.presentation.begin(VideoFlight::Stage::Present, metadata, 1000);
        watchdog.inspect(1501, report);
        require(records.size() == 2 && records.back().find("lane=presentation") != std::string::npos, "independent lanes");
        watchdog.presentation.end();
        watchdog.inspect(6499, report);
        require(records.size() == 2, "repeat rate limited");
        for (auto at : {6500u, 11500u, 16500u, 21500u}) watchdog.inspect(at, report);
        require(records.size() == 5, "four records maximum per stuck call");
        watchdog.decoder.end();
        watchdog.inspect(30000, report);
        require(records.size() == 5, "completed operations no longer reported");
        for (unsigned i = 0; i < 200; ++i) {
            watchdog.decoder.begin(VideoFlight::Stage::Transfer, metadata, 1000);
            watchdog.inspect(1500, report);
        }
        require(records.size() == 128, "session diagnostic output bounded");
        watchdog.decoder.end();

        VideoWatchdog threaded;
        std::mutex mutex;
        std::condition_variable ready;
        unsigned delivered = 0;
        threaded.start([&](const std::string& value) {
            std::lock_guard<std::mutex> lock(mutex);
            if (value.find("stage=texture-upload") != std::string::npos) ++delivered;
            ready.notify_all();
        });
        {
            VideoFlight::Scope blocked(threaded.presentation, VideoFlight::Stage::TextureUpload, metadata);
            std::unique_lock<std::mutex> lock(mutex);
            require(ready.wait_for(lock, std::chrono::seconds(3), [&] { return delivered != 0; }),
                "observer records a blocked owner without owner progress");
        }
        threaded.stop();
        VideoFlight::Snapshot snapshot;
        require(threaded.presentation.read(snapshot) && snapshot.stage == VideoFlight::Stage::Idle, "scope retires operation");
        std::cout << "video watchdog tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
