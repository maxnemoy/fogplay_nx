/* Numeric RTC endpoints need no resolver IPC (Eden has no GetNameInfoRequest). */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifdef __SWITCH__
#undef getnameinfo
#endif
#ifndef RTC_NUMERIC_NATIVE_GETNAMEINFO
#define RTC_NUMERIC_NATIVE_GETNAMEINFO getnameinfo
#endif
int rtc_switch_getnameinfo(const struct sockaddr *sa, socklen_t salen,
                          char *host, socklen_t hostlen,
                          char *serv, socklen_t servlen, int flags) {
    const int want_host = host != NULL && hostlen != 0;
    const int want_serv = serv != NULL && servlen != 0;
    if ((want_host && !(flags & NI_NUMERICHOST)) ||
        (want_serv && !(flags & NI_NUMERICSERV)))
        return RTC_NUMERIC_NATIVE_GETNAMEINFO(sa, salen, host, hostlen, serv, servlen, flags);
    int allowed = NI_NUMERICHOST | NI_NUMERICSERV | NI_DGRAM | NI_NOFQDN | NI_NAMEREQD;
#ifdef NI_NUMERICSCOPE
    allowed |= NI_NUMERICSCOPE;
#endif
    if (flags & ~allowed) return EAI_BADFLAGS;
    if (!want_host && !want_serv) return EAI_NONAME;
    if (want_host && (flags & NI_NAMEREQD)) return EAI_NONAME;
    if (!sa || salen < offsetof(struct sockaddr, sa_family) + sizeof(sa->sa_family)) return EAI_FAMILY;
    char numeric_host[INET6_ADDRSTRLEN + 12]; /* '%' + uint32 scope + NUL */
    char numeric_serv[6]; /* 65535 + NUL */
    unsigned port;
    sa_family_t family;
    memcpy(&family, (const char *)sa + offsetof(struct sockaddr, sa_family), sizeof(family));
#ifdef __SWITCH__
    /* libnx 4.12 declares sin6_family at byte 0 without sin6_len, unlike
     * sockaddr/IPv4 (family at byte 1). Accept that typed IPv6 layout only
     * at its exact size and only when the ordinary family is unspecified.
     * This is numeric formatting, not a change to BSD socket wire layouts. */
    if (family == AF_UNSPEC && salen == sizeof(struct sockaddr_in6)) {
        sa_family_t ipv6_family;
        memcpy(&ipv6_family, (const char *)sa + offsetof(struct sockaddr_in6, sin6_family), sizeof(ipv6_family));
        if (ipv6_family == AF_INET6) family = AF_INET6;
    }
#endif
    if (family == AF_INET) {
        if (salen < sizeof(struct sockaddr_in)) return EAI_FAMILY;
        struct sockaddr_in address;
        memcpy(&address, sa, sizeof(address));
        port = ntohs(address.sin_port);
        if (want_host && !inet_ntop(AF_INET, &address.sin_addr, numeric_host, sizeof(numeric_host))) return EAI_SYSTEM;
    } else if (family == AF_INET6) {
        if (salen < sizeof(struct sockaddr_in6)) return EAI_FAMILY;
        struct sockaddr_in6 address;
        memcpy(&address, sa, sizeof(address));
        port = ntohs(address.sin6_port);
        if (want_host) {
            /* libnx 4.12 inet_ntop6 loses hex groups because its output cursor
             * is not advanced. Format all eight network-order groups locally;
             * compression is optional for valid numeric IPv6 addresses. */
            const unsigned char *bytes = (const unsigned char *)&address.sin6_addr;
            unsigned words[8];
            for (size_t i = 0; i < 8; ++i) words[i] = ((unsigned)bytes[i * 2] << 8) | bytes[i * 2 + 1];
            int length = snprintf(numeric_host, sizeof(numeric_host), "%x:%x:%x:%x:%x:%x:%x:%x",
                                  words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
            if (length < 0 || (size_t)length >= sizeof(numeric_host)) return EAI_OVERFLOW;
            if (address.sin6_scope_id) {
                size_t length = strlen(numeric_host);
                int size = snprintf(numeric_host + length, sizeof(numeric_host) - length, "%%%lu", (unsigned long)address.sin6_scope_id);
                if (size < 0 || (size_t)size >= sizeof(numeric_host) - length) return EAI_OVERFLOW;
            }
        }
    } else return EAI_FAMILY;
    if (want_serv) snprintf(numeric_serv, sizeof(numeric_serv), "%u", port);
    /* Validate both sizes before changing either caller output. */
    if ((want_host && strlen(numeric_host) >= hostlen) ||
        (want_serv && strlen(numeric_serv) >= servlen)) return EAI_OVERFLOW;
    if (want_host) memcpy(host, numeric_host, strlen(numeric_host) + 1);
    if (want_serv) memcpy(serv, numeric_serv, strlen(numeric_serv) + 1);
    return 0;
}
