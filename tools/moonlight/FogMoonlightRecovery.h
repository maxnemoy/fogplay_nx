#ifndef FOG_MOONLIGHT_RECOVERY_H
#define FOG_MOONLIGHT_RECOVERY_H
#include <stdbool.h>
#include <stdint.h>
// Connection-local state. Reset only before core threads start. The receive
// thread reserves a retry; requests from decoder/control threads also reset
// the deadline using internal monotonic time. Never unlock or invalidate decoder reference state here.
void FogIdrRecoveryReset(void);
void FogIdrRecoveryRequested(void);
bool FogIdrRecoveryTryRetry(void);
#endif
