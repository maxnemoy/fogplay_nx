#pragma once
#include <stdint.h>
#include <netinet/in.h>
struct ip {
    uint8_t ip_hl:4, ip_v:4;
    uint8_t ip_tos;
    uint16_t ip_len, ip_id, ip_off;
    uint8_t ip_ttl, ip_p;
    uint16_t ip_sum;
    struct in_addr ip_src, ip_dst;
};
#define IPVERSION 4
#define IP_DF 0x4000
#define IP_MF 0x2000
#define IP_OFFMASK 0x1fff
#define IP_MAXPACKET 65535
