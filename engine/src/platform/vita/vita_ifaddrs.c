// getifaddrs for the PS Vita: see vita_ifaddrs.h.
#include "platform/vita/vita_ifaddrs.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <psp2/net/netctl.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct ifaddrs ifa;
    struct sockaddr_in addr, mask, bcast;
    char name[8];
} Entry;

static Entry *entry(const char *name, unsigned flags, struct in_addr ip, struct in_addr mask) {
    Entry *e = (Entry *)calloc(1, sizeof *e);
    if (!e) return NULL;
    strncpy(e->name, name, sizeof e->name - 1);
    e->addr.sin_family = e->mask.sin_family = e->bcast.sin_family = AF_INET;
    e->addr.sin_addr = ip;
    e->mask.sin_addr = mask;
    e->bcast.sin_addr.s_addr = ip.s_addr | ~mask.s_addr;
    e->ifa.ifa_name = e->name;
    e->ifa.ifa_flags = flags;
    e->ifa.ifa_addr = (struct sockaddr *)&e->addr;
    e->ifa.ifa_netmask = (struct sockaddr *)&e->mask;
    e->ifa.ifa_broadaddr = (flags & IFF_BROADCAST) ? (struct sockaddr *)&e->bcast : NULL;
    return e;
}

int getifaddrs(struct ifaddrs **out) {
    struct in_addr lo, lomask;
    lo.s_addr = htonl(0x7f000001), lomask.s_addr = htonl(0xff000000);
    Entry *l = entry("lo", IFF_UP | IFF_RUNNING | IFF_LOOPBACK, lo, lomask);
    if (!l) return -1;
    *out = &l->ifa;
    // Wi-Fi: only while connected (needs sceNetCtlInit, done with the
    // network start-up; otherwise the call fails and only lo is listed)
    SceNetCtlInfo ip, mask;
    struct in_addr a, m;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &ip) >= 0 &&
        sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_NETMASK, &mask) >= 0 && inet_pton(AF_INET, ip.ip_address, &a) == 1 &&
        inet_pton(AF_INET, mask.netmask, &m) == 1 && a.s_addr) {
        Entry *w = entry("wlan0", IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST, a, m);
        if (w) l->ifa.ifa_next = &w->ifa;
    }
    return 0;
}

void freeifaddrs(struct ifaddrs *ifa) {
    while (ifa) {
        struct ifaddrs *n = ifa->ifa_next;
        free(ifa);  // the Entry starts with its ifaddrs
        ifa = n;
    }
}

unsigned int if_nametoindex(const char *name) {
    if (!strcmp(name, "lo")) return 1;
    if (!strcmp(name, "wlan0")) return 2;
    return 0;
}
