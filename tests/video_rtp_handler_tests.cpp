#include "features/streaming/infrastructure/video_rtp_handler.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

struct Receiver final : rtc::MediaHandler {
    std::vector<int>& order;
    unsigned keyframes = 0, bitrate = 0;
    explicit Receiver(std::vector<int>& value) : order(value) {}
    void incoming(rtc::message_vector& messages, const rtc::message_callback&) override {
        order.push_back(4);
        assert(messages.size() == 3);
        assert(messages[0]->type == rtc::Message::Binary && messages[0]->stream == 42);
        assert(messages[1]->type == rtc::Message::Control);
        assert(messages[2]->type == rtc::Message::Binary && messages[2]->stream == 42);
        messages.erase(messages.begin() + 1); // RTCP consumed before assembly.
    }
    bool requestKeyframe(const rtc::message_callback&) override { ++keyframes; return true; }
    bool requestBitrate(unsigned value, const rtc::message_callback&) override { bitrate = value; return true; }
};

int main() {
    std::vector<int> order;
    auto receiver = std::make_shared<Receiver>(order);
    std::uint64_t normalizeEnteredMs = 0, normalizationDoneMs = 0, firstReceiptMs = 0;
    unsigned delivered = 0;
    stream::VideoRtpHandler handler(receiver,
        [&](rtc::binary& message, bool control) {
            if (order.empty()) {
                normalizeEnteredMs = fog::metrics::steadyNowUs() / 1000;
                std::this_thread::sleep_for(std::chrono::milliseconds(25)); // Contended Shared normalizer.
                normalizationDoneMs = fog::metrics::steadyNowUs() / 1000;
            }
            const auto tag = std::to_integer<unsigned>(message[0]);
            order.push_back(control ? 2 : tag == 3 ? 3 : 1);
            if (tag == 9) return false;
            if (!control) message[11] = std::byte{42};
            return true;
        },
        [&](const std::byte*, std::size_t size, std::uint64_t receiptMs, const rtc::message_callback&) {
            assert(size == 12);
            order.push_back(5);
            assert(receiptMs <= normalizeEnteredMs && normalizationDoneMs - receiptMs >= 20);
            if (delivered++) assert(receiptMs == firstReceiptMs);
            firstReceiptMs = receiptMs;
        });
    rtc::message_vector messages;
    for (auto tag : {1, 2, 3, 9, 7}) {
        auto message = rtc::make_message(12, tag == 2 ? rtc::Message::Control : tag == 7 ? rtc::Message::String : rtc::Message::Binary);
        (*message)[0] = std::byte(tag);
        messages.push_back(std::move(message));
    }
    handler.incomingChain(messages, [](auto) {});
    assert((order == std::vector<int>{1, 2, 3, 1, 4, 5, 5}));
    assert(delivered == 2 && messages.empty());
    assert(handler.requestKeyframe([](auto) {}) && receiver->keyframes == 1);
    assert(handler.requestBitrate(123456, [](auto) {}) && receiver->bitrate == 123456);
    std::cout << "Video ingress: pre-normalization receipt, batch RTX/RTCP/assembly order, filtering and PLI/REMB forwarding passed\n";
}
