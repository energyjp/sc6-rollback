/* C2 (C2-SPEC.md §2.1): the netplay UDP thread and its two SPSC rings. The net thread never reads or writes game memory.
 * Every received datagram is timestamped (QPC), checked for source == the configured peer (else counted rx_foreign and dropped) and
 * pushed into the rx ring (256 x 1,280 B). The game thread pushes datagrams into the tx ring (64 x 1,280 B). A full ring drops the
 * datagram and counts it (the session turns an overflow in an armed battle into ABORT netq_overflow). Addresses are never logged. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define NET_RX_SLOTS 256
#define NET_TX_SLOTS 64

typedef struct {
    uint16_t port;               /* local UDP port */
    uint32_t peer_ip;            /* IPv4, host byte order */
    uint16_t peer_port;
    int bind_loopback;           /* 1 = bind 127.0.0.1, 0 = 0.0.0.0 */
    /* harness / reflector builds only (SC6RB_NETEM): the tx-path netem profile and its seed; ignored in the plugin build */
    char netem[16]; int netem_dir; uint64_t netem_seed;   /* netem_dir: 0 = the profile's out direction, 1 = its back direction (ASYM) */
    int h_corrupt_every;         /* harness only: flip one byte of every n-th sent datagram (NCRC) */
} NetCfg;

typedef struct {
    volatile long long rx, tx, rx_foreign, rx_overflow, tx_overflow, send_err, recv_err, rx_bytes, tx_bytes;
    volatile long long ne_lost, ne_dup, ne_reorder, ne_queued, corrupted;
} NetCounters;

/* 1 = the socket is bound and the thread runs; 0 = failed (*wsa = the WSA error) */
int  net_start(const NetCfg *c, int *wsa);
void net_stop(void);
int  net_running(void);
/* game thread */
int  net_rx_pop(uint8_t *buf, size_t cap, size_t *n, uint64_t *qpc_rx);   /* 1 = a datagram */
int  net_tx_push(const uint8_t *buf, size_t n);                            /* 1 = queued, 0 = ring full (counted) */
const NetCounters *net_counters(void);
/* "a.b.c.d:port" -> ip (host order), port; 1 = ok */
int  net_parse_peer(const char *s, uint32_t *ip, uint16_t *port);
int  net_is_loopback(uint32_t ip);
/* harness only: a third socket sends n datagrams to 127.0.0.1:port (NFOREIGN), returns the count sent */
int  net_h_send_foreign(uint16_t port, const uint8_t *b, size_t n, int count);
void net_h_burst(int n);   /* harness only (NOVF): the net thread resends its last datagram n times at once */
