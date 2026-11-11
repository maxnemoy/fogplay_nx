#pragma once
#include "features/streaming/domain/media_telemetry.hpp"
#include <mutex>
#include <optional>

namespace fog::detail {
// Each source is built on its existing recording owner before publication.
// This mutex protects only bounded value copies; no SDK/query/format/allocation
// is performed under it and no reference escapes a read.
class MediaTelemetryCache {
public:
    using Video=media::telemetry::VideoOwner;
    using Audio=media::telemetry::AudioOwner;
    struct Owners { Video video;Audio audio; };
    void publish(Video value) { std::lock_guard<std::mutex> lock(mutex_);owners_.video=value; }
    void publish(Audio value) { std::lock_guard<std::mutex> lock(mutex_);owners_.audio=value; }
    Owners snapshot() const { std::lock_guard<std::mutex> lock(mutex_);return owners_; }
    void retainFinal(media::telemetry::Snapshot value) {std::lock_guard<std::mutex> lock(mutex_);final_=value;}
    std::optional<media::telemetry::Snapshot> retainedFinal() const {std::lock_guard<std::mutex> lock(mutex_);return final_;}
private:
    mutable std::mutex mutex_;
    Owners owners_;
    std::optional<media::telemetry::Snapshot> final_;
};
}
