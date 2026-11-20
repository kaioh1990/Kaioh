// net for the translated game: TCP (netplay.go) and UDP (GGPO rollback,
// UPnP discovery) over BSD sockets / Winsock.
//
// Goroutines are cooperative (core/sched.c), so every socket is
// non-blocking: a read or accept that would block polls and sleeps 1 ms,
// letting the game loop and other goroutines run, until data arrives or the
// deadline set with SetReadDeadline passes (then a net.Error with
// Timeout() == true, as Go returns).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#define WOULD_BLOCK() (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINPROGRESS)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
int close(int fd);  // <unistd.h> clashes with generated names
int fcntl(int fd, int cmd, ...);
#ifndef F_GETFL
#define F_GETFL 3
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#if defined(__APPLE__)
#define O_NONBLOCK 0x0004
#else
#define O_NONBLOCK 04000
#endif
#endif
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#define WOULD_BLOCK() (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS)
#endif

#include "ik/ik_sim.h"

#ifndef GO_TID_P_net_TCPConn
#define GO_TID_P_net_TCPConn (-30)
#endif
#ifndef GO_TID_P_net_UDPConn
#define GO_TID_P_net_UDPConn (-31)
#endif
#ifndef GO_TID_P_net_TCPListener
#define GO_TID_P_net_TCPListener (-32)
#endif
#ifndef GO_TID_P_net_UDPAddr
#define GO_TID_P_net_UDPAddr (-33)
#endif
#ifndef GO_TID_P_net_TCPAddr
#define GO_TID_P_net_TCPAddr (-34)
#endif
#ifndef GO_TID_P_net_OpError
#define GO_TID_P_net_OpError (-35)
#endif
#ifndef GO_TID_P_net_IPNet
#define GO_TID_P_net_IPNet (-36)
#endif

typedef struct NetSock {
    sock_t fd;
    bool udp, closed;
    i64 rdeadline, wdeadline;  // unix ns, 0 = none
} NetSock;

typedef struct NetOpError {  // *net.OpError as the runtime makes it
    GoString msg;
    bool timeout;
} NetOpError;

static void net_init(void) {
#if defined(_WIN32)
    static bool done;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = true;
    }
#endif
}

static GoIface op_error(const char *op, const char *what, bool timeout) {
    NetOpError *e = go_new(NetOpError);
    char buf[256];
    snprintf(buf, sizeof buf, "%s: %s", op, what);
    e->msg = go_cstr(buf);
    e->msg.p = strcpy((char *)gc_alloc_atomic(strlen(buf) + 1), buf);
    e->timeout = timeout;
    return (GoIface){GO_TID_P_net_OpError, e};
}
GoString net_OpError_Error(GoOpaque *e) { return ((NetOpError *)e)->msg; }
bool net_OpError_Timeout(GoOpaque *e) { return ((NetOpError *)e)->timeout; }
bool net_OpError_Temporary(GoOpaque *e) { return ((NetOpError *)e)->timeout; }

static const char *sock_err(void) {
#if defined(_WIN32)
    static char b[32];
    snprintf(b, sizeof b, "winsock error %d", WSAGetLastError());
    return b;
#else
    return strerror(errno);
#endif
}

static void set_nonblock(sock_t fd) {
#if defined(_WIN32)
    u_long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}

// readiness without blocking (events: POLLIN / POLLOUT)
static bool ready(sock_t fd, bool write) {
#if defined(_WIN32)
    WSAPOLLFD p = {fd, (SHORT)(write ? POLLWRNORM : POLLRDNORM), 0};
    return WSAPoll(&p, 1, 0) > 0;
#else
    struct pollfd p = {fd, (short)(write ? POLLOUT : POLLIN), 0};
    return poll(&p, 1, 0) > 0;
#endif
}

static i64 now_unix_ns(void) { return time_Time_UnixNano(time_Now()); }

// waits (cooperatively) until fd is ready; false on deadline
static bool wait_ready(NetSock *s, bool write) {
    i64 dl = write ? s->wdeadline : s->rdeadline;
    for (;;) {
        if (s->closed) return false;
        if (ready(s->fd, write)) return true;
        if (dl && now_unix_ns() >= dl) return false;
        go_sleep_ns(1000000);
    }
}

// ---------------------------------------------------------------- addresses
static Slice_u8 ip_bytes(const struct sockaddr *sa) {
    Slice_u8 ip = {0};
    if (sa->sa_family == AF_INET) {
        ip = (Slice_u8){(u8 *)gc_alloc_atomic(16), 16, 16};
        memset(ip.p, 0, 10);
        ip.p[10] = ip.p[11] = 0xff;  // Go keeps IPv4 in 16-byte form
        memcpy(ip.p + 12, &((const struct sockaddr_in *)sa)->sin_addr, 4);
    } else if (sa->sa_family == AF_INET6) {
        ip = (Slice_u8){(u8 *)gc_alloc_atomic(16), 16, 16};
        memcpy(ip.p, &((const struct sockaddr_in6 *)sa)->sin6_addr, 16);
    }
    return ip;
}
static int sa_port(const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) return ntohs(((const struct sockaddr_in *)sa)->sin_port);
    if (sa->sa_family == AF_INET6) return ntohs(((const struct sockaddr_in6 *)sa)->sin6_port);
    return 0;
}
static GoIface udp_addr_iface(const struct sockaddr *sa) {
    GoNetUDPAddr *a = go_new(GoNetUDPAddr);
    a->IP = ip_bytes(sa);
    a->Port = sa_port(sa);
    return (GoIface){GO_TID_P_net_UDPAddr, a};
}
static GoIface tcp_addr_iface(const struct sockaddr *sa) {
    GoNetTCPAddr *a = go_new(GoNetTCPAddr);
    a->IP = ip_bytes(sa);
    a->Port = sa_port(sa);
    return (GoIface){GO_TID_P_net_TCPAddr, a};
}

static bool is_v4(Slice_u8 ip) {
    if (ip.len == 4) return true;
    if (ip.len != 16) return false;
    for (int i = 0; i < 10; i++)
        if (ip.p[i]) return false;
    return ip.p[10] == 0xff && ip.p[11] == 0xff;
}
static const u8 *v4_part(Slice_u8 ip) { return ip.len == 4 ? ip.p : ip.p + 12; }

static int to_sockaddr(Slice_u8 ip, int port, struct sockaddr_storage *ss) {
    memset(ss, 0, sizeof *ss);
    if (ip.len == 0 || is_v4(ip)) {
        struct sockaddr_in *in = (struct sockaddr_in *)ss;
        in->sin_family = AF_INET;
        in->sin_port = htons((unsigned short)port);
        if (ip.len) memcpy(&in->sin_addr, v4_part(ip), 4);
        return (int)sizeof *in;
    }
    struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ss;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons((unsigned short)port);
    memcpy(&in6->sin6_addr, ip.p, 16);
    return (int)sizeof *in6;
}

GoString net_IP_String(Slice_u8 ip) {
    char b[64];
    if (ip.len == 0) return GOSTR("<nil>");
    if (is_v4(ip)) {
        const u8 *p = v4_part(ip);
        snprintf(b, sizeof b, "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
    } else if (ip.len == 16) {
        inet_ntop(AF_INET6, ip.p, b, sizeof b);
    } else {
        return GOSTR("?");
    }
    size_t n = strlen(b);
    char *c = (char *)gc_alloc_atomic(n + 1);
    memcpy(c, b, n + 1);
    return (GoString){c, (int)n};
}
Slice_u8 net_IPv4(u8 a, u8 b, u8 c, u8 d) {
    Slice_u8 ip = {(u8 *)gc_alloc_atomic(16), 16, 16};
    memset(ip.p, 0, 10);
    ip.p[10] = ip.p[11] = 0xff;
    ip.p[12] = a, ip.p[13] = b, ip.p[14] = c, ip.p[15] = d;
    return ip;
}
Slice_u8 net_ParseIP(GoString s) {
    u8 b[16];
    const char *z = go_strz(s);
    if (inet_pton(AF_INET, z, b) == 1) return net_IPv4(b[0], b[1], b[2], b[3]);
    if (inet_pton(AF_INET6, z, b) == 1) {
        Slice_u8 ip = {(u8 *)gc_alloc_atomic(16), 16, 16};
        memcpy(ip.p, b, 16);
        return ip;
    }
    return (Slice_u8){0};
}
Slice_u8 net_IP_To4(Slice_u8 ip) {
    if (!is_v4(ip)) return (Slice_u8){0};
    Slice_u8 r = {(u8 *)gc_alloc_atomic(4), 4, 4};
    memcpy(r.p, v4_part(ip), 4);
    return r;
}
bool net_IP_Equal(Slice_u8 a, Slice_u8 b) {
    if (is_v4(a) && is_v4(b)) return !memcmp(v4_part(a), v4_part(b), 4);
    return a.len == b.len && (a.len == 0 || !memcmp(a.p, b.p, (size_t)a.len));
}
bool net_IP_IsLoopback(Slice_u8 ip) {
    if (is_v4(ip)) return v4_part(ip)[0] == 127;
    if (ip.len != 16) return false;
    for (int i = 0; i < 15; i++)
        if (ip.p[i]) return false;
    return ip.p[15] == 1;
}
bool net_IP_IsLinkLocalUnicast(Slice_u8 ip) {
    if (is_v4(ip)) return v4_part(ip)[0] == 169 && v4_part(ip)[1] == 254;
    return ip.len == 16 && ip.p[0] == 0xfe && (ip.p[1] & 0xc0) == 0x80;
}
GoString net_IPMask_String(Slice_u8 m) {
    char *c = (char *)gc_alloc_atomic((size_t)m.len * 2 + 1);
    for (int i = 0; i < m.len; i++) snprintf(c + 2 * i, 3, "%02x", m.p[i]);
    return (GoString){c, m.len * 2};
}
static int mask_bits(Slice_u8 m) {
    int n = 0;
    for (int i = 0; i < m.len; i++)
        for (int b = 7; b >= 0 && (m.p[i] >> b & 1); b--) n++;
    return n;
}
// net.IPMask / net.IP.Mask / net.CIDRMask, with Go's semantics
Slice_u8 net_CIDRMask(int ones, int bits) {
    if ((bits != 32 && bits != 128) || ones < 0 || ones > bits) return (Slice_u8){0};
    int l = bits / 8;
    Slice_u8 m = {(u8 *)gc_alloc_atomic((size_t)l), l, l};
    for (int i = 0; i < l; i++) {
        int n = ones >= 8 ? 8 : ones;
        m.p[i] = (u8)(0xff00 >> n);
        ones -= n;
    }
    return m;
}
Ret_int__int net_IPMask_Size(Slice_u8 m) {
    int ones = 0, i = 0;
    for (; i < m.len && m.p[i] == 0xff; i++) ones += 8;
    if (i < m.len) {  // a partial byte, then only zeros (else non-canonical: 0, 0)
        u8 v = m.p[i++];
        while (v & 0x80) ones++, v = (u8)(v << 1);
        if (v) return (Ret_int__int){0, 0};
        for (; i < m.len; i++)
            if (m.p[i]) return (Ret_int__int){0, 0};
    }
    return (Ret_int__int){ones, m.len * 8};
}
Slice_u8 net_IP_Mask(Slice_u8 ip, Slice_u8 mask) {
    if (mask.len == 16 && ip.len == 4) {
        bool ff = true;
        for (int i = 0; i < 12; i++) ff = ff && mask.p[i] == 0xff;
        if (ff) mask = (Slice_u8){mask.p + 12, 4, 4};
    }
    if (mask.len == 4 && ip.len == 16 && is_v4(ip)) ip = (Slice_u8){ip.p + 12, 4, 4};
    if (ip.len != mask.len) return (Slice_u8){0};
    Slice_u8 out = {(u8 *)gc_alloc_atomic((size_t)ip.len), ip.len, ip.len};
    for (int i = 0; i < ip.len; i++) out.p[i] = ip.p[i] & mask.p[i];
    return out;
}
GoString net_IPNet_String(GoNetIPNet *n) {
    char b[80];
    GoString ip = net_IP_String(n->IP);
    snprintf(b, sizeof b, "%.*s/%d", ip.len, ip.p, mask_bits(n->Mask));
    return go_strcat(go_cstr(b), GOSTR(""));
}
static GoString host_port(Slice_u8 ip, int port) {
    char b[96];
    GoString h = ip.len ? net_IP_String(ip) : GOSTR("");
    if (ip.len && !is_v4(ip)) snprintf(b, sizeof b, "[%.*s]:%d", h.len, h.p, port);
    else snprintf(b, sizeof b, "%.*s:%d", h.len, h.p, port);
    return go_strcat(go_cstr(b), GOSTR(""));
}
GoString net_UDPAddr_String(GoNetUDPAddr *a) { return a ? host_port(a->IP, a->Port) : GOSTR("<nil>"); }
GoString net_TCPAddr_String(GoNetTCPAddr *a) { return a ? host_port(a->IP, a->Port) : GOSTR("<nil>"); }
GoString net_JoinHostPort(GoString h, GoString p) {
    if (memchr(h.p, ':', (size_t)h.len)) return go_strcat(go_strcat(go_strcat(GOSTR("["), h), GOSTR("]:")), p);
    return go_strcat(go_strcat(h, GOSTR(":")), p);
}

// "host:port" -> sockaddr (DNS lookup for names)
static bool resolve(GoString addr, bool udp, struct sockaddr_storage *ss, int *len, const char **err) {
    int c = -1;
    for (int i = addr.len - 1; i >= 0; i--)
        if (addr.p[i] == ':') {
            c = i;
            break;
        }
    if (c < 0) {
        *err = "missing port in address";
        return false;
    }
    char host[256], port[16];
    int hs = 0, he = c;
    if (he > 0 && addr.p[0] == '[' && addr.p[he - 1] == ']') hs = 1, he--;
    if (he - hs >= (int)sizeof host || addr.len - c - 1 >= (int)sizeof port) {
        *err = "address too long";
        return false;
    }
    memcpy(host, addr.p + hs, (size_t)(he - hs));
    host[he - hs] = 0;
    memcpy(port, addr.p + c + 1, (size_t)(addr.len - c - 1));
    port[addr.len - c - 1] = 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_flags = host[0] ? 0 : AI_PASSIVE;
    if (!host[0]) hints.ai_family = AF_INET;
    if (getaddrinfo(host[0] ? host : NULL, port, &hints, &res) != 0 || !res) {
        *err = "no such host";
        return false;
    }
    // prefer IPv4 (the game's lobby publishes IPv4 addresses)
    struct addrinfo *pick = res;
    for (struct addrinfo *r = res; r; r = r->ai_next)
        if (r->ai_family == AF_INET) {
            pick = r;
            break;
        }
    memcpy(ss, pick->ai_addr, pick->ai_addrlen);
    *len = (int)pick->ai_addrlen;
    freeaddrinfo(res);
    return true;
}

Ret_GoNetUDPAddrPtr__GoIface net_ResolveUDPAddr(GoString network, GoString address) {
    net_init();
    bool v6 = network.len == 4 && !memcmp(network.p, "udp6", 4);
    if (!(network.len == 3 && !memcmp(network.p, "udp", 3)) && !(network.len == 4 && !memcmp(network.p, "udp4", 4)) && !v6)
        return (Ret_GoNetUDPAddrPtr__GoIface){NULL, op_error("resolve", "unknown network", false)};
    struct sockaddr_storage ss;
    int sl;
    const char *err = NULL;
    if (!resolve(address, true, &ss, &sl, &err)) return (Ret_GoNetUDPAddrPtr__GoIface){NULL, op_error("lookup", err, false)};
    GoNetUDPAddr *a = (GoNetUDPAddr *)udp_addr_iface((struct sockaddr *)&ss).data;
    if (network.len == 4 && network.p[3] == '4' && !is_v4(a->IP))
        return (Ret_GoNetUDPAddrPtr__GoIface){NULL, op_error("lookup", "no suitable address found", false)};
    // Go leaves IP nil for ":port"
    if (address.len && address.p[0] == ':') a->IP = (Slice_u8){0};
    return (Ret_GoNetUDPAddrPtr__GoIface){a, {0}};
}

// ---------------------------------------------------------------- dial / listen
static Ret_GoIface__GoIface dial(GoString network, GoString address, i64 timeout) {
    net_init();
    bool udp = network.len >= 3 && !memcmp(network.p, "udp", 3);
    struct sockaddr_storage ss;
    int sl;
    const char *err = NULL;
    if (!resolve(address, udp, &ss, &sl, &err)) return (Ret_GoIface__GoIface){{0}, op_error("dial", err, false)};
    sock_t fd = socket(ss.ss_family, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (fd == SOCK_BAD) return (Ret_GoIface__GoIface){{0}, op_error("dial", sock_err(), false)};
    set_nonblock(fd);
    NetSock *s = go_new(NetSock);
    s->fd = fd, s->udp = udp;
    if (connect(fd, (struct sockaddr *)&ss, sl) != 0 && !WOULD_BLOCK()) {
        const char *e = sock_err();
        sock_close(fd);
        return (Ret_GoIface__GoIface){{0}, op_error("dial", e, false)};
    }
    if (!udp) {  // wait for the connection (cooperatively)
        s->wdeadline = timeout > 0 ? now_unix_ns() + timeout : 0;
        if (!wait_ready(s, true)) {
            sock_close(fd);
            return (Ret_GoIface__GoIface){{0}, op_error("dial", "i/o timeout", true)};
        }
        s->wdeadline = 0;
        int so = 0;
        socklen_t ol = sizeof so;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&so, &ol);
        if (so != 0) {
            sock_close(fd);
            return (Ret_GoIface__GoIface){{0}, op_error("dial", "connection refused", false)};
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);  // Go's default
    }
    return (Ret_GoIface__GoIface){{udp ? GO_TID_P_net_UDPConn : GO_TID_P_net_TCPConn, s}, {0}};
}
Ret_GoIface__GoIface net_Dial(GoString network, GoString address) { return dial(network, address, 0); }
Ret_GoIface__GoIface net_Dialer_Dial(GoNetDialer *d, GoString network, GoString address) {
    return dial(network, address, d ? d->Timeout : 0);
}

static Ret_GoIface__GoIface listen_on(GoString network, GoString address, bool udp) {
    net_init();
    struct sockaddr_storage ss;
    int sl;
    const char *err = NULL;
    if (!resolve(address, udp, &ss, &sl, &err)) return (Ret_GoIface__GoIface){{0}, op_error("listen", err, false)};
    (void)network;
    sock_t fd = socket(ss.ss_family, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (fd == SOCK_BAD) return (Ret_GoIface__GoIface){{0}, op_error("listen", sock_err(), false)};
    int one = 1;
    if (!udp) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    if (udp) setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    if (bind(fd, (struct sockaddr *)&ss, sl) != 0 || (!udp && listen(fd, 16) != 0)) {
        const char *e = sock_err();
        sock_close(fd);
        return (Ret_GoIface__GoIface){{0}, op_error("listen", e, false)};
    }
    set_nonblock(fd);
    NetSock *s = go_new(NetSock);
    s->fd = fd, s->udp = udp;
    return (Ret_GoIface__GoIface){{udp ? GO_TID_P_net_UDPConn : GO_TID_P_net_TCPListener, s}, {0}};
}
Ret_GoIface__GoIface net_Listen(GoString network, GoString address) { return listen_on(network, address, false); }
Ret_GoIface__GoIface net_ListenPacket(GoString network, GoString address) { return listen_on(network, address, true); }

Ret_GoOpaque_net_TCPConnPtrPtr__GoIface net_TCPListener_AcceptTCP(GoOpaque *l) {
    NetSock *ls = (NetSock *)l;
    for (;;) {
        if (ls->closed) return (Ret_GoOpaque_net_TCPConnPtrPtr__GoIface){NULL, op_error("accept", "use of closed network connection", false)};
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        sock_t fd = accept(ls->fd, (struct sockaddr *)&ss, &sl);
        if (fd != SOCK_BAD) {
            set_nonblock(fd);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
            NetSock *s = go_new(NetSock);
            s->fd = fd;
            return (Ret_GoOpaque_net_TCPConnPtrPtr__GoIface){(GoOpaque *)s, {0}};
        }
        if (!WOULD_BLOCK()) return (Ret_GoOpaque_net_TCPConnPtrPtr__GoIface){NULL, op_error("accept", sock_err(), false)};
        if (ls->rdeadline && now_unix_ns() >= ls->rdeadline)
            return (Ret_GoOpaque_net_TCPConnPtrPtr__GoIface){NULL, op_error("accept", "i/o timeout", true)};
        go_sleep_ns(2000000);
    }
}
GoIface net_TCPListener_Close(GoOpaque *l) {
    NetSock *s = (NetSock *)l;
    if (!s->closed) sock_close(s->fd), s->closed = true;
    return (GoIface){0};
}

// ---------------------------------------------------------------- conn
Ret_int__GoIface net_conn_Read(GoOpaque *c, Slice_u8 b) {
    NetSock *s = (NetSock *)c;
    for (;;) {
        if (s->closed) return (Ret_int__GoIface){0, op_error("read", "use of closed network connection", false)};
        long n = recv(s->fd, (char *)b.p, (size_t)b.len, 0);
        if (n > 0) return (Ret_int__GoIface){(int)n, {0}};
        if (n == 0 && !s->udp) return (Ret_int__GoIface){0, io_EOF};
        if (n < 0 && !WOULD_BLOCK()) return (Ret_int__GoIface){0, op_error("read", sock_err(), false)};
        if (!wait_ready(s, false)) {
            if (s->closed) continue;
            return (Ret_int__GoIface){0, op_error("read", "i/o timeout", true)};
        }
    }
}
Ret_int__GoIface net_conn_Write(GoOpaque *c, Slice_u8 b) {
    NetSock *s = (NetSock *)c;
    int done = 0;
    while (done < b.len) {
        if (s->closed) return (Ret_int__GoIface){done, op_error("write", "use of closed network connection", false)};
        long n = send(s->fd, (const char *)b.p + done, (size_t)(b.len - done), 0);
        if (n > 0) {
            done += (int)n;
            continue;
        }
        if (n < 0 && !WOULD_BLOCK()) return (Ret_int__GoIface){done, op_error("write", sock_err(), false)};
        if (!wait_ready(s, true)) return (Ret_int__GoIface){done, op_error("write", "i/o timeout", true)};
    }
    return (Ret_int__GoIface){done, {0}};
}
GoIface net_conn_Close(GoOpaque *c) {
    NetSock *s = (NetSock *)c;
    if (s->closed) return op_error("close", "use of closed network connection", false);
    sock_close(s->fd);
    s->closed = true;
    return (GoIface){0};
}
static i64 deadline_of(GoOpaque t) { return time_Time_IsZero(t) ? 0 : time_Time_UnixNano(t); }
GoIface net_conn_SetReadDeadline(GoOpaque *c, GoOpaque t) {
    ((NetSock *)c)->rdeadline = deadline_of(t);
    return (GoIface){0};
}
GoIface net_conn_SetWriteDeadline(GoOpaque *c, GoOpaque t) {
    ((NetSock *)c)->wdeadline = deadline_of(t);
    return (GoIface){0};
}
GoIface net_conn_SetDeadline(GoOpaque *c, GoOpaque t) {
    ((NetSock *)c)->rdeadline = ((NetSock *)c)->wdeadline = deadline_of(t);
    return (GoIface){0};
}
static GoIface sock_addr(NetSock *s, bool peer) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if ((peer ? getpeername(s->fd, (struct sockaddr *)&ss, &sl) : getsockname(s->fd, (struct sockaddr *)&ss, &sl)) != 0)
        return (GoIface){0};
    return s->udp ? udp_addr_iface((struct sockaddr *)&ss) : tcp_addr_iface((struct sockaddr *)&ss);
}
GoIface net_conn_LocalAddr(GoOpaque *c) { return sock_addr((NetSock *)c, false); }
GoIface net_conn_RemoteAddr(GoOpaque *c) { return sock_addr((NetSock *)c, true); }

// *net.TCPConn's promoted methods and the netFD forms are the same calls
Ret_int__GoIface net_TCPConn_Read(GoOpaque *c, Slice_u8 b) { return net_conn_Read(c, b); }
Ret_int__GoIface net_TCPConn_Write(GoOpaque *c, Slice_u8 b) { return net_conn_Write(c, b); }
GoIface net_TCPConn_Close(GoOpaque *c) { return net_conn_Close(c); }
GoIface net_TCPConn_LocalAddr(GoOpaque *c) { return net_conn_LocalAddr(c); }
GoIface net_TCPConn_RemoteAddr(GoOpaque *c) { return net_conn_RemoteAddr(c); }
GoIface net_TCPConn_SetDeadline(GoOpaque *c, GoOpaque t) { return net_conn_SetDeadline(c, t); }
GoIface net_TCPConn_SetReadDeadline(GoOpaque *c, GoOpaque t) { return net_conn_SetReadDeadline(c, t); }
GoIface net_TCPConn_SetWriteDeadline(GoOpaque *c, GoOpaque t) { return net_conn_SetWriteDeadline(c, t); }
Ret_int__GoIface net_netFD_Read(GoOpaque *c, Slice_u8 b) { return net_conn_Read(c, b); }
Ret_int__GoIface net_netFD_Write(GoOpaque *c, Slice_u8 b) { return net_conn_Write(c, b); }
GoIface net_netFD_Close(GoOpaque *c) { return net_conn_Close(c); }
GoIface net_netFD_SetReadDeadline(GoOpaque *c, GoOpaque t) { return net_conn_SetReadDeadline(c, t); }

// UDP packets
Ret_int__GoIface__GoIface net_UDPConn_ReadFrom(GoOpaque *c, Slice_u8 b) {
    NetSock *s = (NetSock *)c;
    for (;;) {
        if (s->closed) return (Ret_int__GoIface__GoIface){0, {0}, op_error("read", "use of closed network connection", false)};
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        long n = recvfrom(s->fd, (char *)b.p, (size_t)b.len, 0, (struct sockaddr *)&ss, &sl);
        if (n >= 0) return (Ret_int__GoIface__GoIface){(int)n, udp_addr_iface((struct sockaddr *)&ss), {0}};
        if (!WOULD_BLOCK()) {
#if defined(_WIN32)
            if (WSAGetLastError() == WSAECONNRESET) continue;  // ICMP port unreachable from an earlier send
#endif
            return (Ret_int__GoIface__GoIface){0, {0}, op_error("read", sock_err(), false)};
        }
        if (!wait_ready(s, false)) {
            if (s->closed) continue;
            return (Ret_int__GoIface__GoIface){0, {0}, op_error("read", "i/o timeout", true)};
        }
    }
}
Ret_int__GoIface net_UDPConn_WriteTo(GoOpaque *c, Slice_u8 b, GoIface addr) {
    NetSock *s = (NetSock *)c;
    Slice_u8 ip = {0};
    int port = 0;
    if (addr.type == GO_TID_P_net_UDPAddr && addr.data) ip = ((GoNetUDPAddr *)addr.data)->IP, port = ((GoNetUDPAddr *)addr.data)->Port;
    else if (addr.type == GO_TID_P_net_TCPAddr && addr.data) ip = ((GoNetTCPAddr *)addr.data)->IP, port = ((GoNetTCPAddr *)addr.data)->Port;
    else return (Ret_int__GoIface){0, op_error("write", "unsupported address", false)};
    struct sockaddr_storage ss;
    int sl = to_sockaddr(ip, port, &ss);
    for (;;) {
        if (s->closed) return (Ret_int__GoIface){0, op_error("write", "use of closed network connection", false)};
        long n = sendto(s->fd, (const char *)b.p, (size_t)b.len, 0, (struct sockaddr *)&ss, sl);
        if (n >= 0) return (Ret_int__GoIface){(int)n, {0}};
        if (!WOULD_BLOCK()) return (Ret_int__GoIface){0, op_error("write", sock_err(), false)};
        // send buffer full (e.g. a subnet sweep waiting on ARP): Go blocks the
        // goroutine until the socket is writable, so wait (cooperatively) too
        go_sleep_ns(1000000);
        if (!wait_ready(s, true)) return (Ret_int__GoIface){0, op_error("write", "i/o timeout", true)};
    }
}

// ---------------------------------------------------------------- interfaces
Ret_Slice_GoNetInterface__GoIface net_Interfaces(void) {
    Slice_GoNetInterface out = {0};
#if defined(_WIN32)
    // one pseudo-interface; its addresses come from Addrs() below
    out.p = (GoNetInterface *)gc_calloc(1, sizeof(GoNetInterface));
    out.len = out.cap = 1;
    out.p[0].Index = 1, out.p[0].Name = GOSTR("default"), out.p[0].Flags = 1 | 8;  // up, multicast-ish
#else
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) return (Ret_Slice_GoNetInterface__GoIface){out, op_error("route", "ip+net: no interfaces", false)};
    int cap = 8;
    out.p = (GoNetInterface *)gc_calloc((size_t)cap, sizeof(GoNetInterface));
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        bool dup = false;
        for (int k = 0; k < out.len; k++)
            if (!strcmp(go_strz(out.p[k].Name), i->ifa_name)) dup = true;
        if (dup) continue;
        if (out.len == cap) {
            GoNetInterface *n = (GoNetInterface *)gc_calloc((size_t)cap * 2, sizeof(GoNetInterface));
            memcpy(n, out.p, sizeof(GoNetInterface) * (size_t)out.len);
            out.p = n, cap *= 2;
        }
        GoNetInterface *g = &out.p[out.len++];
        g->Index = (int)if_nametoindex(i->ifa_name);
        g->Name = go_strcat(go_cstr(i->ifa_name), GOSTR(""));
        uintptr_t f = 0;  // Go's net.Flags: up, broadcast, loopback, point-to-point, multicast
        if (i->ifa_flags & IFF_UP) f |= 1;
        if (i->ifa_flags & IFF_BROADCAST) f |= 2;
        if (i->ifa_flags & IFF_LOOPBACK) f |= 4;
        if (i->ifa_flags & IFF_POINTOPOINT) f |= 8;
        if (i->ifa_flags & IFF_MULTICAST) f |= 16;
        g->Flags = f;
    }
    out.cap = cap;
    freeifaddrs(ifa);
#endif
    return (Ret_Slice_GoNetInterface__GoIface){out, {0}};
}

Ret_Slice_GoIface__GoIface net_Interface_Addrs(GoNetInterface *it) {
    Slice_GoIface out = {0};
    int cap = 4;
    out.p = (GoIface *)gc_calloc((size_t)cap, sizeof(GoIface));
#if defined(_WIN32)
    // the host's IPv4 addresses (enough for the LAN address shown in menus)
    (void)it;
    char host[256];
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    net_init();
    if (gethostname(host, sizeof host) == 0 && getaddrinfo(host, NULL, &hints, &res) == 0) {
        for (struct addrinfo *r = res; r && out.len < cap; r = r->ai_next) {
            GoNetIPNet *n = go_new(GoNetIPNet);
            n->IP = ip_bytes(r->ai_addr);
            n->Mask = (Slice_u8){(u8 *)gc_alloc_atomic(4), 4, 4};
            memset(n->Mask.p, 0xff, 3), n->Mask.p[3] = 0;
            out.p[out.len++] = (GoIface){GO_TID_P_net_IPNet, n};
        }
        freeaddrinfo(res);
    }
#else
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) != 0) return (Ret_Slice_GoIface__GoIface){out, {0}};
    const char *name = go_strz(it->Name);
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        if (strcmp(i->ifa_name, name) || !i->ifa_addr) continue;
        int fam = i->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6) continue;
        if (out.len == cap) {
            GoIface *n = (GoIface *)gc_calloc((size_t)cap * 2, sizeof(GoIface));
            memcpy(n, out.p, sizeof(GoIface) * (size_t)out.len);
            out.p = n, cap *= 2;
        }
        GoNetIPNet *n = go_new(GoNetIPNet);
        n->IP = ip_bytes(i->ifa_addr);
        if (i->ifa_netmask) {
            Slice_u8 m = ip_bytes(i->ifa_netmask);
            n->Mask = fam == AF_INET ? (Slice_u8){m.p + 12, 4, 4} : m;
        }
        out.p[out.len++] = (GoIface){GO_TID_P_net_IPNet, n};
    }
    freeifaddrs(ifa);
#endif
    out.cap = cap;
    return (Ret_Slice_GoIface__GoIface){out, {0}};
}
