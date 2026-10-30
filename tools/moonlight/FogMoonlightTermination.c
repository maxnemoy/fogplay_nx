#include "FogMoonlightTermination.h"
static _Thread_local int origin = FOG_TERMINATION_TRANSPORT;
int FogMoonlightGetTerminationOrigin(void) { return origin; }
void FogMoonlightNotifyHostTermination(void (*callback)(int), int code) {
    const int previous = origin;
    origin = FOG_TERMINATION_HOST;
    callback(code);
    origin = previous;
}
