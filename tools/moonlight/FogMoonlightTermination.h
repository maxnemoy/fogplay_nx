#pragma once
#ifdef __cplusplus
extern "C" {
#endif
enum FogMoonlightTerminationOrigin {
    FOG_TERMINATION_TRANSPORT = 0,
    FOG_TERMINATION_HOST = 1
};
// Origin is scoped to synchronous callback delivery on the originating core
// thread. Concurrent socket notifications cannot overwrite a host's origin.
int FogMoonlightGetTerminationOrigin(void);
void FogMoonlightNotifyHostTermination(void (*callback)(int), int code);
#ifdef __cplusplus
}
#endif
