#pragma once
#include "features/streaming/domain/network_snapshot.hpp"
#include <algorithm>
#include <cstdio>
#include <string>
namespace stream::telemetry::network {
inline const char* lossProtectionName(unsigned mask){
    static constexpr const char* names[]={"none","red","ulpfec","red,ulpfec","flexfec","flexfec,red","flexfec,ulpfec","flexfec,red,ulpfec"};
    return mask<8?names[mask]:"unknown";
}
inline const char* scalePendingName(unsigned reason){
    static constexpr const char* names[]={"no-target","duplicate-last","limit","ready"};
    return reason<4?names[reason]:"unknown";
}
inline std::string rtpText(const RtpSnapshot& s){
        return "nackEnabled=" + std::to_string(s.nackEnabled) + " nackPackets=" + std::to_string(s.nackPackets) +
            " repairedPackets=" + std::to_string(s.repairedPackets) + " repairWindowMs=" + std::to_string(s.repairWindowMs) +
            " gapRepairWindowMs=" + std::to_string(s.gapRepairWindowMs) +
            " nackRetryMs=" + std::to_string(s.nackRetryMs) +
            " nackArrivalSamples=" + std::to_string(s.arrivalSamples) + " nackLateSamples=" + std::to_string(s.lateSamples) +
            " nackRetrySamples=" + std::to_string(s.retrySamples) +
            " nackFirstArrivalLastMs=" + std::to_string(s.arrivalLastMs) +
            " nackFirstArrivalMeanMs=" + std::to_string(s.arrivalMeanMs) + " nackFirstArrivalMaxMs=" + std::to_string(s.arrivalMaxMs) +
            " nackArrivalDeviationMs=" + std::to_string(s.arrivalDeviationMs) +
            " nackArrivalP50Ms=" + std::to_string(s.arrivalP50Ms) +
            " nackArrivalP95Ms=" + std::to_string(s.arrivalP95Ms) +
            " nackArrivalBucketMs=" + std::to_string(s.arrivalBucketMs) +
            " nackRoundTripMs=" + std::to_string(s.roundTripMs) +
            " nackArrivalBeforeRtt=" + std::to_string(s.arrivalBeforeRtt) +
            " nackArrivalAfterRtt=" + std::to_string(s.arrivalAfterRtt) +
            " nackArrivalRttUnknown=" + std::to_string(s.arrivalRttUnknown) +
            " nackHistory=" + std::to_string(s.history) + " nackHistoryEvicted=" + std::to_string(s.historyEvicted) +
            " catchupYields=" + std::to_string(s.catchupYields) +
            " rtp=" + std::to_string(s.counters.received) +
            " gapCandidates=" + std::to_string(s.counters.candidates) +
            " reordered=" + std::to_string(s.counters.reordered) +
            " duplicate=" + std::to_string(s.counters.duplicate) +
            " late=" + std::to_string(s.counters.late) +
            " gaps=" + std::to_string(s.counters.gaps) + " missing=" + std::to_string(s.counters.missing) +
            " malformed=" + std::to_string(s.counters.malformed) +
            " foreignSsrc=" + std::to_string(s.counters.foreignSsrc) + " padding=" + std::to_string(s.counters.padding) +
            " auDrop=" + std::to_string(s.counters.dropped) + " auOut=" + std::to_string(s.counters.emitted) +
            " resync=" + std::to_string(s.counters.resyncs) + " recovered=" + std::to_string(s.counters.recovered) +
            " pressure=" + std::to_string(s.counters.pressure) + " timeout=" + std::to_string(s.counters.timeout) +
            " waitIdr=" + std::to_string(s.waiting) + " pending=" + std::to_string(s.pendingPackets) +
            " pendingBytes=" + std::to_string(s.pendingBytes) + " auBytes=" + std::to_string(s.assemblyBytes) +
            " nextSeq=" + std::to_string(s.expectedSequence) + " lastTs=" + std::to_string(s.lastTimestamp) +
            " lastFirstSeq=" + std::to_string(s.lastFirstSequence) + " lastLastSeq=" + std::to_string(s.lastLastSequence) +
            " eventDrop=" + std::to_string(s.counters.eventOverflow) +
            " expiredGaps=" + std::to_string(s.expiredGaps) + " expiredPackets=" + std::to_string(s.expiredPackets) +
            " expiredNoNackGaps=" + std::to_string(s.expiredNoNackGaps) +
            " lastGapFirst=" + std::to_string(s.lastExpiry.first) + " lastGapLast=" + std::to_string(s.lastExpiry.last) +
            " lastGapDeadlineMs=" + std::to_string(s.lastExpiry.deadlineMs);
    }
inline std::string gapText(const RtpSnapshot& s){
        return "expiredGaps=" + std::to_string(s.expiredGaps) + " expiredPackets=" + std::to_string(s.expiredPackets) +
            " expiredNoNackGaps=" + std::to_string(s.expiredNoNackGaps) +
            " expiredUnrequestedPackets=" + std::to_string(s.expiredUnrequestedPackets) +
            " expiredNackAttempts=" + std::to_string(s.expiredNackAttempts) +
            " maxExpiryOvershootMs=" + std::to_string(s.maxExpiryOvershootMs) +
            " matchedLatePackets=" + std::to_string(s.matchedLatePackets) +
            " lastFirst=" + std::to_string(s.lastExpiry.first) + " lastLast=" + std::to_string(s.lastExpiry.last) +
            " lastDetectedMs=" + std::to_string(s.lastExpiry.detectedMs) +
            " lastOldestEvidenceMs=" + std::to_string(s.lastExpiry.oldestMs) +
            " lastBudgetMs=" + std::to_string(s.lastExpiry.budgetMs) +
            " lastDeadlineMs=" + std::to_string(s.lastExpiry.deadlineMs) +
            " lastExpiredMs=" + std::to_string(s.lastExpiry.expiredMs) +
            " lastOvershootMs=" + std::to_string(s.lastExpiry.overshootMs) +
            " lastFirstNackMs=" + std::to_string(s.lastExpiry.firstNackMs) +
            " lastLastNackMs=" + std::to_string(s.lastExpiry.lastNackMs) +
            " lastNackAttempts=" + std::to_string(s.lastExpiry.nackAttempts) +
            " lastRequestedPackets=" + std::to_string(s.lastExpiry.requestedPackets) +
            " lastPendingFirst=" + std::to_string(s.lastExpiry.pendingFirst) +
            " lastPendingLast=" + std::to_string(s.lastExpiry.pendingLast) +
            " lastPendingDepth=" + std::to_string(s.lastExpiry.pendingDepth) +
            " lateSeq=" + std::to_string(s.lastLate.sequence) + " lateArrivedMs=" + std::to_string(s.lastLate.arrivedMs) +
            " lateGapFirst=" + std::to_string(s.lastLate.first) + " lateGapLast=" + std::to_string(s.lastLate.last) +
            " lateDeadlineMs=" + std::to_string(s.lastLate.deadlineMs) + " lateByMs=" + std::to_string(s.lastLate.lateByMs) +
            " lateFirstNackMs=" + std::to_string(s.lastLate.firstNackMs) +
            " lateLastNackMs=" + std::to_string(s.lastLate.lastNackMs) +
            " lateNackAttempts=" + std::to_string(s.lastLate.nackAttempts);
    }
inline std::string autoQualityText(const quality::NumericSnapshot& s){
    static constexpr const char* pauseNames[]={"none","loss","superseded","audio","other"};
        char out[1280];
        std::snprintf(out, sizeof(out), " autoEnabled=%d autoReason=%s autoPending=%d autoTargetFps=%d sampleFps=%.2f decodedFps=%.2f videoWorkMs=%.2f videoBusy=%.3f autoUpAttempts=%u autoDownAttempts=%u autoDowngrades=%u autoNextUpMs=%llu autoQueueWindows=%u autoDeliveryWindows=%u autoAudioWindows=%u autoCapCoalesced=%llu autoGoodMs=%llu autoEvidenceMs=%llu autoPaused=%d workerVideo=%d uiVideoMs=%.2f decoderVideoMs=%.2f uiVideoBusy=%.3f decoderVideoBusy=%.3f"
            " autoPauseCause=%s autoPauseLoss=%llu autoPauseSuperseded=%llu autoPauseAudio=%llu autoPauseOther=%llu"
            " autoRejectLoss=%llu autoRejectSuperseded=%llu autoRejectAudio=%llu autoRejectLoad=%llu autoRejectDelivery=%llu",
            s.enabled, quality::diagnosticReasonName(s.reason), s.pending, s.targetFps, s.sampleFps, s.decodedFps, s.videoWorkMs, s.videoBusy, s.upAttempts, s.downAttempts, s.downgrades,
            static_cast<unsigned long long>(s.nextUpMs), s.queueWindows, s.deliveryWindows, s.audioWindows, static_cast<unsigned long long>(s.capCoalesced),
            static_cast<unsigned long long>(s.goodMs), static_cast<unsigned long long>(s.evidenceMs), s.paused,
            s.workerVideo, s.uiVideoMs, s.decoderVideoMs, s.uiVideoBusy, s.decoderVideoBusy, pauseNames[s.pauseCause<5?s.pauseCause:0],
            static_cast<unsigned long long>(s.pauseCounts[1]),
            static_cast<unsigned long long>(s.pauseCounts[2]),
            static_cast<unsigned long long>(s.pauseCounts[3]),
            static_cast<unsigned long long>(s.pauseCounts[4]),
            static_cast<unsigned long long>(s.rejectLoss), static_cast<unsigned long long>(s.rejectSuperseded),
            static_cast<unsigned long long>(s.rejectAudio), static_cast<unsigned long long>(s.rejectLoad),
            static_cast<unsigned long long>(s.rejectDelivery));
        return out;
    }
inline std::string qualityText(const QualitySnapshot& s){
    static constexpr const char* profileNames[]={"unknown","LOW","MID","HIGH","ULTRA"};
        char numbers[650];
        std::snprintf(numbers, sizeof(numbers),
            " codec=%s requestedCodec=%s capMbps=%d quality=%d pliSeconds=%d requestedFps=%d receiveMbps=%.3f serverTargetMbps=%.3f targetAgeMs=%lld serverPreset=%s presetAgeMs=%lld statsCount=%llu statsAgeMs=%lld serverAuto=%d autoAgeMs=%lld serverFps=%.2f fpsAgeMs=%lld serverCodec=%s codecAgeMs=%lld qualityRequests=%u qualityPending=%d",
            s.codecH265 ? "H265" : "H264",
            s.requestedH265 ? "H265" : "H264", s.capMbps,
            s.quality, s.pliSeconds, s.requestedFps, s.receiveMbps,
            s.targetMbps,
            static_cast<long long>(s.targetAgeMs), profileNames[s.profile>=0&&s.profile<5?s.profile:0],
            static_cast<long long>(s.presetAgeMs), static_cast<unsigned long long>(s.statsCount),
            static_cast<long long>(s.statsAgeMs),
            s.automatic,
            static_cast<long long>(s.autoAgeMs), s.fps,
            static_cast<long long>(s.fpsAgeMs), s.serverCodec==2?"H265":s.serverCodec==1?"H264":"unknown",
            static_cast<long long>(s.codecAgeMs), static_cast<unsigned>(s.qualityRequests), s.qualityPending ? 1 : 0);
        return std::string(numbers) + autoQualityText(s.automaticPolicy) +
            " offerLossProtection=" + lossProtectionName(s.offeredLossProtection) +
            " scaleTarget=1280x720 scaleAttempts=" + std::to_string(s.scaleAttempts) +
            " scaleQueued=" + std::to_string(s.scaleQueued) +
            " scaleRejected=" + std::to_string(s.scaleRejected) +
            " scaleInvalidTables=" + std::to_string(s.scaleInvalidTables)+
            " scaleTableRevision="+std::to_string(s.scaleTableRevision)+" scaleTableEntries="+std::to_string(s.scaleTableEntries)+
            " scaleNativeWidth="+std::to_string(s.scaleNativeWidth)+" scaleNativeHeight="+std::to_string(s.scaleNativeHeight)+
            " scalePreset="+std::to_string(s.scalePreset)+" scaleLastPreset="+std::to_string(s.scaleLastPreset)+
            " scalePending="+scalePendingName(s.scalePendingReason)+" scaleActual=observe-decoded-video";
    }
inline std::string performanceText(const PerformanceSnapshot& s){
        const auto now=s.capturedAtMs;
        static constexpr std::array<const char*,11> keys={"cpu_usage","gpu_usage","ping","encode","encodefull","grab","serverfps","serverqueuefps","packetize","sendsamples_video","sendsamples_audio"};
        std::string result = "serverPerf=reported-raw freshnessMs=" + std::to_string(s.staleAfterMs);
        result.reserve(1600);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const auto& field = s.fields[i];
            const auto* key = keys[i];
            char value[32] = "unknown", age[32] = "unknown", entry[160];
            if (field.known) std::snprintf(value, sizeof(value), "%.9g", field.value);
            const bool ageKnown = field.known && now >= field.sampledAtMs;
            const auto elapsed = ageKnown ? now - field.sampledAtMs : 0;
            if (ageKnown) std::snprintf(age, sizeof(age), "%llu", static_cast<unsigned long long>(elapsed));
            const bool stale = !ageKnown || elapsed > s.staleAfterMs;
            const int written = std::snprintf(entry, sizeof(entry), " %sKnown=%d %sRaw=%s %sAgeMs=%s %sStale=%d",
                key, field.known ? 1 : 0, key, value, key, age, key, stale ? 1 : 0);
            if (written > 0) result.append(entry, std::min({static_cast<std::size_t>(written),
                sizeof(entry) - 1, 1600 - result.size()}));
        }
        return result;
    }
inline std::string fhTimingText(const StageTiming& t,const char* name){return std::string(name)+"_n="+std::to_string(t.calls)+" avg_us="+std::to_string(t.calls?t.totalUs/t.calls:0)+" max_us="+std::to_string(t.maxUs)+" over50ms="+std::to_string(t.over50ms);}
inline std::string fhVideoText(const FhSnapshot& f){        return "Moonlight " + std::string(f.codecH265?"H.265":"H.264") + " cumulative units=" + std::to_string(f.units) +
            " encoded_payload_bytes=" + std::to_string(f.payloadBytes) + " idr=" + std::to_string(f.idrs) +
            " skipped_frame_numbers=" + std::to_string(f.skippedFrameNumbers) +
            " repeated_or_reordered=" + std::to_string(f.repeatedOrReordered) +
            " invalid_times=" + std::to_string(f.invalidTimes) +
            " last_unit_age_us=" + std::to_string(f.lastUnitAgeUs) +
            " | " + fhTimingText(f.videoTimings[0],"assembly") + " | " + fhTimingText(f.videoTimings[1],"core_queue_age") +
            " | " + fhTimingText(f.videoTimings[2],"unit_gap") + " | " + fhTimingText(f.videoTimings[3],"host_processing");
    }
inline std::string fhGapText(const FhSnapshot& f){const auto& core=f.core;const auto now=f.coreNowUs;        return "Moonlight core cumulative available=" + std::to_string(true) +
            " udp_video_packets=" + std::to_string(core.videoPacketsReceived) +
            " udp_video_bytes=" + std::to_string(core.videoBytesReceived) +
            " receive_errors=" + std::to_string(core.videoReceiveErrors) +
            " decrypt_failures=" + std::to_string(core.videoDecryptFailures) +
            " runt_packets=" + std::to_string(core.videoRuntPackets) +
            " last_packet_age_us=" + std::to_string(core.lastVideoPacketUs && now >= core.lastVideoPacketUs ? now - core.lastVideoPacketUs : 0) +
            " packet_gap_max_us=" + std::to_string(core.maxVideoPacketGapUs) +
            " rtp_processing_n=" + std::to_string(core.videoPacketProcessingCount) +
            " rtp_processing_avg_us=" + std::to_string(core.videoPacketProcessingCount ? core.videoPacketProcessingTotalUs / core.videoPacketProcessingCount : 0) +
            " rtp_processing_max_us=" + std::to_string(core.videoPacketProcessingMaxUs) +
            " rtp_processing_invalid_times=" + std::to_string(core.videoPacketProcessingInvalidTimes) +
            " fec_blocks_complete=" + std::to_string(core.fecBlocksCompleted) +
            " fec_blocks_failed=" + std::to_string(core.fecBlocksFailed) +
            " fec_packets_recovered=" + std::to_string(core.fecPacketsRecovered) +
            " fec_reconstruction_failures=" + std::to_string(core.fecReconstructionFailures) +
            " rtp_reject_seq=" + std::to_string(core.videoPacketsRejectedSequence) +
            " rtp_reject_frame=" + std::to_string(core.videoPacketsRejectedFrame) +
            " rtp_reject_block=" + std::to_string(core.videoPacketsRejectedBlock) +
            " rtp_reject_header=" + std::to_string(core.videoPacketsRejectedHeader) +
            " rtp_reject_range=" + std::to_string(core.videoPacketsRejectedRange) +
            " rtp_reject_duplicate=" + std::to_string(core.videoPacketsRejectedDuplicate) +
            " rtp_reject_transition=" + std::to_string(core.videoPacketsRejectedTransition) +
            " rtp_out_of_order=" + std::to_string(core.videoPacketsOutOfOrder) +
            " rtp_gap_fill=" + std::to_string(core.videoPacketsGapFill) +
            " rtp_observed_gap_packets=" + std::to_string(core.videoSequenceGapPackets) +
            " fec_loss_predictions=" + std::to_string(core.fecSpeculativeLossPredictions) +
            " fec_loss_mispredictions=" + std::to_string(core.fecSpeculativeLossMispredictions) +
            " fec_speculation_resumes=" + std::to_string(core.fecSpeculativeModeResumes) +
            " fec_oos_mode=" + std::to_string(core.fecOosMode) +
            " fec_abandoned_current=" + std::to_string(core.fecAbandonedCurrentBlocks) +
            " fec_abandoned_queued=" + std::to_string(core.fecAbandonedPacketsQueued) +
            " fec_abandoned_data_expected=" + std::to_string(core.fecAbandonedDataExpected) +
            " fec_abandoned_data_received=" + std::to_string(core.fecAbandonedDataReceived) +
            " fec_abandoned_data_missing=" + std::to_string(core.fecAbandonedDataMissing) +
            " fec_abandoned_parity_expected=" + std::to_string(core.fecAbandonedParityExpected) +
            " fec_abandoned_parity_received=" + std::to_string(core.fecAbandonedParityReceived) +
            " fec_abandoned_parity_missing=" + std::to_string(core.fecAbandonedParityMissing) +
            " fec_abandoned_data_missing_max=" + std::to_string(core.fecAbandonedMissingDataMax) +
            " fec_abandoned_parity_missing_max=" + std::to_string(core.fecAbandonedMissingParityMax) +
            " core_idr_requests=" + std::to_string(core.idrRequests) +
            " core_waiting_idr=" + std::to_string(core.waitingForIdr) +
            " core_waiting_idr_drops=" + std::to_string(core.waitingForIdrDrops) +
            " core_idr_wait_entries=" + std::to_string(core.idrWaitEntries) +
            " core_idr_wait_reason=" + std::to_string(core.idrWaitReason) +
            " core_idr_wait_age_us=" + std::to_string(core.waitingForIdr && core.idrWaitStartedUs && now >= core.idrWaitStartedUs ? now - core.idrWaitStartedUs : 0) +
            " core_idr_wait_releases=" + std::to_string(core.idrWaitReleases) +
            " core_idr_wait_last_us=" + std::to_string(core.idrWaitLastUs) +
            " core_idr_wait_max_us=" + std::to_string(core.idrWaitMaxUs) +
            " core_idr_wait_retries=" + std::to_string(core.idrWaitRetryRequests) +
            " core_idr_drop_limit_requests=" + std::to_string(core.idrDropLimitRequests) +
            " core_idr_request_age_us=" + std::to_string(core.lastIdrRequestUs && now >= core.lastIdrRequestUs ? now - core.lastIdrRequestUs : 0) +
            " core_idr_dispatches=" + std::to_string(core.idrDispatches) +
            " core_idr_dispatch_ok=" + std::to_string(core.idrDispatchSuccesses) +
            " core_idr_dispatch_failed=" + std::to_string(core.idrDispatchFailures) +
            " core_idr_dispatch_age_us=" + std::to_string(core.lastIdrDispatchUs && now >= core.lastIdrDispatchUs ? now - core.lastIdrDispatchUs : 0) +
            " core_idr_frames_completed=" + std::to_string(core.idrFramesCompleted);
    }
inline std::string fhQueueText(const FhSnapshot& f,bool known){const auto& core=f.core;        return "Moonlight video_drop=" + std::to_string(f.videoDrops) + " audio_drop=" + std::to_string(f.audioDrops) +
            " audio_overflow_drop=" + std::to_string(f.audioOverflowDrops) +
            " audio_invalid_drop=" + std::to_string(f.audioInvalidDrops) +
            " audio_drop_video_waiting=" + std::to_string(f.audioDropsVideoWaiting) +
            " audio_drop_video_stalled=" + std::to_string(f.audioDropsVideoStalled) +
            " audio_drop_last_ms=" + std::to_string(f.audioDropLastMs) +
            " audio_drop_last_queue_age_ms=" + std::to_string(f.audioDropLastQueueAgeMs) +
            " audio_drop_last_video_age_ms=" + std::to_string(f.audioDropLastVideoAgeMs) +
            " video_pull=1 application_video_queue=0" +
            " invalid_frames=" + std::to_string(f.videoInvalid) + " waiting_idr_drops=" + std::to_string(f.videoWaitDrops) +
            " invalid_resyncs=" + std::to_string(f.videoResyncs) +
            (known ? " core_pending=" + std::to_string(core.decodeQueuePending) +
            " core_high=" + std::to_string(core.decodeQueueHigh) +
            " core_overflows=" + std::to_string(core.decodeQueueOverflows) +
            " core_flushes=" + std::to_string(core.decodeQueueFlushes) +
            " core_flushed_frames=" + std::to_string(core.decodeQueueFlushedFrames) : " core_unavailable=not-published");
    }

inline std::string repairText(const RepairSnapshot& s){
    std::string pts;
    for(unsigned i=0;i<128;++i)if(s.media[i].present)pts+=std::to_string(i)+":n"+std::to_string(s.media[i].nack)+",";
    for(unsigned i=0;i<128;++i)if(s.rtx[i].present)pts+=std::to_string(i)+":a"+std::to_string(s.rtx[i].originalPayload)+",";
    return " repairPts="+pts+" repairMediaPtCount="+std::to_string(s.mediaCount)+" repairRtxPtCount="+std::to_string(s.rtxCount)+
        " repairFid="+std::to_string(s.fid)+" rtxReceived="+std::to_string(s.rtxReceived)+" rtxRestored="+std::to_string(s.restored)+
        " repairControlRejected="+std::to_string(s.controlRejected)+" repairRejected="+std::to_string(s.rejected)+" nackBatches="+std::to_string(s.nackBatches);
}
inline std::string values(std::initializer_list<std::pair<const char*,std::uint64_t>> fields){
    std::string out;for(const auto& field:fields)out+=" "+std::string(field.first)+"="+std::to_string(field.second);return out;
}
inline std::string queueVideoText(const QueueSnapshot& q){
    auto out=values({{"nackSendErrors",q.nackSendErrors},{"queue",q.queue},{"queueBytes",q.queueBytes},{"videoQueue",q.videoQueue},{"audioQueue",q.audioQueue},
        {"videoAgeMs",q.videoAgeMs},{"audioAgeMs",q.audioAgeMs},{"videoAgeHighMs",q.videoAgeHighMs},{"audioAgeHighMs",q.audioAgeHighMs},
        {"videoHigh",q.videoHigh},{"audioHigh",q.audioHigh},{"queueBytesHigh",q.bytesHigh},{"queueResync",q.resyncs},{"queueCapacity",q.capacity},
        {"queueAge",q.age},{"audioDrop",q.audioDrop},{"audioReject",q.audioReject},{"videoGeneration",q.generation},{"pli",q.pli}});
    return out+" videoQueueOwner=assembly videoQueueSpsc=1"+values({{"ingressDrops",q.ingressDrops}})+
        " ingressWaitScope=receive-to-assembly"+values({{"ingressWaitSamples",q.ingressWaitSamples},{"ingressWaitTotalMs",q.ingressWaitTotalMs},
        {"ingressWaitAvgMs",q.ingressWaitSamples?q.ingressWaitTotalMs/q.ingressWaitSamples:0},{"ingressWaitMaxMs",q.ingressWaitMaxMs},
        {"videoAgeKnown",q.videoAgeKnown},{"audioAgeKnown",q.audioAgeKnown},{"videoEmptyObserved",q.videoEmptyObserved},{"audioEmptyObserved",q.audioEmptyObserved}});
}
inline const char* udpPollAvailabilityName(unsigned value){
    static constexpr const char* names[]={"available","not-started","unsupported","read-failed","closed-without-sample"};
    return value<5?names[value]:"unknown";
}
inline std::string udpPollText(const UdpPollSummary& u){
    return std::string(" udpPollSchema=2 udpPollClock=backend-monotonic-us udpPollCpu=unsupported udpPollYieldGapMayIncludeIdle=1 udpPollStatus=")+udpPollAvailabilityName(u.availability)+" udpPollTail=not-sampled-at-stop udpPollDetailMinUs=50000 udpPollDetailLargeDatagrams=256 udpPollDetailYieldOrError=1 udpPollLegacyMinUs=5000 udpPollPhaseScope=sampled-batches-wall-us udpPollPhaseIncludesPreemption=1"+
        values({{"udpPollAvailability",u.availability},{"udpPollHasValue",u.hasValue},{"udpPollProvenance",u.provenance},
        {"udpPollClockValid",u.clockAvailable},{"udpPollSampleValid",u.sampleClockAvailable},{"udpPollLastValid",u.lastClockAvailable},
        {"udpPollMappingKnown",u.mappingKnown},{"udpPollSampleUs",u.sampleMonoUs},{"udpPollQueryBeforeUs",u.queryBeforeUs},{"udpPollQueryAfterUs",u.queryAfterUs},
        {"udpPollClockInvalid",u.clockInvalid},{"udpPollBatches",u.batches},{"udpPollBytes",u.bytes},{"udpPollTimed",u.timedBatches},
        {"udpPollTotalUs",u.elapsedTotalUs},{"udpPollMaxUs",u.elapsedMaxUs},{"udpPollLastStartUs",u.lastStartMonoUs},{"udpPollLastEndUs",u.lastEndMonoUs},
        {"udpPollYieldPending",u.pendingYield},{"udpPollYieldGaps",u.serviceGaps},{"udpPollYieldGapMaxUs",u.serviceGapMaxUs},
        {"udpPollReadyDelays",u.readyDelays},{"udpPollReadyDelayMaxUs",u.readyDelayMaxUs},{"udpPollEvents",u.eventsTotal},{"udpPollOverwritten",u.eventsOverwritten},{"udpPollCursor",u.eventCursor},{"udpPollSourceMissed",u.sourceMissed},{"udpPollJournalSkipped",u.journalSkipped},
        {"udpPollPhaseSampleEvery",u.phaseSampleEvery},
        {"udpPollLegacyEligibleBatches",u.legacyEligibleBatches},
        {"udpPollPhaseSampledBatches",u.phaseSampledBatches},
        {"udpPollPhaseTimedBatches",u.phaseTimedBatches},
        {"udpPollPhaseInvalidBatches",u.phaseInvalidBatches},
        {"udpPollPhaseSampledDatagrams",u.phaseSampledDatagrams},
        {"udpPollPhaseTimedDatagrams",u.phaseTimedDatagrams},
        {"udpPollPhaseSampledReceiveCalls",u.phaseSampledReceiveCalls},
        {"udpPollPhaseTimedReceiveCalls",u.phaseTimedReceiveCalls},
        {"udpPollPhaseRecvTotalUs",u.phaseRecvTotalUs},
        {"udpPollPhaseRecvBatchMaxUs",u.phaseRecvBatchMaxUs},
        {"udpPollPhaseCallbackTotalUs",u.phaseCallbackTotalUs},
        {"udpPollPhaseCallbackBatchMaxUs",u.phaseCallbackBatchMaxUs},
        {"udpRcvbufChecked",u.rcvbufChecked},{"udpRcvbufAvailable",u.rcvbufAvailable}})+
        " udpRcvbufBytes="+std::to_string(u.rcvbufBytes)+" udpRcvbufErrno="+std::to_string(u.rcvbufErrno);
}
inline std::string udpPollEventText(const UdpPollEvent& u){
    return "UDP poll event: schema=2 clock=backend-monotonic-us cpu=unsupported afterYieldMayIncludeIdle=1 phaseScope=sampled-batch-wall-us"+
        values({{"seq",u.sequence},{"startUs",u.startMonoUs},{"elapsedUs",u.elapsedUs},{"datagrams",u.datagrams},{"bytes",u.bytes},
        {"readyDelayUs",u.readyDelayUs},{"previousYieldGapUs",u.serviceGapUs},{"flags",u.flags},{"exit",u.exitReason},{"sampleUs",u.sampleMonoUs},
        {"phaseSampled",bool(u.flags&64)},{"phaseValid",bool(u.flags&128)}})+
        " phaseRecvUs="+((u.flags&128)?std::to_string(u.phaseRecvUs):std::string("unknown"))+
        " phaseCallbackUs="+((u.flags&128)?std::to_string(u.phaseCallbackUs):std::string("unknown"))+
        values({{"mappingKnown",u.mappingKnown},{"queryBeforeUs",u.queryBeforeUs},{"queryAfterUs",u.queryAfterUs},{"sourceMissed",u.sourceMissed}});
}

inline std::string audioCadenceText(const AudioRtpCadenceSnapshot& a){
    return " audioCadenceBoundary=depacketized-before-queue audioRtpClockHz=48000 audioCadenceAbsoluteAge=unavailable"+
        values({{"audioCadenceAvailable",a.packets!=0}})+
        values({{"audioRxPackets",a.packets},{"audioRxBytes",a.bytes},{"audioRxFirstMs",a.firstArrivalMs},
        {"audioRxLastMs",a.lastArrivalMs},{"audioRxGapMaxMs",a.maximumGapMs},{"audioRxBurstIntervals",a.burstIntervals},
        {"audioRxGapsGe40Ms",a.gapsGe40Ms},{"audioRxGapsGe100Ms",a.gapsGe100Ms},{"audioRxGapsGe250Ms",a.gapsGe250Ms},
        {"audioRtpSegments",a.segments},{"audioRtpSegmentFirstMs",a.segmentFirstArrivalMs},{"audioRtpSegmentLastMs",a.segmentLastArrivalMs},
        {"audioRtpSegmentTicks",a.segmentTicks},{"audioRtpSegmentPackets",a.segmentPackets},{"audioRtpRepeats",a.repeatedTimestamps},{"audioRtpBacksteps",a.backsteps},
        {"audioRtpJumps",a.jumps},{"audioRtpWraps",a.wraps},{"audioRxClockRegressions",a.clockRegressions}});
}
inline std::string pressureText(const QueueSnapshot& q){
    static constexpr const char* reasons[]={"none","count","bytes","age"};
    return " queuePressureCount="+std::to_string(q.pressureCount)+" pressureReason="+reasons[q.pressureReason<4?q.pressureReason:0]+
        values({{"pressureAtMs",q.pressureAtMs},{"pressureVideoCount",q.pressureVideoCount},{"pressureAudioCount",q.pressureAudioCount},
        {"pressureVideoBytes",q.pressureVideoBytes},{"pressureAudioBytes",q.pressureAudioBytes},{"pressureVideoAgeMs",q.pressureVideoAgeMs},
        {"pressureAudioAgeMs",q.pressureAudioAgeMs},{"pressureIncomingBytes",q.pressureIncomingBytes},{"audioAgeDropped",q.audioAgeDropped},
        {"audioCapacityDropped",q.audioCapacityDropped},{"audioPriorityPopped",q.audioPriorityPopped}});
}
inline std::string historyEntryText(const HistoryEntry& e){
    static constexpr const char* names[]={"peer","ice","gathering","channel-open","ws-closed","callback-failure"};
    const auto k=static_cast<unsigned>(e.kind);return std::string(k<6?names[k]:"unknown")+":"+std::to_string(e.state)+"@"+std::to_string(e.at);
}
inline std::string historyText(const HistorySnapshot& s){
    std::string out=" firstTerminal=";out+=s.hasTerminal?historyEntryText(s.terminal):"none";
    if(s.hasTerminal)out+=" terminalRtpAgeMs="+(s.terminalVideoAge.known?std::to_string(s.terminalVideoAge.valueMs):"-1")+
        " terminalAudioAgeMs="+(s.terminalAudioAge.known?std::to_string(s.terminalAudioAge.valueMs):"-1")+
        " terminalControlAgeMs="+(s.terminalControlAge.known?std::to_string(s.terminalControlAge.valueMs):"-1");
    out+=" transportHistory=";for(std::size_t i=0;i<s.count&&i<s.entries.size();++i){if(i)out+=',';out+=historyEntryText(s.entries[i]);}return out;
}
inline std::string incomingText(const ControlSnapshot& s){
    const auto& c=s.incoming;const auto& n=c.counts;
    return values({{"incomingIgnored",n[0]},{"incomingStats",n[1]},{"incomingPing",n[2]},{"incomingClose",n[5]},{"incomingError",n[6]},
        {"incomingInvalid",n[7]},{"incomingRumble",n[3]},{"incomingInvalidRumble",n[4]},{"incomingBurst",c.burst},{"incomingBurstHigh",c.highBurst},
        {"incomingDelayHighMs",c.delayHighMs},{"statsCoalesced",c.statsCoalesced},{"pongSent",c.pongsSent},{"pongFailed",c.pongsFailed},
        {"incomingEvents",s.schemas.received},{"incomingSchemas",s.schemas.schemas},{"incomingCoalesced",s.schemas.coalesced},{"incomingSchemaOverflow",s.schemas.overflow}});
}
inline const char* stateName(State s){switch(s){case State::Live:return "live";case State::FinalRetained:return "final";case State::CaptureFailed:return "capture-failed";
    case State::ClosedWithoutFinal:return "closed";case State::NotSupported:return "unsupported";default:return "not-published";}}
inline std::string observationText(const Observation& s){return " snapshotState="+std::string(stateName(s.state))+" snapshotRevision="+std::to_string(s.revision)+
    " snapshotCapturedAtUs="+std::to_string(s.capturedAtUs)+" snapshotAgeUs="+(s.ageKnown?std::to_string(s.ageUs):"unknown")+" snapshotStale="+std::to_string(s.stale);}
inline std::string rtcpText(const NetworkSnapshot& n){
    const auto& s=n.mtsRtcp;const auto& f=s.feedback;
    auto out="scope=transport-lifetime sessions="+std::to_string(s.sessions)+values({{"running",s.running},{"stopped",n.rtcpStopped},
        {"inFlight",f.inFlight},{"inFlightMs",0},{"rtp",f.rtpPackets},{"rtcp",f.rtcpPackets},{"sr",f.senderReports},{"malformed",f.malformedControl},
        {"requests",f.regularRequests},{"coalesced",f.coalescedRequests},{"discarded",f.discardedReports},{"pending",f.pending},{"attempts",f.sendAttempts},
        {"sent",f.sentPackets},{"rr",f.rrPackets},{"remb",f.rembPackets},{"compound",f.compoundPackets},{"sendFailures",f.sendFailures},
        {"pendingAgeMaxUs",f.pendingAgeMaxUs},{"sendAvgUs",f.sendAttempts?f.sendTotalUs/f.sendAttempts:0},{"sendMaxUs",f.sendMaxUs},
        {"workerCalls",s.flushCalls},{"workerAvgUs",s.flushCalls?s.flushTotalUs/s.flushCalls:0},{"workerMaxUs",s.flushMaxUs},{"workerFailures",s.failures},{"rejectedSessions",s.rejectedSessions},
        {"feedbackAvailable",n.rtcp.hasValue},{"feedbackFinal",n.rtcp.state==State::FinalRetained},{"feedbackStale",n.rtcp.stale}});
    return out+" feedbackAgeUs="+(n.rtcp.ageKnown?std::to_string(n.rtcp.ageUs):"-1")+values({{"feedbackCapturedAtUs",n.rtcp.capturedAtUs},{"feedbackRevision",n.rtcp.revision},{"workerActive",n.rtcpWorkerActive}})+observationText(n.rtcp);
}
}

namespace stream::telemetry::network {
inline std::string dtlsText(const DtlsCounters& d) {
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
