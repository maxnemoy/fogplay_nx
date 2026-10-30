#ifndef FOG_MOONLIGHT_RECOVERY_H
#define FOG_MOONLIGHT_RECOVERY_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Connection-local state. Reset only before core threads start. The receive
// thread reserves a retry; requests from decoder/control threads also reset
// the deadline using internal monotonic time. Never unlock or invalidate decoder reference state here.
void FogIdrRecoveryReset(void);
void FogIdrRecoveryRequested(void);
bool FogIdrRecoveryTryRetry(void);
// A decoder-thread command; consumed only by the receive/depacketizer owner.
void FogIdrRecoveryRequestDecoderRefresh(void);
bool FogIdrRecoveryTakeDecoderRefresh(void);
// Receive-thread poll; owns depacketizer state and the existing retry gate.
void FogIdrRecoveryPollDecoder(void);
#ifdef __cplusplus
}
#endif
#endif
