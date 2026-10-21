#include <switch.h>
#include <stddef.h>
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
#include <mbedtls/platform_time.h>
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    randomGet(output, len);
    *olen = len;
    return 0;
}
mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)(armTicksToNs(armGetSystemTick()) / 1000000);
}
