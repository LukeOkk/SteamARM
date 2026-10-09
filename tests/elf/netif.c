// netif: the network interfaces a glibc program sees -- getifaddrs() and
// if_nameindex() (both built on NETLINK_ROUTE dumps) and the SIOCGIF*
// ioctls -- must name the loopback interface consistently. Before the
// runtime answered rtnetlink, if_nameindex() returned NULL and Wine's
// nsiproxy faulted on it (benchmarks/stage62, 15).
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    struct ifaddrs *list;
    CHECK(getifaddrs(&list) == 0, "getifaddrs: %m");
    char lo[IF_NAMESIZE] = "";
    int packet = 0, inet = 0, inet6 = 0;
    for (struct ifaddrs *a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr)
            continue;
        if (a->ifa_addr->sa_family == AF_PACKET) packet++;
        if (a->ifa_addr->sa_family == AF_INET6) inet6++;
        if (a->ifa_addr->sa_family == AF_INET) {
            inet++;
            struct in_addr ip = ((struct sockaddr_in *)a->ifa_addr)->sin_addr;
            if (ip.s_addr == htonl(INADDR_LOOPBACK) && (a->ifa_flags & IFF_LOOPBACK))
                snprintf(lo, sizeof lo, "%s", a->ifa_name);
        }
    }
    printf("getifaddrs: %d link, %d IPv4, %d IPv6 entries; loopback \"%s\"\n", packet, inet, inet6, lo);
    CHECK(packet > 0, "no AF_PACKET (link) entries");
    CHECK(lo[0], "no loopback interface with 127.0.0.1");

    struct if_nameindex *ni = if_nameindex();
    CHECK(ni != NULL, "if_nameindex: NULL (%m)");
    int found = 0;
    for (struct if_nameindex *e = ni; ni && e->if_index; e++)
        if (!strcmp(e->if_name, lo) && e->if_index == if_nametoindex(lo))
            found = 1;
    CHECK(found, "if_nameindex has no \"%s\" with its if_nametoindex index", lo);
    if (ni)
        if_freenameindex(ni);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq r;
    memset(&r, 0, sizeof r);
    snprintf(r.ifr_name, sizeof r.ifr_name, "%s", lo);
    CHECK(ioctl(s, SIOCGIFFLAGS, &r) == 0 && (r.ifr_flags & (IFF_UP | IFF_LOOPBACK)) == (IFF_UP | IFF_LOOPBACK),
          "SIOCGIFFLAGS %s: %m flags 0x%x", lo, r.ifr_flags);
    CHECK(ioctl(s, SIOCGIFHWADDR, &r) == 0 && r.ifr_hwaddr.sa_family == 772 /* ARPHRD_LOOPBACK */,
          "SIOCGIFHWADDR %s: family %d", lo, r.ifr_hwaddr.sa_family);
    CHECK(ioctl(s, SIOCGIFINDEX, &r) == 0 && (unsigned)r.ifr_ifindex == if_nametoindex(lo),
          "SIOCGIFINDEX %s: %d", lo, r.ifr_ifindex);
    CHECK(ioctl(s, SIOCGIFADDR, &r) == 0 &&
          ((struct sockaddr_in *)&r.ifr_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK),
          "SIOCGIFADDR %s", lo);
    memset(r.ifr_name, 0, sizeof r.ifr_name);
    r.ifr_ifindex = (int)if_nametoindex(lo);
    CHECK(ioctl(s, SIOCGIFNAME, &r) == 0 && !strcmp(r.ifr_name, lo), "SIOCGIFNAME: \"%s\"", r.ifr_name);
    strcpy(r.ifr_name, "nosuchif0");
    CHECK(ioctl(s, SIOCGIFFLAGS, &r) == -1, "SIOCGIFFLAGS on a missing interface succeeded");
    close(s);
    freeifaddrs(list);
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails != 0;
}
