"""Paced deferred RR/REMB and corrected receiver statistics for pinned libdatachannel."""
from pathlib import Path
import hashlib

HEADER = r'''/**
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#ifndef RTC_RTCP_RECEIVING_SESSION_H
#define RTC_RTCP_RECEIVING_SESSION_H
#if RTC_ENABLE_MEDIA
#include "common.hpp"
#include "mediahandler.hpp"
#include "message.hpp"
#include "rtp.hpp"
#include <atomic>
#include <chrono>
#include <mutex>

#define RTP_SEQ_MOD (1<<16)
namespace rtc {
class RTC_CPP_EXPORT RtcpReceivingSession : public MediaHandler {
public:
	RtcpReceivingSession() = default;
	virtual ~RtcpReceivingSession() = default;
	void incoming(message_vector &messages, const message_callback &send) override;
	bool requestKeyframe(const message_callback &send) override;
	bool requestBitrate(unsigned int bitrate, const message_callback &send) override;
	[[deprecated("Use Track::requestKeyframe()")]] inline bool requestKeyframe() { return false; };
	[[deprecated("Use Track::requestBitrate()")]] inline void requestBitrate(unsigned int) {};
	struct SyncTimestamps { uint64_t rtpTimestamp; uint64_t ntpTimestamp; };
	SyncTimestamps getSyncTimestamps();

	// Opt-in: an application-owned worker flushes latest regular feedback.
	// The captured callback is Track's original weak-transport callback.
	// PLI (and other handlers' NACK) bypass regular report pacing.
	void setDeferredReports(bool enabled);
	bool flushPendingReports(std::chrono::milliseconds interval = std::chrono::milliseconds(250));
	// Terminal for the deferred queue until explicitly enabled again. Never waits on send().
	void discardPendingReports();
	struct FeedbackDiagnostics {
		uint64_t rtpPackets = 0, rtcpPackets = 0, senderReports = 0, malformedControl = 0;
		uint64_t regularRequests = 0, coalescedRequests = 0, discardedReports = 0;
		// sentPackets means the void send callback completed; UDP delivery is not acknowledged.
		uint64_t sendAttempts = 0, sentPackets = 0, sendFailures = 0;
		uint64_t rrPackets = 0, rembPackets = 0, compoundPackets = 0;
		uint64_t pendingAgeMaxUs = 0, sendTotalUs = 0, sendMaxUs = 0;
		bool pending = false, stopped = false, inFlight = false;
	};
	FeedbackDiagnostics getFeedbackDiagnostics() const;
protected:
	void pushREMB(const message_callback &send, unsigned int bitrate);
	void pushRR(const message_callback &send, unsigned int lastSrDelay);
	void pushPLI(const message_callback &send);
	void initSeq(uint16_t seq);
	bool updateSeq(uint16_t seq);
	SSRC mSsrc = 0;
	uint32_t mGreatestSeqNo = 0;
	uint16_t mMaxSeq = 0;
	uint32_t mCycles = 0, mBaseSeq = 0, mBadSeq = 0, mProbation = 0;
	uint32_t mReceived = 0, mExpectedPrior = 0, mReceivedPrior = 0;
	uint32_t mTransit = 0, mJitter = 0;
	SyncTimestamps mSyncTimestamps{0,0};
	std::atomic<unsigned int> mRequestedBitrate = 0;
	std::mutex mSyncMutex;
private:
	using Clock = std::chrono::steady_clock;
	void selectSourceLocked(SSRC ssrc);
	void queueReportsLocked(bool rr, bool remb, const message_callback &send);
	message_ptr makeReceiverReportLocked(unsigned int lastSrDelay, uint32_t &expected, uint32_t &received);
	message_ptr makeRembLocked(unsigned int bitrate);
	uint32_t senderReportDelayLocked(Clock::time_point now) const;
	mutable std::mutex mStateMutex;
	bool mSourceKnown = false, mSequenceInitialized = false;
	bool mDeferredReports = false, mReportsStopped = false, mFlushInFlight = false;
	bool mPendingRR = false, mPendingREMB = false;
	uint64_t mSourceGeneration = 0;
	Clock::time_point mLastSrAt{}, mPendingSince{}, mNextReportAt{};
	message_callback mReportSend;
	FeedbackDiagnostics mFeedback;
};
} // namespace rtc
#endif // RTC_ENABLE_MEDIA
#endif // RTC_RTCP_RECEIVING_SESSION_H
'''

SOURCE = r'''/**
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#if RTC_ENABLE_MEDIA
#include "rtcpreceivingsession.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
namespace rtc {
RtcpReceivingSession::SyncTimestamps RtcpReceivingSession::getSyncTimestamps() {
	std::lock_guard lock(mStateMutex);
	return mSyncTimestamps;
}
void RtcpReceivingSession::selectSourceLocked(SSRC ssrc) {
	if (mSourceKnown && mSsrc == ssrc) return;
	mSourceKnown = true;
	mSsrc = ssrc;
	mSequenceInitialized = false;
	mCycles = mMaxSeq = mBaseSeq = mReceived = mExpectedPrior = mReceivedPrior = 0;
	mBadSeq = RTP_SEQ_MOD + 1;
	mProbation = 0;
	mSyncTimestamps = {0,0};
	mLastSrAt = {};
	++mSourceGeneration;
	if (mPendingRR) { mPendingRR = false; ++mFeedback.discardedReports; }
}
void RtcpReceivingSession::queueReportsLocked(bool rr, bool remb, const message_callback &send) {
	++mFeedback.regularRequests;
	if (mReportsStopped) { mFeedback.discardedReports += unsigned(rr) + unsigned(remb); return; }
	if (mPendingRR || mPendingREMB) ++mFeedback.coalescedRequests;
	else mPendingSince = Clock::now();
	mPendingRR = mPendingRR || rr;
	mPendingREMB = mPendingREMB || remb;
	mReportSend = send;
}
void RtcpReceivingSession::incoming(message_vector &messages, const message_callback &send) {
	message_vector result;
	for (auto &message : messages) {
		if (message->type == Message::Binary) {
			if (message->size() < sizeof(RtpHeader)) continue;
			const auto *rtp = reinterpret_cast<const RtpHeader *>(message->data());
			if (rtp->version() != 2) continue;
			{
				std::lock_guard lock(mStateMutex);
				++mFeedback.rtpPackets;
				selectSourceLocked(rtp->ssrc());
				if (!mSequenceInitialized) { initSeq(rtp->seqNumber()); mSequenceInitialized = true; }
				updateSeq(rtp->seqNumber());
			}
			result.push_back(std::move(message));
		} else if (message->type == Message::Control) {
			{ std::lock_guard lock(mStateMutex); ++mFeedback.rtcpPackets; }
			size_t offset = 0;
			while (offset < message->size()) {
				if (message->size() - offset < sizeof(RtcpHeader)) {
					std::lock_guard lock(mStateMutex); ++mFeedback.malformedControl; break;
				}
				const auto *header = reinterpret_cast<const RtcpHeader *>(message->data() + offset);
				const auto bytes = header->lengthInBytes();
				if (header->version() != 2 || bytes < sizeof(RtcpHeader) || bytes > message->size() - offset) {
					std::lock_guard lock(mStateMutex); ++mFeedback.malformedControl; break;
				}
				if (header->payloadType() == 200) {
					if (bytes < RtcpSr::Size(header->reportCount())) {
						std::lock_guard lock(mStateMutex); ++mFeedback.malformedControl; break;
					}
					const auto *sr = reinterpret_cast<const RtcpSr *>(header);
					bool deferred;
					unsigned int bitrate;
					{
						std::lock_guard lock(mStateMutex);
						++mFeedback.senderReports;
						// Compound RTCP may mention another source mapped to this Track.
						if (mSequenceInitialized && sr->senderSSRC() != mSsrc) { offset += bytes; continue; }
						selectSourceLocked(sr->senderSSRC());
						mSyncTimestamps = {sr->rtpTimestamp(), sr->ntpTimestamp()};
						mLastSrAt = Clock::now();
						deferred = mDeferredReports;
						bitrate = mRequestedBitrate.load();
						if (deferred) queueReportsLocked(true, bitrate > 0, send);
					}
					if (!deferred) { pushRR(send, 0); if (bitrate > 0) pushREMB(send, bitrate); }
				}
				offset += bytes;
			}
		}
	}
	messages.swap(result);
}
void RtcpReceivingSession::setDeferredReports(bool enabled) {
	std::lock_guard lock(mStateMutex);
	mFeedback.discardedReports += unsigned(mPendingRR) + unsigned(mPendingREMB);
	mPendingRR = mPendingREMB = false;
	mReportSend = {};
	mDeferredReports = enabled;
	mReportsStopped = false;
	mNextReportAt = {};
}
void RtcpReceivingSession::discardPendingReports() {
	std::lock_guard lock(mStateMutex);
	mReportsStopped = true;
	mFeedback.discardedReports += unsigned(mPendingRR) + unsigned(mPendingREMB);
	mPendingRR = mPendingREMB = false;
	mReportSend = {};
}
RtcpReceivingSession::FeedbackDiagnostics RtcpReceivingSession::getFeedbackDiagnostics() const {
	std::lock_guard lock(mStateMutex);
	auto snapshot = mFeedback;
	snapshot.pending = mPendingRR || mPendingREMB;
	snapshot.stopped = mReportsStopped;
	snapshot.inFlight = mFlushInFlight;
	return snapshot;
}
uint32_t RtcpReceivingSession::senderReportDelayLocked(Clock::time_point now) const {
	if (mLastSrAt == Clock::time_point{}) return 0;
	const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - mLastSrAt).count();
	return uint32_t(std::min<uint64_t>(uint64_t(std::max<int64_t>(0, us)) * 65536 / 1000000,
	                                   std::numeric_limits<uint32_t>::max()));
}
message_ptr RtcpReceivingSession::makeReceiverReportLocked(unsigned int lastSrDelay, uint32_t &expected, uint32_t &received) {
	auto message = make_message(RtcpRr::SizeWithReportBlocks(1), Message::Control);
	auto *rr = reinterpret_cast<RtcpRr *>(message->data());
	rr->preparePacket(mSsrc, 1);
	const uint32_t extendedMax = mCycles + mMaxSeq;
	expected = mSequenceInitialized ? extendedMax - mBaseSeq + 1 : 0;
	received = mReceived;
	const int64_t lost = std::clamp<int64_t>(int64_t(expected) - received, -0x800000, 0x7fffff);
	const uint32_t expectedInterval = expected - mExpectedPrior;
	const uint32_t receivedInterval = received - mReceivedPrior;
	const int64_t lostInterval = int64_t(expectedInterval) - receivedInterval;
	const uint8_t fraction = expectedInterval == 0 || lostInterval <= 0 ? 0 :
	    uint8_t(std::min<int64_t>(255, (lostInterval * 256) / expectedInterval));
	rr->getReportBlock(0)->preparePacket(mSsrc, fraction, uint32_t(lost), mMaxSeq,
	    uint16_t(mCycles >> 16), mJitter, mSyncTimestamps.ntpTimestamp, lastSrDelay);
	return message;
}
message_ptr RtcpReceivingSession::makeRembLocked(unsigned int bitrate) {
	auto message = make_message(RtcpRemb::SizeWithSSRCs(1), Message::Control);
	auto *remb = reinterpret_cast<RtcpRemb *>(message->data());
	remb->preparePacket(mSsrc, 1, bitrate);
	remb->setSsrc(0, mSsrc);
	return message;
}
bool RtcpReceivingSession::flushPendingReports(std::chrono::milliseconds interval) {
	message_ptr message;
	message_callback send;
	uint32_t expected = 0, received = 0;
	uint64_t generation = 0;
	bool rr = false, remb = false;
	{
		std::lock_guard lock(mStateMutex);
		const auto now = Clock::now();
		if (!mDeferredReports || mReportsStopped || mFlushInFlight || (!mPendingRR && !mPendingREMB) ||
		    now < mNextReportAt || !mReportSend) return false;
		rr = mPendingRR;
		remb = mPendingREMB || (rr && mRequestedBitrate.load() > 0);
		if (rr) message = makeReceiverReportLocked(senderReportDelayLocked(now), expected, received);
		if (remb) {
			auto part = makeRembLocked(mRequestedBitrate.load());
			if (message) message->insert(message->end(), part->begin(), part->end());
			else message = std::move(part);
		}
		generation = mSourceGeneration;
		send = mReportSend;
		mPendingRR = mPendingREMB = false;
		mFlushInFlight = true;
		mNextReportAt = now + std::max(interval, std::chrono::milliseconds(0));
		const auto age = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - mPendingSince).count());
		mFeedback.pendingAgeMaxUs = std::max(mFeedback.pendingAgeMaxUs, age);
		++mFeedback.sendAttempts;
	}
	const auto started = Clock::now();
	try {
		send(message);
	} catch (...) {
		std::lock_guard lock(mStateMutex);
		++mFeedback.sendFailures;
		const auto us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
		mFeedback.sendTotalUs += us;
		mFeedback.sendMaxUs = std::max(mFeedback.sendMaxUs, us);
		mFlushInFlight = false;
		// Loss interval remains uncommitted. Retry latest state, never a stale packet.
		if (!mReportsStopped && generation == mSourceGeneration) queueReportsLocked(rr, remb, send);
		throw;
	}
	{
		std::lock_guard lock(mStateMutex);
		mFlushInFlight = false;
		++mFeedback.sentPackets;
		mFeedback.rrPackets += unsigned(rr);
		mFeedback.rembPackets += unsigned(remb);
		mFeedback.compoundPackets += unsigned(rr && remb);
		const auto us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
		mFeedback.sendTotalUs += us;
		mFeedback.sendMaxUs = std::max(mFeedback.sendMaxUs, us);
		if (rr && generation == mSourceGeneration) { mExpectedPrior = expected; mReceivedPrior = received; }
	}
	return true;
}
bool RtcpReceivingSession::requestBitrate(unsigned int bitrate, const message_callback &send) {
	bool deferred;
	{
		std::lock_guard lock(mStateMutex);
		mRequestedBitrate.store(bitrate);
		deferred = mDeferredReports;
		if (deferred) queueReportsLocked(false, true, send);
	}
	if (!deferred) pushREMB(send, bitrate);
	return true;
}
void RtcpReceivingSession::pushREMB(const message_callback &send, unsigned int bitrate) {
	message_ptr message;
	{ std::lock_guard lock(mStateMutex); message = makeRembLocked(bitrate); }
	send(message);
}
void RtcpReceivingSession::pushRR(const message_callback &send, unsigned int lastSrDelay) {
	message_ptr message;
	uint32_t expected = 0, received = 0;
	uint64_t generation;
	{
		std::lock_guard lock(mStateMutex);
		message = makeReceiverReportLocked(lastSrDelay, expected, received);
		generation = mSourceGeneration;
	}
	send(message);
	{
		std::lock_guard lock(mStateMutex);
		if (generation == mSourceGeneration) { mExpectedPrior = expected; mReceivedPrior = received; }
	}
}
bool RtcpReceivingSession::requestKeyframe(const message_callback &send) { pushPLI(send); return true; }
void RtcpReceivingSession::pushPLI(const message_callback &send) {
	auto message = make_message(RtcpPli::Size(), Message::Control);
	{ std::lock_guard lock(mStateMutex); reinterpret_cast<RtcpPli *>(message->data())->preparePacket(mSsrc); }
	send(message);
}
void RtcpReceivingSession::initSeq(uint16_t seq) {
	++mSourceGeneration; // A same-SSRC restart invalidates an in-flight report snapshot.
	mBaseSeq = mMaxSeq = seq;
	mBadSeq = RTP_SEQ_MOD + 1;
	mCycles = mReceived = mReceivedPrior = mExpectedPrior = 0;
}
bool RtcpReceivingSession::updateSeq(uint16_t seq) {
	const uint16_t delta = seq - mMaxSeq;
	constexpr unsigned maxDropout = 3000, maxMisorder = 100;
	if (delta < maxDropout) {
		if (seq < mMaxSeq) mCycles += RTP_SEQ_MOD;
		mMaxSeq = seq;
	} else if (delta <= RTP_SEQ_MOD - maxMisorder) {
		if (seq == mBadSeq) initSeq(seq);
		else { mBadSeq = (seq + 1) & (RTP_SEQ_MOD - 1); return false; }
	}
	++mReceived;
	return true;
}
} // namespace rtc
#endif // RTC_ENABLE_MEDIA
'''

SOURCES = [
    ('include/rtc/rtcpreceivingsession.hpp', '81caa19a359f3b729bbfefc5fdf9d46f2a02a4d8b71a081599bcc7ea8bf0ca8d', HEADER),
    ('src/rtcpreceivingsession.cpp', '8384e4121dd41f930872b92f47c647a4949003f4a5b3b35572c192e81971a0f4', SOURCE),
]

def apply(root):
    for name, upstream_hash, replacement in SOURCES:
        path = root / "third_party/rtc/libdatachannel" / name
        source = path.read_text()
        if source == replacement:
            continue
        if hashlib.sha256(source.encode()).hexdigest() != upstream_hash:
            raise SystemExit("Unrecognized pinned RTCP receiver source: " + name)
        path.write_text(replacement)

if __name__ == "__main__":
    apply(Path(__file__).resolve().parents[2])
