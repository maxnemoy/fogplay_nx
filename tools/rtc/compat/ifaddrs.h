#pragma once
#include <sys/socket.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ifaddrs {
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    struct sockaddr *ifa_broadaddr;
    void *ifa_data;
};
int getifaddrs(struct ifaddrs **result);
void freeifaddrs(struct ifaddrs *list);
#ifdef __cplusplus
}
#endif
