#include "features/streaming/infrastructure/media_decoder_diagnostics.hpp"
#include <cassert>
#include <cstdarg>
#include <iostream>
#include <thread>
#include <vector>
using fog::detail::DecoderDiagnosticCapture;
void emit(DecoderDiagnosticCapture& capture, void* context, const char* format, ...) {
    va_list args;
    va_start(args, format);
    capture.capture(context, format, args);
    va_end(args);
}
int main() {
    DecoderDiagnosticCapture capture;
    int decoder = 0, unrelated = 0;
    capture.registerContext(&decoder);
    emit(capture, &decoder, "input URL=%s", "secret payload");
    emit(capture, &decoder, "FOG_NVDEC_STATUS_V1 error_status=%u mbs_error=%u mbs_ok=%u cycles_raw=%u slice_header_error=%u\n extra", 1u, 2u, 3u, 4u, 5u);
    emit(capture, &unrelated, fog::detail::nvdecStatusFormat, 1u, 2u, 3u, 4u, 5u);
    assert(capture.snapshot(&decoder).events == 0);
    emit(capture, &decoder, fog::detail::nvdecStatusFormat, 0xffffffffu, 2u, 8100u, 0x80000000u, 7u);
    auto status = capture.snapshot(&decoder);
    assert(status.events == 1 && status.hasStatus && status.error == 0xffffffffu);
    assert(status.badBlocks == 2 && status.goodBlocks == 8100 && status.cycles == 0x80000000u && status.headerError == 7);
    emit(capture, &decoder, fog::detail::nvdecWaitFormat, -12);
    status = capture.snapshot(&decoder);
    assert(status.events == 2 && !status.hasStatus && status.waitCode == -12 && status.error == 0 && status.goodBlocks == 0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) threads.emplace_back([&] {
        for (int j = 0; j < 10000; ++j) {
            emit(capture, &decoder, fog::detail::nvdecStatusFormat, 1u, 2u, 3u, 4u, 5u);
            const auto sample = capture.snapshot(&decoder);
            assert(sample.hasStatus && sample.error == 1 && sample.badBlocks == 2 && sample.goodBlocks == 3 && sample.cycles == 4 && sample.headerError == 5);
        }
    });
    for (auto& thread : threads) thread.join();
    assert(capture.snapshot(&decoder).events == 40002);
    capture.unregisterContext(&decoder);
    emit(capture, &decoder, fog::detail::nvdecWaitFormat, -99);
    assert(capture.snapshot(&decoder).events == 0);
    capture.registerContext(&decoder);
    assert(capture.snapshot(&decoder).events == 0);
    emit(capture, nullptr, fog::detail::nvdecStatusFormat, 1u, 2u, 3u, 4u, 5u);
    emit(capture, &decoder, nullptr);
    assert(capture.snapshot(&decoder).events == 0);
    std::cout << "Decoder numeric whitelist, context isolation, lifecycle and concurrency passed\n";
}
