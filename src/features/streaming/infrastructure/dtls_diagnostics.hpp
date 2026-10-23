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
        " recvWorkMaxUs=" + std::to_string(d.recvProcessingMaxUs) +
        " sendSslLockCount=" + std::to_string(d.sendSslLockCount) +
        " sendSslWaitAvgUs=" + std::to_string(average(d.sendSslLockWaitTotalUs, d.sendSslLockCount)) +
        " sendSslWaitMaxUs=" + std::to_string(d.sendSslLockWaitMaxUs) +
        " sendSslHoldAvgUs=" + std::to_string(average(d.sendSslLockHoldTotalUs, d.sendSslLockCount)) +
        " sendSslHoldMaxUs=" + std::to_string(d.sendSslLockHoldMaxUs) +
        " recvSslLockCount=" + std::to_string(d.recvSslLockCount) +
        " recvSslWaitAvgUs=" + std::to_string(average(d.recvSslLockWaitTotalUs, d.recvSslLockCount)) +
        " recvSslWaitMaxUs=" + std::to_string(d.recvSslLockWaitMaxUs) +
        " recvSslHoldAvgUs=" + std::to_string(average(d.recvSslLockHoldTotalUs, d.recvSslLockCount)) +
        " recvSslHoldMaxUs=" + std::to_string(d.recvSslLockHoldMaxUs) +
        " recvBatchPackets=" + std::to_string(d.recvBatchPacketsTotal) +
        " recvBatchMax=" + std::to_string(d.recvBatchPacketsMax) +
        " recvBatchYields=" + std::to_string(d.recvBatchYields) +
        " recvBatchTimeYields=" + std::to_string(d.recvBatchTimeYields) +
        " recvBatchWorkYields=" + std::to_string(d.recvBatchWorkYields) +
        " recvBatchEmpty=" + std::to_string(d.recvBatchEmpty) +
        " admissionMediaAccepted=" + std::to_string(d.admissionMediaAccepted) +
        " admissionControlAccepted=" + std::to_string(d.admissionControlAccepted) +
        " admissionMediaDropped=" + std::to_string(d.admissionMediaDropped) +
        " admissionMediaEvicted=" + std::to_string(d.admissionMediaEvicted) +
        " admissionInvalid=" + std::to_string(d.admissionInvalid) +
        " admissionStopped=" + std::to_string(d.admissionStopped) +
        " admissionCriticalOverflow=" + std::to_string(d.admissionCriticalOverflow) +
        " packetTiming=wall-us-nested" +
        " demuxCount=" + std::to_string(d.recvDemuxCount) +
        " demuxTotalUs=" + std::to_string(d.recvDemuxTotalUs) +
        " demuxMaxUs=" + std::to_string(d.recvDemuxMaxUs) +
        " unprotectCount=" + std::to_string(d.recvUnprotectCount) +
        " unprotectTotalUs=" + std::to_string(d.recvUnprotectTotalUs) +
        " unprotectMaxUs=" + std::to_string(d.recvUnprotectMaxUs) +
        " mediaCallbackCount=" + std::to_string(d.recvMediaCallbackCount) +
        " mediaCallbackTotalUs=" + std::to_string(d.recvMediaCallbackTotalUs) +
        " mediaCallbackMaxUs=" + std::to_string(d.recvMediaCallbackMaxUs) +
        " dataCallbackCount=" + std::to_string(d.recvDataCallbackCount) +
        " dataCallbackTotalUs=" + std::to_string(d.recvDataCallbackTotalUs) +
        " dataCallbackMaxUs=" + std::to_string(d.recvDataCallbackMaxUs) +
        " recvRtpCallbackCount=" + std::to_string(d.recvRtpCallbackCount) +
        " recvRtpCallbackTotalUs=" + std::to_string(d.recvRtpCallbackTotalUs) +
        " recvRtpCallbackMaxUs=" + std::to_string(d.recvRtpCallbackMaxUs) +
        " recvRtcpCallbackCount=" + std::to_string(d.recvRtcpCallbackCount) +
        " recvRtcpCallbackTotalUs=" + std::to_string(d.recvRtcpCallbackTotalUs) +
        " recvRtcpCallbackMaxUs=" + std::to_string(d.recvRtcpCallbackMaxUs) +
        " sendSrtpLockCount=" + std::to_string(d.sendSrtpLockCount) +
        " sendSrtpLockWaitTotalUs=" + std::to_string(d.sendSrtpLockWaitTotalUs) +
        " sendSrtpLockWaitMaxUs=" + std::to_string(d.sendSrtpLockWaitMaxUs) +
        " sendSrtpLockHoldTotalUs=" + std::to_string(d.sendSrtpLockHoldTotalUs) +
        " sendSrtpLockHoldMaxUs=" + std::to_string(d.sendSrtpLockHoldMaxUs) +
        " sendSrtpRejected=" + std::to_string(d.sendSrtpRejected) +
        " sendSrtpFailed=" + std::to_string(d.sendSrtpFailed) +
        " sendRtpCount=" + std::to_string(d.sendRtpCount) +
        " sendRtcpCount=" + std::to_string(d.sendRtcpCount) +
        " sendRtpProtectCount=" + std::to_string(d.sendRtpProtectCount) +
        " sendRtpProtectTotalUs=" + std::to_string(d.sendRtpProtectTotalUs) +
        " sendRtpProtectMaxUs=" + std::to_string(d.sendRtpProtectMaxUs) +
        " sendRtcpProtectCount=" + std::to_string(d.sendRtcpProtectCount) +
        " sendRtcpProtectTotalUs=" + std::to_string(d.sendRtcpProtectTotalUs) +
        " sendRtcpProtectMaxUs=" + std::to_string(d.sendRtcpProtectMaxUs) +
        " sendRtpLowerCount=" + std::to_string(d.sendRtpLowerCount) +
        " sendRtpLowerTotalUs=" + std::to_string(d.sendRtpLowerTotalUs) +
        " sendRtpLowerMaxUs=" + std::to_string(d.sendRtpLowerMaxUs) +
        " sendRtcpLowerCount=" + std::to_string(d.sendRtcpLowerCount) +
        " sendRtcpLowerTotalUs=" + std::to_string(d.sendRtcpLowerTotalUs) +
        " sendRtcpLowerMaxUs=" + std::to_string(d.sendRtcpLowerMaxUs);
}
}
