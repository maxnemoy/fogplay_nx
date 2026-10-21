/* Execute the exact numeric helper with libnx 4.12 sockaddr field offsets
 * on the host. libc inet_pton/ntop still provide pure address conversion. */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct nx_sockaddr { uint8_t sa_len, sa_family; char sa_data[14]; };
struct nx_sockaddr_in { uint8_t sin_len, sin_family; in_port_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };
struct nx_sockaddr_in6 { uint8_t sin6_family; in_port_t sin6_port; uint32_t sin6_flowinfo; struct in6_addr sin6_addr; uint32_t sin6_scope_id; };
_Static_assert(offsetof(struct nx_sockaddr, sa_family) == 1, "generic family offset");
_Static_assert(offsetof(struct nx_sockaddr_in6, sin6_family) == 0, "libnx IPv6 family offset");
_Static_assert(sizeof(struct nx_sockaddr_in6) == 28, "libnx IPv6 size");
#define sockaddr nx_sockaddr
#define sockaddr_in nx_sockaddr_in
#define sockaddr_in6 nx_sockaddr_in6
#define sa_family_t uint8_t
#define __SWITCH__ 1
#include "numeric-contract.c"
