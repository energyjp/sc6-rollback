/* C2 (C2-SPEC.md §2.2): the sc6rb netplay wire codec. FROZEN interface: the TOOLS builder's reflector links this file read-only.
 * Pure C: no game state, no Windows, no log. All fields little-endian.
 *
 * Header, 28 B: magic 'S','C','6','R' | u16 proto (1) | u8 type | u8 flags (0) | u32 nonce_tx | u32 nonce_rx | u16 battle | u16 len |
 *               u32 seq | u32 crc32 (IEEE 802.3 over bytes 0..23 and the payload)
 * A datagram is at most NP_MAXDG (1,200) bytes; the encoder refuses a larger one (returns 0). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define NP_PROTO      1
#define NP_HDR        28
#define NP_MAXDG      1200
#define NP_MAXPAY     (NP_MAXDG - NP_HDR)
#define NP_SLOT       1280            /* ring slot payload capacity (net.c) */

enum { NP_HELLO = 1, NP_HELLO_ACK = 2, NP_PING = 3, NP_PONG = 4, NP_SEED = 5, NP_SEED_ACK = 6, NP_SETUP_PART = 7, NP_SETUP_ACK = 8, NP_READY = 9,
       NP_INPUT = 10, NP_ZONE = 11, NP_PAUSE = 12, NP_RESUME = 13, NP_DESYNC_NOTE = 14, NP_DUMP_REQ = 15, NP_DUMP_ACK = 16, NP_QUIT = 17, NP_ABORT = 18,
       NP_NTYPES = 19 };
/* QUIT / ABORT reason codes */
enum { NPR_QUIT_F3 = 1, NPR_DESYNC = 2, NPR_PEER_TIMEOUT = 3, NPR_REFUSED = 4, NPR_SETUP = 5, NPR_CTLTYPE = 6, NPR_BARRIER_MISPREDICT = 7, NPR_DEPTH = 8,
       NPR_PAUSE_CAP = 9, NPR_PEER_RESTART = 10, NPR_NETQ_OVERFLOW = 11, NPR_REC_END = 12, NPR_INTERNAL = 13,
       NPR_PEER_LEFT = 14,   /* C2 fix r1: the peer left this battle (another battle id, or DISARM with no ABORT) and a frame past F is needed */
       NPR_N = 15 };
extern const char *const k_np_type[NP_NTYPES];
extern const char *const k_np_reason[NPR_N];     /* "quit_f3", "desync", ... (index 0 = "-") */

typedef struct {
    uint16_t proto;
    uint8_t  type, flags;
    uint32_t nonce_tx, nonce_rx;
    uint16_t battle, len;
    uint32_t seq, crc;
} NpHdr;

/* decode results */
enum { NP_OK = 0, NP_EMAGIC = 1, NP_EPROTO = 2, NP_ELEN = 3, NP_ECRC = 4 };

uint32_t np_crc32(const void *p, size_t n, uint32_t crc);   /* crc = 0 to start (IEEE 802.3, reflected 0xEDB88320) */
/* writes header + payload into out; h->proto, len and crc are set here. 0 = refused (payload > NP_MAXPAY or cap too small). */
size_t   np_encode(uint8_t *out, size_t cap, NpHdr *h, const void *payload, size_t len);
/* NP_OK and *payload / h filled; NP_EPROTO is reported after the magic and the length matched (the caller may refuse cat=proto) */
int      np_decode(const uint8_t *in, size_t n, NpHdr *h, const uint8_t **payload);

/* little-endian writers / readers on a cursor */
typedef struct { uint8_t *p; size_t n, cap; int over; } NpW;
typedef struct { const uint8_t *p; size_t n, len; int under; } NpR;
void np_w8(NpW *w, uint8_t v); void np_w16(NpW *w, uint16_t v); void np_w32(NpW *w, uint32_t v); void np_w64(NpW *w, uint64_t v);
void np_wb(NpW *w, const void *b, size_t n);
uint8_t np_r8(NpR *r); uint16_t np_r16(NpR *r); uint32_t np_r32(NpR *r); uint64_t np_r64(NpR *r); void np_rb(NpR *r, void *b, size_t n);

/* ---------------------------------------------------------------- HELLO / HELLO_ACK (112 B) */
#define NP_HELLO_LEN 112
typedef struct {
    uint8_t  role;                 /* 1 host, 2 guest */
    char     version[24];          /* SC6RB_VERSION, NUL padded */
    uint64_t plugin_sha;           /* the first 8 bytes of the SHA-256 of the loaded .asi */
    uint32_t exe_ts, exe_soi;
    uint64_t modset2, plugins_list, simcfg_net;
    uint32_t mxcsr; uint16_t x87cw;
    uint32_t ucrt_ms, ucrt_ls, win_build;
    char     cpu_vendor[12];
    uint32_t cpu_fms, cpu_feat;    /* feat: bit0 FMA, bit1 AVX2, bit2 AVX-512F */
} NpHello;
size_t np_hello_put(uint8_t *out, const NpHello *h);              /* NP_HELLO_LEN */
int    np_hello_get(const uint8_t *in, size_t n, NpHello *h);     /* 1 = ok */

/* ---------------------------------------------------------------- INPUT */
#define NP_IN_FIXED   36
#define NP_IN_MAXREC  16
#define NP_IN_MAXCHK  16
#define NP_REC_WORD   8               /* rec_size: the word only (the guest's P2 word) */
#define NP_REC_WORDX  32              /* rec_size: word + the 24-byte third block (the host's P1 word) */
#define NP_IF_PAUSED  0x1
#define NP_IF_BARRIER 0x2
#define NP_IF_DISARM  0x4
typedef struct { uint64_t w; uint8_t x[24]; } NpRec;
typedef struct { uint64_t t1, t15; uint32_t ued; } NpChk;
typedef struct {
    uint32_t first; uint8_t count, rec_size; uint16_t iflags;
    uint32_t ack;                  /* the largest contiguous frame received from the peer */
    uint32_t s;                    /* the sender's lastFrame (its last stepped battle frame) */
    int16_t  adv16;                /* the sender's averaged advantage x 16 */
    uint32_t ping_id, echo_id, echo_hold_us;
    uint32_t cconf;                /* the sender's confirmed frame C (diagnostic; C2 round 1 addition: the spec's 36-byte arithmetic) */
    NpRec    rec[NP_IN_MAXREC];
    uint32_t cfirst; uint8_t ccount;
    NpChk    chk[NP_IN_MAXCHK];
    uint32_t cack;                 /* the largest contiguous frame whose CHECK was received */
} NpInput;
size_t np_input_put(uint8_t *out, size_t cap, const NpInput *in);   /* 0 = does not fit / bad counts */
int    np_input_get(const uint8_t *in, size_t n, NpInput *out);     /* 1 = ok */

/* ---------------------------------------------------------------- small messages */
typedef struct { uint32_t id; uint64_t t_us; uint32_t hold_us; } NpPing;           /* PING (hold unused), PONG */
typedef struct { uint32_t seq, seed; } NpSeed;                                     /* SEED; SEED_ACK uses seq only */
#define NP_SETUP_FRAG 1100
typedef struct { uint16_t idx, count; uint32_t total_len; uint64_t total_fnv; uint16_t dlen; const uint8_t *data; } NpSetupPart;
typedef struct { uint16_t idx; uint32_t z, e; } NpZone;                            /* e = 0xffffffff while open */
typedef struct { uint32_t f; uint64_t local_t1, peer_t1; uint8_t comps_ok; uint64_t comps[25]; } NpDesyncNote;
typedef struct { uint8_t kind; uint32_t f, g; uint8_t comps_ok; uint64_t comps[25]; } NpDumpReq;   /* kind 1 desync, 2 khit */
typedef struct { uint8_t kind; uint32_t g; uint8_t ok; } NpDumpAck;
typedef struct { uint8_t reason; uint32_t frame; } NpEnd;                          /* QUIT, ABORT */
typedef struct { uint32_t frame, paused_ms; } NpPause;                             /* PAUSE (frame), RESUME (frame, paused_ms) */

size_t np_ping_put(uint8_t *o, const NpPing *p);                  int np_ping_get(const uint8_t *i, size_t n, NpPing *p);
size_t np_seed_put(uint8_t *o, const NpSeed *p);                  int np_seed_get(const uint8_t *i, size_t n, NpSeed *p);
size_t np_setup_part_put(uint8_t *o, size_t cap, const NpSetupPart *p); int np_setup_part_get(const uint8_t *i, size_t n, NpSetupPart *p);
size_t np_zone_put(uint8_t *o, const NpZone *p);                  int np_zone_get(const uint8_t *i, size_t n, NpZone *p);
size_t np_desync_put(uint8_t *o, const NpDesyncNote *p);          int np_desync_get(const uint8_t *i, size_t n, NpDesyncNote *p);
size_t np_dumpreq_put(uint8_t *o, const NpDumpReq *p);            int np_dumpreq_get(const uint8_t *i, size_t n, NpDumpReq *p);
size_t np_dumpack_put(uint8_t *o, const NpDumpAck *p);            int np_dumpack_get(const uint8_t *i, size_t n, NpDumpAck *p);
size_t np_end_put(uint8_t *o, const NpEnd *p);                    int np_end_get(const uint8_t *i, size_t n, NpEnd *p);
size_t np_pause_put(uint8_t *o, const NpPause *p);                int np_pause_get(const uint8_t *i, size_t n, NpPause *p);
uint64_t np_fnv64(const void *p, size_t n, uint64_t h);           /* FNV-1a 64; h = 0xcbf29ce484222325 to start */
uint32_t np_fnv32(const void *p, size_t n, uint32_t h);           /* FNV-1a 32; h = 0x811c9dc5 to start */
