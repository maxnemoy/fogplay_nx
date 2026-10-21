#pragma once
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int rtc_switch_getnameinfo(const struct sockaddr *, socklen_t, char *, socklen_t, char *, socklen_t, int);
#ifdef __cplusplus
}
#endif
#ifndef RTC_NUMERIC_FAILURE
#define RTC_NUMERIC_FAILURE(line, condition, host, service) ((void)0)
#endif
#define RTC_NUMERIC_CHECK(condition) do { if (!(condition)) { RTC_NUMERIC_FAILURE(__LINE__, #condition, host, serv); return __LINE__; } } while (0)
static int rtc_numeric_contract(void) {
    struct sockaddr_in v4 = {0};
    struct sockaddr_in6 v6 = {0};
#ifdef __SWITCH__
    v4.sin_len = sizeof(v4);
#endif
    char host[80] = {0}, serv[16] = {0};
    const int numeric = NI_NUMERICHOST | NI_NUMERICSERV;
    v4.sin_family = AF_INET; v4.sin_port = htons(65535);
    RTC_NUMERIC_CHECK(inet_pton(AF_INET, "127.0.0.1", &v4.sin_addr) == 1);
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v4, sizeof(v4), host, sizeof(host), serv, sizeof(serv), numeric | NI_DGRAM) == 0);
    RTC_NUMERIC_CHECK(strcmp(host,"127.0.0.1") == 0 && strcmp(serv,"65535") == 0);
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v4,sizeof(v4),host,sizeof(host),NULL,0,NI_NUMERICHOST | NI_NAMEREQD) == EAI_NONAME);
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v4, sizeof(v4), host, 10, NULL, 0, NI_NUMERICHOST) == 0);
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v4, sizeof(v4), NULL, 0, serv, 6, NI_NUMERICSERV) == 0);
    v4.sin_port = 0;
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v4, sizeof(v4), NULL, 0, serv, 2, numeric) == 0 && strcmp(serv,"0") == 0);
    v6.sin6_family = AF_INET6; v6.sin6_port = htons(443);
    RTC_NUMERIC_CHECK(inet_pton(AF_INET6, "::1", &v6.sin6_addr) == 1);
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v6, sizeof(v6), host, sizeof(host), serv, sizeof(serv), numeric) == 0);
    RTC_NUMERIC_CHECK(strcmp(host,"0:0:0:0:0:0:0:1") == 0 && strcmp(serv,"443") == 0);
#ifdef __SWITCH__
    /* Also accept the ordinary BSD family offset used by generic sockaddrs. */
    unsigned char bsd_v6[sizeof(v6)];
    memcpy(bsd_v6, &v6, sizeof(v6));
    bsd_v6[offsetof(struct sockaddr, sa_len)] = sizeof(v6);
    bsd_v6[offsetof(struct sockaddr, sa_family)] = AF_INET6;
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)bsd_v6, sizeof(bsd_v6), host, sizeof(host), serv, sizeof(serv), numeric) == 0);
    RTC_NUMERIC_CHECK(strcmp(host,"0:0:0:0:0:0:0:1") == 0 && strcmp(serv,"443") == 0);
#endif
    RTC_NUMERIC_CHECK(inet_pton(AF_INET6, "::ffff:192.0.2.1", &v6.sin6_addr) == 1);
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v6, sizeof(v6), host, sizeof(host), serv, sizeof(serv), numeric) == 0);
    struct in6_addr roundtrip;
    RTC_NUMERIC_CHECK(inet_pton(AF_INET6,host,&roundtrip) == 1 && memcmp(&roundtrip,&v6.sin6_addr,sizeof(roundtrip)) == 0);
    const char *ipv6_cases[] = {"::", "2001:db8::1234", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", "2001:0:1:0:2:0:3:4"};
    for (size_t i = 0; i < sizeof(ipv6_cases)/sizeof(ipv6_cases[0]); ++i) {
        RTC_NUMERIC_CHECK(inet_pton(AF_INET6,ipv6_cases[i],&v6.sin6_addr) == 1);
        RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v6,sizeof(v6),host,sizeof(host),serv,sizeof(serv),numeric) == 0);
        RTC_NUMERIC_CHECK(inet_pton(AF_INET6,host,&roundtrip) == 1 && memcmp(&roundtrip,&v6.sin6_addr,sizeof(roundtrip)) == 0);
        RTC_NUMERIC_CHECK(strcmp(serv,"443") == 0);
    }
    RTC_NUMERIC_CHECK(inet_pton(AF_INET6,"fe80::1",&v6.sin6_addr) == 1);
    v6.sin6_scope_id = 4294967295U;
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v6, sizeof(v6), host, sizeof(host), NULL, 0, NI_NUMERICHOST) == 0 && strcmp(host,"fe80:0:0:0:0:0:0:1%4294967295") == 0);
    strcpy(host,"unchanged"); strcpy(serv,"unchanged");
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v4,sizeof(v4),host,9,serv,sizeof(serv),numeric) == EAI_OVERFLOW);
    RTC_NUMERIC_CHECK(strcmp(host,"unchanged") == 0 && strcmp(serv,"unchanged") == 0);
    v4.sin_port = htons(65535);
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v4,sizeof(v4),host,sizeof(host),serv,5,numeric) == EAI_OVERFLOW);
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v4,sizeof(v4)-1,host,sizeof(host),serv,sizeof(serv),numeric) == EAI_FAMILY);
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v6,sizeof(v6)-1,host,sizeof(host),serv,sizeof(serv),numeric) == EAI_FAMILY);
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo(NULL,0,host,sizeof(host),NULL,0,NI_NUMERICHOST) == EAI_FAMILY);
    v4.sin_family = AF_UNSPEC;
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo((struct sockaddr *)&v4,sizeof(v4),host,sizeof(host),NULL,0,NI_NUMERICHOST) == EAI_FAMILY);
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo(NULL,0,NULL,0,NULL,0,numeric) == EAI_NONAME);
    RTC_NUMERIC_CHECK(rtc_switch_getnameinfo(NULL,0,host,sizeof(host),NULL,0,NI_NUMERICHOST | (1U << 29)) == EAI_BADFLAGS);
    return 0;
}
#undef RTC_NUMERIC_CHECK
