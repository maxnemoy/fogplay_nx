#pragma once
#include <rtc/peerconnection.hpp>
#include <string>

namespace stream {
inline std::string dtlsDiagnosticText(const rtc::DtlsDiagnostics& d) {
    const auto average = [](std::uint64_t total, std::uint64_t count) { return count ? total / count : 0; };
    return "available=" + std::to_string(d.available) + " scope=transport-lifetime" +
        " queueDepth=" + std::to_string(d.queueDepth) + " queueHigh=" + std::to_string(d.queueHighWater) +
        " oldestAgeUs=" + std::to_string(d.queueOldestAgeUs) +
        " enqueued=" + std::to_string(d.enqueued) + " dequeued=" + std::to_string(d.dequeued) +
        " residenceAvgUs=" + std::to_string(average(d.residenceTotalUs, d.dequeued)) +
        " residenceMaxUs=" + std::to_string(d.residenceMaxUs) +
        " fullWaitCount=" + std::to_string(d.fullPushWaitCount) +
        " fullWaitTotalUs=" + std::to_string(d.fullPushWaitTotalUs) +
        " fullWaitMaxUs=" + std::to_string(d.fullPushWaitMaxUs) +
        " recvSchedules=" + std::to_string(d.recvScheduleCount) +
        " scheduleWaitAvgUs=" + std::to_string(average(d.recvScheduleWaitTotalUs, d.recvScheduleCount)) +
        " scheduleWaitMaxUs=" + std::to_string(d.recvScheduleWaitMaxUs) +
        " recvRuns=" + std::to_string(d.recvRuns) +
        " recvWorkAvgUs=" + std::to_string(average(d.recvProcessingTotalUs, d.recvRuns)) +
        " recvWorkMaxUs=" + std::to_string(d.recvProcessingMaxUs);
}
}
