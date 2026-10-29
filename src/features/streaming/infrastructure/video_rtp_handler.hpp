#pragma once
#include "core/diagnostics/latency_histogram.hpp"
#include <rtc/rtc.hpp>
#include <functional>
#include <utility>

namespace stream {
// Keep the former chain's order (RTX normalization -> RTCP -> assembly) inside
// one ingress scope, so receipt is captured before either Shared mutex wait.
class VideoRtpHandler final : public rtc::MediaHandler {
public:
    using Normalize = std::function<bool(rtc::binary&, bool)>;
    using Receive = std::function<void(const std::byte*, std::size_t, std::uint64_t,
                                      const rtc::message_callback&)>;
    VideoRtpHandler(std::shared_ptr<rtc::MediaHandler> receiver, Normalize normalize, Receive receive)
        : receiver_(std::move(receiver)), normalize_(std::move(normalize)), receive_(std::move(receive)) {}
    bool requestKeyframe(const rtc::message_callback& send) override {
        return receiver_->requestKeyframe(send);
    }
    bool requestBitrate(unsigned int bitrate, const rtc::message_callback& send) override {
        return receiver_->requestBitrate(bitrate, send);
    }
    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override {
        const auto receivedMs = fog::metrics::steadyNowUs() / 1000;
        rtc::message_vector accepted;
        for (auto& message : messages) {
            if (message->type == rtc::Message::Control) {
                if (normalize_(*message, true)) accepted.push_back(std::move(message));
                continue;
            }
            if (message->type != rtc::Message::Binary) continue;
            if (normalize_(*message, false)) {
                message->stream = (std::to_integer<unsigned>(message->at(8)) << 24) |
                    (std::to_integer<unsigned>(message->at(9)) << 16) |
                    (std::to_integer<unsigned>(message->at(10)) << 8) | std::to_integer<unsigned>(message->at(11));
                accepted.push_back(std::move(message));
            }
        }
        messages.swap(accepted);
        receiver_->incomingChain(messages, send);
        for (const auto& message : messages)
            if (message->type == rtc::Message::Binary)
                receive_(message->data(), message->size(), receivedMs, send);
        messages.clear();
    }
private:
    std::shared_ptr<rtc::MediaHandler> receiver_;
    Normalize normalize_;
    Receive receive_;
};
}
