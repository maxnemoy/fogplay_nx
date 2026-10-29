#ifndef FOG_RTC_THREAD_OBSERVATION_H
#define FOG_RTC_THREAD_OBSERVATION_H
/* Optional application observer, called by thread naming helpers at entry.
 * The observer must copy role before returning and must never throw, change
 * scheduling, or call back into RTC. No application symbol is required. */
#if defined(__GNUC__) || defined(__clang__)
#ifdef __cplusplus
extern "C" {
#endif
void fogObserveThread(const char *role) __attribute__((weak));
#ifdef __cplusplus
}
#endif
static inline void fog_observe_thread_start(const char *role) {
    if (fogObserveThread) fogObserveThread(role);
}
#else
static inline void fog_observe_thread_start(const char *role) { (void)role; }
#endif
#endif
