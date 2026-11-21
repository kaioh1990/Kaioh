// getifaddrs for the PS Vita (vitasdk has neither <ifaddrs.h> nor
// <net/if.h>): the Wi-Fi interface from SceNetCtl plus loopback, enough for
// ik_net.c's net.Interfaces / Addrs (Nearby's subnet sweep, the host's
// address). Included by ik_net.c in place of the system headers.
#ifndef CVF_VITA_IFADDRS_H
#define CVF_VITA_IFADDRS_H
#include <sys/socket.h>

#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK 0x8
#define IFF_POINTOPOINT 0x10
#define IFF_RUNNING 0x40
#define IFF_MULTICAST 0x1000

struct ifaddrs {
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    struct sockaddr *ifa_broadaddr;
    void *ifa_data;
};
int getifaddrs(struct ifaddrs **out);
void freeifaddrs(struct ifaddrs *ifa);
unsigned int if_nametoindex(const char *name);
#endif
