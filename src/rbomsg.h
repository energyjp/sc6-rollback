/* Option A M2 (OPTA-M2-SPEC.md §4, sc6rb/M2-INTERFACES.md §5): the rbonline side-channel wire format (Steam P2P, T2).
 * FROZEN interface (the interface pass, v0.29.0): the transport layer implements src/rbomsg.c and may APPEND declarations at the end;
 * no existing struct, constant or enum value changes without a new RBM_PROTO and a note in M2-INTERFACES.md.
 *
 * Pure C: no game state, no Windows, no log, no allocation. Every field little-endian, packed exactly as listed (the encoder writes
 * byte by byte through NpW, never memcpy of a struct). One message per datagram.
 *
 * Datagram = RbmHdr (32 B) + payload (exactly the type's fixed length, RBM_LEN_*). At most RBM_MAXDG bytes.
 *   +0  u32 magic     RBM_MAGIC ('R','B','O','N' = bytes 52 42 4F 4E)
 *   +4  u8  proto     RBM_PROTO
 *   +5  u8  type      RBM_HELLO..RBM_BEACON
 *   +6  u16 len       payload bytes (must equal the type's fixed length)
 *   +8  u32 nonce_tx  the sender's per-battle nonce (random, nonzero; fixed for the battle)
 *   +12 u32 nonce_rx  the receiver's nonce as the sender last saw it (0 = not seen yet)
 *   +16 u32 seq       the sender's message sequence (1, 2, ... per battle; 0 = unsequenced: RBM_ACK, RBM_BEACON)
 *   +20 u32 ack       cumulative ack: every peer seq <= ack was received (0 = none)
 *   +24 u32 ack_bits  selective ack: bit i set = peer seq ack+2+i was also received (bit 0 = ack+2; ack+1 is by definition missing)
 *   +28 u32 crc       CRC-32 (IEEE 802.3, np_crc32 of src/netproto.c) over bytes 0..27 followed by the payload
 * A datagram that fails any check (magic, proto, type, len, crc) is dropped and counted; nothing in it is ever acted on.
 * Identity: no SteamID, name or address is ever carried or logged. The sender's SteamID comes from ReadP2PPacket (p2p.c). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define RBM_MAGIC   0x4E4F4252u     /* "RBON" in memory order */
#define RBM_PROTO   10               /* v0.34.0 (OPTA-M2-SPEC.md §23.1): 10 = the EvaluateIf argc fix in the handshake (RBM_HF_ARGC, compared: RBM_V_ARGC; a disagreement refuses to pair, PASSIVE why=argc_off on both, RBW_ARGC); a v0.33.0 / v0.34.0 pair never pairs (a proto-9 datagram fails rbm_decode with RBM_EPROTO: PASSIVE why=no_answer, PROTO10). v0.33.0 (OPTA-M2-SPEC.md §22.2): 9 = the ping-adaptive input delay agreed at ARM (RbmHello grows to 116 B: + rtt_ms, the sender's RTT estimate; RBM_HF_DADAPT; RbmArm.d is the delay both PCs computed from the two HELLOs and must be equal: RB ARM REFUSED why=d_disagree otherwise; a v0.32.0 and a v0.33.0 never pair: a proto-8 datagram fails rbm_decode with RBM_EPROTO, the mixed pair stays passive why=no_answer, as PROTO9 shows); v0.32.0 (OPTA-M2-SPEC.md §21.1): 8 = the CHECK over the chain of every confirmed frame (RbmCheck grows to 40 B: n, chain, fset; a v0.31.0 and a v0.32.0 never pair: a proto-7 datagram fails rbm_decode with RBM_EPROTO, the mixed pair stays passive why=no_answer, as PROTO8 shows); v0.31.0 (OPTA-M2-SPEC.md §20.5): 7 = the round-start change (what is scanned and pinned at the replay edges, the step thread's free filters): no wire change, but a v0.30.0 and a v0.31.0 never pair (both PCs upgrade together; a proto-6 datagram fails rbm_decode with RBM_EPROTO: a mixed pair stays passive why=no_answer, as PROTO7 shows); v0.30.0 (OPTA-M2-SPEC.md §19): 6 = ARM(0) / BEACON armed_round 0 mean the game's first round (the game numbers rounds from 0) and the new
                                        unsequenced RBM_TSYNC (the time sync); a v0.29.9 and a v0.30.0 never pair (a proto-5 datagram fails rbm_decode with RBM_EPROTO: a mixed
                                        pair stays passive why=no_answer, as PROTO5 / PROTO6 show); v0.29.9 (OPTA-M2-SPEC.md §18.12): 5 =v0.29.9's base-independent content digest; a v0.29.8-or-older and a v0.29.9+ never pair (a proto-4 datagram fails rbm_decode with RBM_EPROTO, so a mixed pair stays passive why=no_answer instead of refusing why=mismatch on a digest of another rule); v0.29.6 (review MINOR 7, OPTA-M2-SPEC.md §18.9): 4 = v0.29.5's canonical CHECK hash; a v0.29.4 and a v0.29.5+ never pair (v0.29.5 itself still sent 3, so the bump takes effect from v0.29.6: a proto-3 datagram fails rbm_decode with RBM_EPROTO); v0.29.2 (Q1 roots, OPTA-M2-SPEC.md §16): 3 = a v0.29.1 and a v0.29.2 never pair; v0.29.1 (review fixes): 2 = + RBM_V_BUILTIN, + the DRAIN reason arm_fail (asymmetric arm) */
#ifdef SC6RB_HARNESS
extern int rbm_h_notsync;            /* v0.30.0 harness only (src/rbomsg.c; h_rbo_ts_notype, XPROTO6): no RBM_TSYNC in this process's codec */
extern int rbm_h_proto;              /* v0.29.6 harness only (src/rbomsg.c; set by rbo.c from h_rbo_proto): the protocol version this process encodes and
                                        accepts, RBM_PROTO by default; PROTO4 runs a peer at 3 (a v0.29.5-or-older build), PROTO5 one at 4 (v0.29.6..v0.29.8) */
#endif
#define RBM_HDR     32
#define RBM_MAXDG   512              /* OPTA-M0.md §1.3 rule (cap 512), kept for T2 */
#define RBM_MAXPAY  (RBM_MAXDG - RBM_HDR)

enum {                               /* message types (sequenced = resent until acked by p2p.c, delivered once, in any order) */
    RBM_HELLO = 1,                   /* sequenced   RbmHello       PROBE: my configuration and measured context */
    RBM_HELLO_ACK = 2,               /* sequenced   RbmHelloAck    my verdict on the peer's HELLO */
    RBM_ARM = 3,                     /* sequenced   RbmArm         PAIRED: I propose arming at round r */
    RBM_ARM_ACK = 4,                 /* sequenced   RbmArm         I accept round r (the agreed values) */
    RBM_CHECK = 5,                   /* sequenced   RbmCheck       tier-1 hash of a confirmed frame (every check_every) */
    RBM_DESYNC = 6,                  /* sequenced   RbmDesync      a CHECK mismatched */
    RBM_DRAIN = 7,                   /* sequenced   RbmDrain       I drained (unilateral or because of the peer) */
    RBM_BYE = 8,                     /* sequenced   RbmBye         battle END (best effort: sent 3x, never waited for) */
    RBM_ACK = 9,                     /* unsequenced (no payload)   transport-internal: acks only; never delivered to the core */
    RBM_BEACON = 10,                 /* unsequenced RbmBeacon      state beacon (1/s while PAIRED/ARMED/DRAINED); delivered, not resent */
    RBM_TSYNC = 11,                  /* v0.30.0 (RBM_PROTO 6): unsequenced RbmTsync  the time-sync report (ARMED, eligible fight ticks only); delivered, not resent */
    RBM_NTYPES = 12
};

/* fixed payload lengths (the decoder refuses any other len for the type) */
#define RBM_LEN_HELLO     116          /* v0.33.0 (RBM_PROTO 9): 112 -> 116 (+ rtt_ms, pad3) */
#define RBM_LEN_HELLO_ACK 8
#define RBM_LEN_ARM       8
#define RBM_LEN_CHECK     40           /* v0.32.0 (RBM_PROTO 8): 24 -> 40 (+ chain, fset; pad2 -> n) */
#define RBM_LEN_DESYNC    24
#define RBM_LEN_DRAIN     8
#define RBM_LEN_BYE       4
#define RBM_LEN_ACK       0
#define RBM_LEN_BEACON    12
#define RBM_LEN_TSYNC     16           /* v0.30.0 */

/* reasons: RbmDrain.reason, RbmBye.reason, the `why=` of RB STATE / RB DRAIN / RB END, and the title's drained:<why> */
enum {
    RBW_NONE = 0, RBW_DESYNC = 1, RBW_FAULT = 2, RBW_PEER_SILENT = 3, RBW_DEPTH = 4, RBW_TRIPWIRE = 5, RBW_MASK = 6,
    RBW_RESET_WALL = 7, RBW_PEER_DRAIN = 8, RBW_ASYM = 9, RBW_KHIT = 10, RBW_COST = 11, RBW_PEER_RESTART = 12, RBW_INTERNAL = 13,
    RBW_ENDPLAY = 14, RBW_STATE5 = 15, RBW_ONLINESYNC0 = 16, RBW_NO_ANSWER = 17, RBW_MISMATCH = 18, RBW_FOREIGN = 19,
    RBW_NOSTEAM = 20, RBW_BADID = 21, RBW_CFG = 22, RBW_NOT_ONLINE = 23, RBW_SPECTATOR = 24, RBW_TAINT = 25, RBW_PEER_BYE = 26,
    RBW_ARMED = 27, RBW_PAIRED = 28, RBW_PROBE = 29,
    /* v0.29.1 (the review fixes, appended; RBM_PROTO 2): */
    RBW_ARM_FAIL = 30,               /* M2: this side agreed on ARM(r) but could not arm (DRAIN reason; the peer armed alone -> rb=ASYM) */
    RBW_SELF = 31,                   /* M1: the session's opponent id is this PC's own SteamID (PROBE refused) */
    RBW_RANKED = 32,                 /* M3: no ranked detector exists and neither rbonline_ranked=1 nor rbonline_unranked_ack=1 is set */
    RBW_ABORT = 33,                  /* B2: the rollback core aborted while armed (DRAIN reason; its committed frames are verified later) */
    RBW_TIMERS = 34,                 /* m4: the stock FetchInput would apply the input-suppression timers (CheckOnlineSessionActive is 0) */
    RBW_C3 = 35,                     /* m2: the c3f0 predicate (FUN_1403f2220) changed between two fetches outside a zone */
    /* v0.29.2 (OPTA-M2-SPEC.md §16.6, appended; RBM_PROTO 3): */
    RBW_Q1SIG = 36,                  /* the Q1 root table's code signatures differ from this exe: PROBE off for the process (v0.29.3: named "q1sig" in
                                        k_rbw_name, review m7 / CQ-1; unit_rbo RBO-P2P-WHY checks every name below RBW_N) */
    /* v0.33.0 (OPTA-M2-SPEC.md §22.2, appended; RBM_PROTO 9): */
    RBW_DARM = 37,                   /* the peer's ARM / ARM_ACK carried another delay than the one this PC computed from the two HELLOs: never armed */
    /* v0.34.0 (OPTA-M2-SPEC.md §23.1, appended; RBM_PROTO 10): */
    RBW_ARGC = 38,                   /* the two HELLOs differ only in RBM_HF_ARGC (one PC advertises the EvaluateIf argc fix, the other cannot or will not): never paired */
    RBW_N = 39
};
extern const char *const k_rbw_name[RBW_N];      /* "-", "desync", "fault", ... (src/rbomsg.c) */
extern const char *const k_rbm_type[RBM_NTYPES]; /* "-", "hello", "hello_ack", ... (src/rbomsg.c) */

/* HELLO_ACK verdict bits (0 = everything compared equal; pairing needs 0 in BOTH directions) */
#define RBM_V_PROTO   0x0001         /* RbmHello.proto differs */
#define RBM_V_EXE     0x0002         /* exe_ts / exe_soi differ */
#define RBM_V_CONTENT 0x0004         /* content digest differs (move records / Creation body data) */
#define RBM_V_CTX     0x0008         /* measured Step-entry context differs (any ctx_* field) */
#define RBM_V_NOCTX   0x0010         /* a side has no usable measurement (ctx_samples == 0 or ctx_const_ok == 0) */
#define RBM_V_CFG     0x0020         /* d / max_rb / check_every differ */
#define RBM_V_RANKED  0x0040         /* ranked_allowed differs (see M2-INTERFACES.md open question Q4) */
#define RBM_V_BUILTIN 0x0080         /* v0.29.1 (review M2): a side's measured Step-entry context differs from the built-in L3-stock constants
                                        (RBO_L3S_*): an asymmetric arm could then never be harmless, so the pair is refused */
#define RBM_V_ARGC    0x0100         /* v0.34.0 (OPTA-M2-SPEC.md §23.1): RBM_HF_ARGC differs (the EvaluateIf argc fix: on only when both PCs advertise it) */

#define RBM_HF_RANKED   0x0001       /* RbmHello.flags: rbonline_ranked=1 on the sender */
#define RBM_HF_TSYNC    0x0002       /* v0.30.0: RbmHello.flags: rbonline_tsync=1 on the sender (logged as peer_tsync=, never compared) */
#define RBM_HF_DADAPT   0x0004       /* v0.33.0: RbmHello.flags: rbonline_d_adapt=1 on the sender (compared: RBM_V_CFG when the two PCs differ) */
#define RBM_HF_ARGC     0x0008       /* v0.34.0: RbmHello.flags: argc_fix=1 AND the four EvaluateIf sites are the verified stock jcc on the sender (src/argcfix.h;
                                        compared: RBM_V_ARGC; both set = the fix is applied at ARM on both PCs and restored at END) */

typedef struct {                     /* RBM_HELLO, 112 B. Values only, never an identity. */
    uint16_t proto;                  /* RBM_PROTO (checked again: the handshake's own version gate) */
    uint16_t flags;                  /* RBM_HF_* */
    char     plugin_version[24];     /* SC6RB_VERSION, NUL padded (logged as peer_ver=, NOT compared) */
    uint32_t exe_ts, exe_soi;        /* 0x61717836, 0x4ba2000 */
    uint64_t content;                /* gameplay content digest (P1, P2 in game order; spec §4) */
    uint32_t content_n[2];           /* the move-record multimap sizes P1 / P2 (PLAYER+0x455d8), for the log */
    uint32_t ctx_xmm[10];            /* xmm6..xmm15 dword 0 at Step entry (xmm6 / xmm13: the most frequent value) */
    uint32_t ctx_mxcsr;              /* MXCSR & 0xffc0 (control bits; the status bits vary per tick) */
    uint16_t ctx_fcw;                /* x87 control word */
    uint8_t  ctx_upper_zero;         /* 1 = dwords 1..3 of xmm6-15 and the ymm upper halves were 0 on every sample */
    uint8_t  ctx_const_ok;           /* 1 = xmm7-12, xmm14, xmm15, MXCSR control and FCW had exactly one value on every sample */
    uint32_t ctx_samples;            /* Step entries measured (0 = none: the peer answers RBM_V_NOCTX) */
    uint8_t  d, max_rb;              /* rbonline_d, rbonline_max_rb */
    uint16_t check_every;            /* rbonline_check_every */
    uint32_t arm_round_min;          /* rbonline_arm_round (0 = earliest) */
    uint16_t rtt_ms;                 /* v0.33.0 (RBM_PROTO 9): the sender's RTT estimate when it built the HELLO (the game's +0x4408, ms; 0 = unknown) */
    uint16_t pad3;
} RbmHello;

typedef struct { uint32_t hello_nonce; uint16_t verdict; uint16_t pad; } RbmHelloAck;   /* 8 B; hello_nonce = the answered HELLO's nonce_tx */
typedef struct { uint8_t round, d, max_rb, pad; uint16_t check_every, pad2; } RbmArm;     /* 8 B (ARM and ARM_ACK) */
/* RBM_CHECK, 40 B (v0.32.0, RBM_PROTO 8; was 24 B with pad2 where n is). (round, gt) = GameRound, GameTime of the frame f (the armed-timeline index,
 * f % check_every == 0); hash = frame f's tier-1-minus-D6 hash (the v0.31.0 CHECK); chain = FNV-1a 64 over le32(k) || le64(hash_k) of every
 * confirmed frame k of the window ((f / E - 1) * E, f] that this PC checked (outside a zone), in order (rbo_chain_step); n = how many; fset = FNV-1a 32
 * over le32(k) of the same frames (rbo_fset_step). The chain is compared only when n and fset are equal on both PCs (the same frames). */
typedef struct { uint8_t round, pad[3]; uint32_t gt; uint32_t f; uint32_t n; uint64_t hash; uint64_t chain; uint32_t fset, pad2; } RbmCheck;
typedef struct { uint8_t round, pad[3]; uint32_t gt; uint64_t mine, theirs; } RbmDesync;  /* 24 B */
typedef struct { uint8_t reason, round, pad[2]; uint32_t gt; } RbmDrain;                  /* 8 B; reason = RBW_* */
typedef struct { uint8_t reason, pad[3]; } RbmBye;                                         /* 4 B */
typedef struct { uint8_t state, armed_round, drained_reason, round; uint32_t gt; uint32_t confirmed_gt; } RbmBeacon;   /* 12 B; state = RboState */
/* v0.30.0 (OPTA-M2-SPEC.md §19.4, design_v0300/DESIGN_C.md §2.3): the time-sync report, 16 B. adv = GameTime - (SyncTime - 1) sampled at IsPaused pre on
 * the sender's eligible fight ticks (armed, InputSync state 2, the core on, outside the zones, InputLag pinned); a window of up to 16 samples. */
typedef struct {
    uint8_t  round;                  /* GameRound of the window */
    uint8_t  flags;                  /* bit0 = the window holds eligible samples; bit1 = the sender's waits are on (rbonline_tsync=1); bit2 = a debt or hold-off runs */
    uint16_t n;                      /* samples in the window (<= 16) */
    int32_t  adv_sum;                /* the sum of adv over the window (frames) */
    uint32_t gt;                     /* the sender's GameTime at the report (log) */
    uint16_t waits;                  /* the sender's waits this round (log) */
    uint16_t rtt_ms;                 /* the game's RTT (+0x4408) rounded (log) */
} RbmTsync;

typedef struct {                     /* one decoded message (what the core sends and receives through p2p.h) */
    uint8_t  type;                   /* RBM_* */
    uint32_t seq;                    /* filled by p2p.c on receive (0 for unsequenced); ignored on send */
    union { RbmHello hello; RbmHelloAck hack; RbmArm arm; RbmCheck check; RbmDesync desync; RbmDrain drain; RbmBye bye; RbmBeacon beacon; RbmTsync tsync; } u;
} RbmMsg;

typedef struct {                     /* the decoded header (p2p.c fills nonce/seq/ack fields on send) */
    uint8_t  proto, type;
    uint16_t len;
    uint32_t nonce_tx, nonce_rx, seq, ack, ack_bits, crc;
} RbmHdr;

enum { RBM_OK = 0, RBM_EMAGIC = 1, RBM_EPROTO = 2, RBM_ETYPE = 3, RBM_ELEN = 4, RBM_ECRC = 5, RBM_ESHORT = 6, RBM_NERR = 7 };
extern const char *const k_rbm_err[RBM_NERR];    /* "ok", "magic", "proto", "type", "len", "crc", "short" */

size_t   rbm_len_of(uint8_t type);                /* the fixed payload length of a type; (size_t)-1 for an unknown type */
int      rbm_sequenced(uint8_t type);             /* 1 for HELLO..BYE, 0 for ACK / BEACON / TSYNC / unknown */
/* header + payload into out (cap >= RBM_HDR + len). h->proto/type/len/crc are set here from m (h's nonce/seq/ack fields are the
 * caller's). Returns the datagram length, or 0 (unknown type, cap too small). m may be NULL for RBM_ACK. */
size_t   rbm_encode(uint8_t *out, size_t cap, RbmHdr *h, const RbmMsg *m);
/* RBM_OK and h / m filled (m->seq = h->seq), or an RBM_E* code (nothing in h / m may be used). Never reads past n. */
int      rbm_decode(const uint8_t *in, size_t n, RbmHdr *h, RbmMsg *m);
