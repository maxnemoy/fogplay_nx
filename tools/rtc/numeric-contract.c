#include <netdb.h>
#include <stdio.h>
static int fallback_calls;
static int numeric_fallback(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags) {
    (void)sa; (void)salen; (void)host; (void)hostlen; (void)serv; (void)servlen; (void)flags;
    ++fallback_calls; return 12345;
}
#define RTC_NUMERIC_NATIVE_GETNAMEINFO numeric_fallback
#include "numeric-switch.c"
#include "numeric-contract.h"
int main(void) {
    int result = rtc_numeric_contract();
    if (result || fallback_calls) { printf("FAIL numeric contract line=%d resolver_calls=%d\n",result,fallback_calls); return 1; }
    char host[80], serv[16];
    if (rtc_switch_getnameinfo(NULL,0,host,sizeof(host),NULL,0,0) != 12345 || fallback_calls != 1) return 2;
    if (rtc_switch_getnameinfo(NULL,0,NULL,0,serv,sizeof(serv),0) != 12345 || fallback_calls != 2) return 3;
    if (rtc_switch_getnameinfo(NULL,0,host,sizeof(host),serv,sizeof(serv),NI_NUMERICHOST) != 12345 || fallback_calls != 3) return 4;
    puts("PASS numeric IPv4/IPv6/mapped/scope/ports/bounds/errors without resolver; native fallback preserved");
    return 0;
}
