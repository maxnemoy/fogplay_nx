#include <switch.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <errno.h>
struct nx_interface {
    struct ifaddrs entry;
    struct sockaddr_in address;
};
int getifaddrs(struct ifaddrs **result) {
    if (!result) { errno = EINVAL; return -1; }
    *result = NULL;
    u32 ip = 0;
    Result rc = nifmGetCurrentIpAddress(&ip);
    if (R_FAILED(rc) || ip == 0) { errno = ENETDOWN; return -1; }
    struct nx_interface *item = calloc(1, sizeof(*item));
    if (!item) { errno = ENOMEM; return -1; }
    item->entry.ifa_name = "nx0";
    item->entry.ifa_flags = IFF_UP;
    item->entry.ifa_addr = (struct sockaddr *)&item->address;
    item->address.sin_len = sizeof(item->address);
    item->address.sin_family = AF_INET;
    item->address.sin_addr.s_addr = ip;
    *result = &item->entry;
    return 0;
}
void freeifaddrs(struct ifaddrs *list) { free(list); }
