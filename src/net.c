/* C2 (C2-SPEC.md §2.1): the netplay UDP thread. See net.h. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "net.h"
#include "netproto.h"
#ifdef SC6RB_NETEM
#include "netem.h"
#endif
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

typedef struct { uint16_t len; uint64_t qpc; uint8_t b[NP_SLOT]; } Slot;
static Slot *g_rx, *g_tx;
static volatile LONG g_rx_w, g_rx_r, g_tx_w, g_tx_r;   /* SPSC indexes (monotonic; slot = index % N) */
static SOCKET g_sock = INVALID_SOCKET;
static WSAEVENT g_sev = WSA_INVALID_EVENT;
static HANDLE g_txev, g_thr;
static volatile LONG g_stop, g_running;
static NetCfg g_c;
static NetCounters g_n;
static struct sockaddr_in g_peer;
static int g_wsa_up;
#ifdef SC6RB_NETEM
static NetEm g_ne; static int g_ne_on;
#endif
static uint64_t g_sent_n;
static volatile LONG g_h_burst;   /* harness only (NOVF): resend the last datagram this many times from the net thread */
static uint8_t g_last_dg[NP_SLOT]; static size_t g_last_n;
void net_h_burst(int n) { InterlockedExchange(&g_h_burst, n); }

#ifdef SC6RB_NETEM
static uint64_t now_us(void) { static LARGE_INTEGER f; LARGE_INTEGER c; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (uint64_t)((double)c.QuadPart * 1e6 / (double)f.QuadPart); }
#endif

int net_parse_peer(const char *s, uint32_t *ip, uint16_t *port) {
    unsigned a, b, c, d, p; char tail = 0;
    if (!s || sscanf(s, "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &p, &tail) != 5) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255 || p < 1 || p > 65535) return 0;
    *ip = (a << 24) | (b << 16) | (c << 8) | d; *port = (uint16_t)p;
    return 1;
}
int net_is_loopback(uint32_t ip) { return (ip >> 24) == 127; }

static void send_raw(const uint8_t *b, size_t n) {
    uint8_t tmp[NP_SLOT];
    const uint8_t *p = b;
    g_sent_n++;
    if (g_c.h_corrupt_every > 0 && g_sent_n % (uint64_t)g_c.h_corrupt_every == 0 && n > NP_HDR) {   /* harness-only plant (NCRC) */
        memcpy(tmp, b, n); tmp[n - 1] ^= 0x5a; p = tmp; InterlockedIncrement64(&g_n.corrupted);
    }
    if (n <= sizeof g_last_dg) { memcpy(g_last_dg, b, n); g_last_n = n; }
    int r = sendto(g_sock, (const char *)p, (int)n, 0, (const struct sockaddr *)&g_peer, sizeof g_peer);
    if (r == (int)n) { InterlockedIncrement64(&g_n.tx); InterlockedExchangeAdd64(&g_n.tx_bytes, (LONGLONG)n); }
    else InterlockedIncrement64(&g_n.send_err);
}
static void tx_drain(void) {
    while (g_tx_r != g_tx_w) {
        Slot *s = &g_tx[(uint32_t)g_tx_r % NET_TX_SLOTS];
        MemoryBarrier();
#ifdef SC6RB_NETEM
        if (g_ne_on) { if (!ne_push(&g_ne, now_us(), s->b, s->len)) {} }
        else
#endif
        send_raw(s->b, s->len);
        MemoryBarrier();
        InterlockedIncrement(&g_tx_r);
    }
#ifdef SC6RB_NETEM
    if (g_ne_on) {
        uint8_t b[NE_PKT]; size_t n;
        uint64_t t = now_us();
        while (ne_pop(&g_ne, t, b, &n)) send_raw(b, n);
        g_n.ne_lost = (long long)g_ne.lost; g_n.ne_dup = (long long)g_ne.dups; g_n.ne_reorder = (long long)g_ne.reordered; g_n.ne_queued = g_ne.n;
    }
#endif
}
static void rx_drain(void) {
    for (;;) {
        uint8_t b[NP_SLOT + 16];
        struct sockaddr_in from; int fl = sizeof from;
        int r = recvfrom(g_sock, (char *)b, (int)sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) return;
            if (e == WSAECONNRESET || e == WSAEMSGSIZE) { InterlockedIncrement64(&g_n.recv_err); continue; }
            InterlockedIncrement64(&g_n.recv_err);
            return;
        }
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        if (fl != sizeof from || from.sin_family != AF_INET || from.sin_addr.s_addr != g_peer.sin_addr.s_addr || from.sin_port != g_peer.sin_port) {
            InterlockedIncrement64(&g_n.rx_foreign);
            continue;
        }
        if (r > NP_SLOT) { InterlockedIncrement64(&g_n.recv_err); continue; }
        if ((uint32_t)(g_rx_w - g_rx_r) >= NET_RX_SLOTS) { InterlockedIncrement64(&g_n.rx_overflow); continue; }
        Slot *s = &g_rx[(uint32_t)g_rx_w % NET_RX_SLOTS];
        s->len = (uint16_t)r; s->qpc = (uint64_t)q.QuadPart; memcpy(s->b, b, (size_t)r);
        MemoryBarrier();
        InterlockedIncrement(&g_rx_w);
        InterlockedIncrement64(&g_n.rx); InterlockedExchangeAdd64(&g_n.rx_bytes, r);
    }
}
static DWORD WINAPI net_thread(LPVOID p) {
    (void)p;
    WSAEVENT evs[2] = { g_sev, g_txev };
    while (!g_stop) {
        DWORD w = WSAWaitForMultipleEvents(2, evs, FALSE, 2, FALSE);
        if (w == WSA_WAIT_EVENT_0) { WSANETWORKEVENTS ne; WSAEnumNetworkEvents(g_sock, g_sev, &ne); }
        rx_drain();
        tx_drain();
        if (g_h_burst > 0 && g_last_n) { LONG k = InterlockedExchange(&g_h_burst, 0); for (LONG i = 0; i < k; i++) sendto(g_sock, (const char *)g_last_dg, (int)g_last_n, 0, (const struct sockaddr *)&g_peer, sizeof g_peer); }
    }
    tx_drain();
    InterlockedExchange(&g_running, 0);
    return 0;
}

int net_start(const NetCfg *c, int *wsa) {
    *wsa = 0;
    np_crc32("", 0, 0);   /* the CRC table, before a second thread can ask for it */
    g_c = *c;
    memset(&g_n, 0, sizeof g_n);
    WSADATA wd;
    if (!g_wsa_up) { int e = WSAStartup(MAKEWORD(2, 2), &wd); if (e) { *wsa = e; return 0; } g_wsa_up = 1; }
    if (!g_rx) g_rx = (Slot *)VirtualAlloc(NULL, sizeof(Slot) * NET_RX_SLOTS, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_tx) g_tx = (Slot *)VirtualAlloc(NULL, sizeof(Slot) * NET_TX_SLOTS, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_rx || !g_tx) { *wsa = -1; return 0; }
    g_rx_w = g_rx_r = g_tx_w = g_tx_r = 0;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { *wsa = WSAGetLastError(); return 0; }
    { BOOL nb = FALSE; DWORD br = 0; WSAIoctl(g_sock, SIO_UDP_CONNRESET, &nb, sizeof nb, NULL, 0, &br, NULL, NULL); }   /* an ICMP port-unreachable never fails recvfrom */
    { int sz = 1 << 20; setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&sz, sizeof sz); }
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(c->port); a.sin_addr.s_addr = htonl(c->bind_loopback ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(g_sock, (struct sockaddr *)&a, sizeof a) == SOCKET_ERROR) { *wsa = WSAGetLastError(); closesocket(g_sock); g_sock = INVALID_SOCKET; return 0; }
    u_long one = 1; ioctlsocket(g_sock, FIONBIO, &one);
    memset(&g_peer, 0, sizeof g_peer);
    g_peer.sin_family = AF_INET; g_peer.sin_port = htons(c->peer_port); g_peer.sin_addr.s_addr = htonl(c->peer_ip);
    g_sev = WSACreateEvent();
    if (g_sev == WSA_INVALID_EVENT || WSAEventSelect(g_sock, g_sev, FD_READ) == SOCKET_ERROR) { *wsa = WSAGetLastError(); closesocket(g_sock); g_sock = INVALID_SOCKET; return 0; }
    g_txev = CreateEventA(NULL, FALSE, FALSE, NULL);
#ifdef SC6RB_NETEM
    {   NeDir o, b; g_ne_on = 0;
        const NeDir *d = c->netem_dir ? &b : &o;
        if (c->netem[0] && ne_profile(c->netem, &o, &b) && ne_active(d)) g_ne_on = ne_init(&g_ne, d, c->netem_seed ? c->netem_seed : 1);
    }
#endif
    g_stop = 0; InterlockedExchange(&g_running, 1);
    g_thr = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
    if (!g_thr) { *wsa = (int)GetLastError(); InterlockedExchange(&g_running, 0); closesocket(g_sock); g_sock = INVALID_SOCKET; return 0; }
    SetThreadPriority(g_thr, THREAD_PRIORITY_ABOVE_NORMAL);
    return 1;
}
void net_stop(void) {
    if (!g_thr) return;
    InterlockedExchange(&g_stop, 1); SetEvent(g_txev);
    WaitForSingleObject(g_thr, 2000);
    CloseHandle(g_thr); g_thr = NULL;
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
}
int net_running(void) { return g_running != 0; }
int net_rx_pop(uint8_t *buf, size_t cap, size_t *n, uint64_t *qpc_rx) {
    if (!g_rx || g_rx_r == g_rx_w) return 0;
    MemoryBarrier();
    Slot *s = &g_rx[(uint32_t)g_rx_r % NET_RX_SLOTS];
    size_t len = s->len < cap ? s->len : cap;
    memcpy(buf, s->b, len); *n = len; if (qpc_rx) *qpc_rx = s->qpc;
    MemoryBarrier();
    InterlockedIncrement(&g_rx_r);
    return 1;
}
int net_tx_push(const uint8_t *buf, size_t n) {
    if (!g_tx || !g_running || n > NP_SLOT) return 0;
    if ((uint32_t)(g_tx_w - g_tx_r) >= NET_TX_SLOTS) { InterlockedIncrement64(&g_n.tx_overflow); return 0; }
    Slot *s = &g_tx[(uint32_t)g_tx_w % NET_TX_SLOTS];
    s->len = (uint16_t)n; memcpy(s->b, buf, n);
    MemoryBarrier();
    InterlockedIncrement(&g_tx_w);
    SetEvent(g_txev);
    return 1;
}
const NetCounters *net_counters(void) { return &g_n; }
int net_h_send_foreign(uint16_t port, const uint8_t *b, size_t n, int count) {
    WSADATA wd; if (!g_wsa_up) { if (WSAStartup(MAKEWORD(2, 2), &wd)) return 0; g_wsa_up = 1; }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int k = 0;
    for (int i = 0; i < count; i++) if (sendto(s, (const char *)b, (int)n, 0, (struct sockaddr *)&a, sizeof a) == (int)n) k++;
    closesocket(s);
    return k;
}
