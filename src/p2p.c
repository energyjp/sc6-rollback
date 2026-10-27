/* Option A M2: the rbonline side channel over Steam P2P (src/p2p.h; sc6rb/M2-INTERFACES.md §4). Part of the transport layer.
 *
 * Threads and queues
 *   - ONE I/O thread (created by p2p_init, idle until p2p_open). Every Steam (or fake) call is made on it; nothing else calls Steam.
 *   - Game thread -> I/O: a bounded lock-free command ring (Vyukov MPMC cells, used multi-producer / single-consumer) carrying OPEN and
 *     SEND, plus a one-word CLOSE slot (p2p_close can never fail for lack of queue space). Every command carries the open's generation
 *     `gen` (bumped by p2p_open AND p2p_close), so commands of a closed open are discarded and a later OPEN is never undone by an
 *     older CLOSE (and vice versa).
 *   - I/O -> game thread: a bounded SPSC event ring of {gen, P2pEvent}; p2p_poll drops events whose gen is not the current one (the
 *     "flush" of p2p_close without a second consumer). Data events (VERIFIED, MSG, PEER_RESTART) are produced only while the ring has
 *     P2P_EV_RESERVE free slots (else the datagrams stay in Steam's buffer: backpressure, never a lost delivery); control events
 *     (OPENED, NOSTEAM, FOREIGN, SILENT, BUDGET) wait in a small pending list when the ring is full.
 *   - p2p_open / p2p_close / p2p_send / p2p_poll / p2p_stats: no lock, no wait, no syscall, no allocation (static storage only; p2p.c
 *     never allocates at all).
 *
 * Per open (one battle)
 *   - Resolution at every open (UE loads Steam late, H): p2p_set_api's table, else steam_api64.dll via GetModuleHandleA (never
 *     LoadLibrary), PE timestamp 0x585894b1, GetProcAddress of the flat exports, SteamClient() -> GetHSteamUser/Pipe ->
 *     SteamAPI_ISteamClient_GetISteamNetworking(..., "SteamNetworking005"). Any failure -> P2P_EV_NOSTEAM (detail P2P_NS_*).
 *   - Self check (v0.29.1, review M1): the local SteamID (SteamAPI_ISteamClient_GetISteamUser(.., "SteamUser019") ->
 *     SteamAPI_ISteamUser_GetSteamID) must be known and differ from the expected peer id; equal -> P2P_EV_SELF, unknown -> P2P_EV_NOSTEAM
 *     (P2P_NS_NO_USER). Either way nothing is sent (unless h_no_self_check).
 *   - Candidates (v0.29.7, OPTA-M2-SPEC.md §18.10): the OPEN carries up to P2P_CAND_MAX candidate ids in the core's order. In order, a
 *     candidate is skipped when it is this PC's own id (self) and kept only when SteamAPI_ISteamNetworking_GetP2PSessionState says Steam has
 *     a P2P session with it (active or connecting: the user the game itself talks to). The first kept one is the peer, unless a different
 *     one is kept too (ambiguous: none). None kept: every candidate self -> P2P_EV_SELF, else P2P_EV_BADID. Counts only are published
 *     (p2p_peerid_result) before the event; nothing is sent before a peer is chosen.
 *   - Guard (T-M2-10): every datagram already waiting on our channel is examined. A size that is not one of the six RBM sizes is
 *     foreign WITHOUT being read (we never consume another program's packet to find out); otherwise it is read: another id or a failed
 *     decode -> foreign -> P2P_EV_FOREIGN and no further Steam call this open (unless h_no_guard). A read that fails while a datagram is
 *     reported waiting counts as foreign too (v0.29.1, review m3: fail closed). A valid RBM datagram from the expected id predates this
 *     open's fresh nonce: discarded (open_stale / rx_stale_nonce). Then P2P_EV_OPENED.
 *   - Peer filter: only datagrams whose ReadP2PPacket remote id == the expected id (unless h_accept_any_id).
 *   - Nonces: nonce_rx must be 0 or my nonce; the first accepted datagram teaches the peer's nonce_tx; later a different nonce_tx is
 *     stale (nonce_rx == my nonce) or a peer restart (nonce_rx == 0: P2P_EV_PEER_RESTART once, never adopted). nonce_tx 0 = stale.
 *     nonce_tx == my own nonce = our own datagram looped back (v0.29.1, review M1): dropped (rx_self), never adopted, never verifies.
 *   - Unverified budget: until the first valid datagram only RBM_HELLO goes out (p2p_send refuses the rest), at most probe_max
 *     datagrams spaced >= probe_gap_ms; when they are used up and probe_gap_ms passed after the last one with no answer:
 *     P2P_EV_BUDGET and no datagram of any kind for the rest of the open.
 *   - Reliability: sequenced types get seq 1, 2, ... (assigned by the I/O thread); each is (re)sent every resend_ms until acked
 *     (h_no_resend: once). Every datagram carries the cumulative ack + 32 selective bits. Receipt of a sequenced datagram (new or
 *     duplicate) schedules a pure RBM_ACK min(50, resend_ms/4) ms later unless another datagram carries the ack first. Each seq is
 *     delivered once (any order) within a P2P_RX_WINDOW receive window. RBM_BEACON: sent once, delivered as received. RBM_ACK: never
 *     delivered.
 *   - Rate cap: a token bucket (P2P_RATE_PER_S, burst P2P_RATE_BURST; p2p_set_rate_cap for tests) over every datagram except the
 *     three BYEs of p2p_close.
 *   - Silent peer: verified and no valid datagram for peer_timeout_ms -> P2P_EV_SILENT (once).
 *   - p2p_close: if the peer was verified (and the budget not exhausted), RBM_BYE(why) three times back to back (no wait), then the
 *     peer is forgotten and every queue of the open is dropped. The next open starts at seq 1. An ACTIVE open's channel is closed with
 *     CloseP2PChannelWithUser(peer, our channel) P2P_CHCLOSE_DELAY_MS after the BYEs (v0.29.1, review m3: Steam's per-user channel
 *     state goes back to what it was before our PROBE); earlier when another open or the thread's stop comes first.
 *   - h_corrupt_every = N: every Nth datagram of an open goes out with one CRC bit flipped (a plant; the peer must reject it).
 *
 * Shutdown: p2p_shutdown(0) stops the thread (it sends BYE first if a verified open is still active) and waits at most
 * P2P_SHUTDOWN_WAIT_MS; a thread stuck in a Steam call is left running and joined by the next p2p_init. p2p_shutdown(1) (DllMain at
 * process exit, where the OS already terminated the thread) never waits and never calls Steam.
 *
 * Logging: only from the I/O thread, `P2P ...` lines, at most P2P_LOG_PER_OPEN per open and P2P_LOG_PER_PROC per process. Never a
 * SteamID, a nonce, a name or an address. p2p.c uses nothing of the plugin but log.c (no g_cfg, no g_base, no game memory, no game
 * code): unit_rbo checks the object's undefined symbols (RBO-P2P-STATIC).
 *
 * Steam binding, re-VERIFIED read-only (2026-10-12, pefile + capstone on
 * <game folder>\...\Steamworks\Steamv139\Win64\steam_api64.dll: 242,976 bytes, TimeDateStamp 0x585894b1, machine 0x8664, 808 exports):
 *   SteamAPI_ISteamNetworking_SendP2PPacket  0x4ea0  sub rsp,38h; eax=[rsp+68h] -> [rsp+28h]; eax=[rsp+60h] -> [rsp+20h]; call [vtbl+0]
 *       = rcx this, rdx CSteamID, r8 data, r9d len, stack arg 5 (EP2PSend, dword) and arg 6 (channel, dword) re-pushed unchanged
 *   SteamAPI_ISteamNetworking_IsP2PPacketAvailable 0x5390  jmp [vtbl+8]     (rcx this, rdx uint32 *size, r8d channel)
 *   SteamAPI_ISteamNetworking_ReadP2PPacket  0x4e60  jmp [vtbl+10h]          (this, dst, cap, uint32 *size, CSteamID *remote [stack 5], channel [stack 6])
 *   SteamAPI_ISteamClient_GetISteamNetworking 0x4150  jmp [vtbl+80h]          (client, HSteamUser, HSteamPipe, version)
 *   SteamClient 0x8c70 `mov rax,[rip+...]; ret`; SteamAPI_GetHSteamUser 0x7ea0 / GetHSteamPipe 0x7eb0 `mov eax,[rip+...]; ret`
 *   (AcceptP2PSessionWithUser 0x4ac0 -> vtbl+18h exists and is never called. CloseP2PChannelWithUser 0x47f0 -> vtbl+28h: called once per
 *   ACTIVE open since v0.29.1, see p2p_close above.)
 *   Every wrapper forwards its arguments untouched, so the C prototypes below ARE the virtuals' ABI. HYPOTHESIS (strong; M2 Q2): the
 *   CSteamID by-value argument is its uint64 in rdx inside steamclient64.dll (an 8-byte class without a user copy constructor or
 *   destructor travels in a register under MSVC x64; Steamworks.NET calls this same flat export with a ulong). Nothing offline can
 *   settle it: RBO-P2P-NOSTEAM only proves p2p.c passes the id as the wrapper's 2nd register argument (a fake DLL with the same
 *   prototypes); S-B settles it (a wrong ABI shows as P2P_EV_BUDGET on both PCs, never as a send to a wrong id). */
#include "common.h"
#include "p2p.h"
#include "netproto.h"

const char *const k_p2p_ev[P2P_EV_N] = { "none", "opened", "nosteam", "foreign", "verified", "msg", "silent", "peer_restart", "budget", "self", "badid" };
const char *const k_p2p_ns[P2P_NS_N] = { "-", "no_module", "dll_ts", "export", "no_client", "no_networking", "no_user" };

#define P2P_CMDQ          256      /* command ring (power of 2) */
#define P2P_EVQ           512      /* event ring (power of 2) */
#define P2P_EV_RESERVE    16       /* free event slots required before a datagram is read */
#define P2P_CTL_PENDING   8
#define P2P_UNSEQQ        32       /* queued RBM_BEACONs / RBM_TSYNCs (v0.30.0: 16 -> 32, the time-sync reports ride the same queue) */
#define P2P_RXBUF         65536    /* read buffer (Steam unreliable <= 1200 B; reliable packets of another program can be larger) */
#define P2P_DRAIN_MAX     256      /* datagrams examined at p2p_open */
#define P2P_RX_PER_IT     256      /* datagrams read per I/O iteration */
#define P2P_LOG_PER_OPEN  16
#define P2P_LOG_PER_PROC  400
#define P2P_ID_PATTERN    0x01100001u   /* (id >> 32): universe public, individual account, desktop instance */
#define P2P_CHCLOSE_DELAY_MS 250          /* v0.29.1: CloseP2PChannelWithUser this long after an ACTIVE open's close (its BYEs go first) */

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* ---------------------------------------------------------------- lock-free rings (Vyukov bounded queue cells) */
enum { CMD_OPEN = 1, CMD_SEND = 2 };
typedef struct { uint32_t kind, gen; uint64_t peer; uint32_t nonce; RbmMsg msg; uint32_t ncand; uint64_t cands[P2P_CAND_MAX]; } P2pCmd;   /* v0.29.7: + the candidates */
typedef struct { uint32_t gen; P2pEvent ev; } P2pEvRec;

#define P2P_RING(NAME, T, CAP)                                                                                                     \
    typedef struct { volatile int64_t seq; T v; } NAME##_cell_t;                                                                   \
    static NAME##_cell_t NAME##_cells[CAP];                                                                                        \
    static volatile int64_t NAME##_enq, NAME##_deq;                                                                                \
    static void NAME##_reset(void) {                                                                                               \
        for (int64_t i = 0; i < (CAP); i++) __atomic_store_n(&NAME##_cells[i].seq, i, __ATOMIC_RELAXED);                         \
        __atomic_store_n(&NAME##_enq, 0, __ATOMIC_RELAXED); __atomic_store_n(&NAME##_deq, 0, __ATOMIC_RELEASE);                    \
    }                                                                                                                              \
    static int NAME##_push(const T *x) {   /* any number of producers; never waits (lock-free CAS on the enqueue index) */         \
        int64_t pos = __atomic_load_n(&NAME##_enq, __ATOMIC_RELAXED);                                                              \
        for (;;) {                                                                                                                 \
            NAME##_cell_t *c = &NAME##_cells[pos & ((CAP) - 1)];                                                                   \
            int64_t d = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) - pos;                                                          \
            if (d == 0) {                                                                                                          \
                if (__atomic_compare_exchange_n(&NAME##_enq, &pos, pos + 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {              \
                    c->v = *x; __atomic_store_n(&c->seq, pos + 1, __ATOMIC_RELEASE); return 1;                                     \
                }                                                                                                                  \
            } else if (d < 0) return 0;                                                                                            \
            else pos = __atomic_load_n(&NAME##_enq, __ATOMIC_RELAXED);                                                             \
        }                                                                                                                          \
    }                                                                                                                              \
    static int NAME##_pop(T *x) {          /* exactly one consumer */                                                              \
        int64_t pos = __atomic_load_n(&NAME##_deq, __ATOMIC_RELAXED);                                                              \
        NAME##_cell_t *c = &NAME##_cells[pos & ((CAP) - 1)];                                                                       \
        if (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) - (pos + 1) < 0) return 0;                                                  \
        *x = c->v; __atomic_store_n(&c->seq, pos + (CAP), __ATOMIC_RELEASE);                                                       \
        __atomic_store_n(&NAME##_deq, pos + 1, __ATOMIC_RELEASE); return 1;                                                        \
    }                                                                                                                              \
    static __attribute__((unused)) int64_t NAME##_free(void) {                                                                                          \
        return (CAP) - (__atomic_load_n(&NAME##_enq, __ATOMIC_ACQUIRE) - __atomic_load_n(&NAME##_deq, __ATOMIC_ACQUIRE));          \
    }

P2P_RING(cmdq, P2pCmd, P2P_CMDQ)
P2P_RING(evq, P2pEvRec, P2P_EVQ)

/* ---------------------------------------------------------------- shared state (game thread <-> I/O thread) */
static const P2pApi *volatile g_api_override;    /* p2p_set_api */
static P2pCfg g_cfg_p2p;                          /* written by p2p_init before the thread starts; read-only afterwards */
static volatile LONG g_inited;
static HANDLE g_thread, g_zombie;                 /* g_zombie: a thread whose shutdown wait timed out */
static volatile LONG g_stop, g_stop_exit, g_epoch;
static volatile uint32_t g_io_tid;
static volatile int g_rate_per_s, g_rate_burst;   /* p2p_set_rate_cap */

/* game-thread owned (single writer) */
static volatile uint32_t g_gen;                   /* the current open's generation (bumped by open and close) */
static volatile LONG g_is_open;
/* the CLOSE slot: reason first, then the gen (release) */
static volatile uint32_t g_close_gen, g_close_reason;
/* I/O -> game: per-open flags, each = the gen it applies to */
static volatile uint32_t g_sh_verified_gen, g_sh_budget_gen, g_sh_failed_gen, g_sh_peer_nonce;
static const char *volatile g_sh_api_name;           /* p2p_api_name(): set by io_open before its first event */
/* counters bumped outside the I/O thread (atomic adds) */
static volatile int64_t g_q_drops_total, g_q_drops_base, g_tx_refused_total, g_ev_stale_total;

/* v0.29.7: the last open's candidate counts (written by io_open before its control event, seqlock; read by p2p_peerid_result) */
static volatile int64_t g_pid_seq;
static P2pPeerIdRes g_pid;

/* published stats snapshot (seqlock: odd = being written) */
static volatile int64_t g_pub_seq;
static P2pStats g_pub_tot, g_pub_base;
static P2pStatsExt g_pub_ext;
static volatile int g_shutdown_state;

/* ---------------------------------------------------------------- time */
static LARGE_INTEGER g_qpf;
static uint64_t us_now(void) {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e6 / (double)g_qpf.QuadPart);
}

/* ---------------------------------------------------------------- the real Steam binding (I/O thread only) */
typedef uint8_t (*FnSendP2P)(void *self, uint64_t steamid, const void *data, uint32_t len, int send_type, int channel);
typedef uint8_t (*FnAvailP2P)(void *self, uint32_t *size, int channel);
typedef uint8_t (*FnReadP2P)(void *self, void *dst, uint32_t cap, uint32_t *size, uint64_t *remote, int channel);
typedef void *(*FnGetNetworking)(void *client, int32_t user, int32_t pipe, const char *version);
typedef void *(*FnSteamClient)(void);
typedef int32_t (*FnHandle)(void);
/* v0.29.1 (review M1 / m3), VERIFIED read-only on the game's steam_api64.dll (pefile + capstone, 2026-10-13):
 *   SteamAPI_ISteamClient_GetISteamUser rva 0x4e20 `mov rax,[rcx]; jmp [rax+0x28]` (client, HSteamUser, HSteamPipe, version);
 *   SteamAPI_ISteamUser_GetSteamID rva 0x52f0 calls ISteamUser vtbl+0x10 with a hidden return buffer and returns its uint64 in rax;
 *   SteamAPI_ISteamNetworking_CloseP2PChannelWithUser rva 0x47f0 -> ISteamNetworking vtbl+0x28 (this, CSteamID, channel), bool in al
 *   (CSteamID by value: the same HYPOTHESIS Q2 as SendP2PPacket). "SteamUser019" is the version the game itself asks for (T-M2-11). */
typedef void *(*FnGetUser)(void *client, int32_t user, int32_t pipe, const char *version);
typedef uint64_t (*FnGetSteamID)(void *user_iface);
typedef uint8_t (*FnCloseCh)(void *self, uint64_t steamid, int channel);
/* v0.29.7 (OPTA-M2-SPEC.md §18.10), VERIFIED read-only on the game's steam_api64.dll (pefile + capstone, 2026-10-21):
 *   SteamAPI_ISteamNetworking_GetP2PSessionState rva 0x4560 `sub rsp,28h; mov rax,[rcx]; call [rax+30h]; add rsp,28h; ret`
 *   = ISteamNetworking vtbl+0x30 (this, CSteamID by value, P2PSessionState_t *), bool in al. P2PSessionState_t (SDK 1.39, 20 bytes):
 *   uint8 m_bConnectionActive, m_bConnecting, m_eP2PSessionError, m_bUsingRelay; int32 bytes / packets queued; uint32 m_nRemoteIP;
 *   uint16 m_nRemotePort. p2p.c reads bytes 0 and 1 only and zeroes the buffer after the call (the IP never leaves it; never logged).
 *   CSteamID by value: the same HYPOTHESIS Q2 as SendP2PPacket (a wrong ABI = no session found = BADID: fail closed, never a wrong send). */
typedef uint8_t (*FnSessState)(void *self, uint64_t steamid, void *state);
static struct {
    FnSendP2P send; FnAvailP2P avail; FnReadP2P read; FnGetNetworking getnet; FnSteamClient client; FnHandle user, pipe;
    FnGetUser getuser; FnGetSteamID steamid; FnCloseCh closech; FnSessState sessst;
    void *net, *useri;
} g_st;
static uint8_t st_send(void *n, uint64_t id, const void *d, uint32_t len, int t, int ch) { return g_st.send(n, id, d, len, t, ch); }
static uint8_t st_avail(void *n, uint32_t *sz, int ch) { return g_st.avail(n, sz, ch); }
static uint8_t st_read(void *n, void *dst, uint32_t cap, uint32_t *sz, uint64_t *rid, int ch) { return g_st.read(n, dst, cap, sz, rid, ch); }
static void *st_networking(void) { return g_st.net; }
static uint64_t st_local_id(void *n) { (void)n; return g_st.useri ? g_st.steamid(g_st.useri) : 0; }
static uint8_t st_close(void *n, uint64_t id, int ch) { return g_st.closech(n, id, ch); }
static uint8_t st_session(void *n, uint64_t id, uint8_t *active, uint8_t *connecting) {   /* v0.29.7 */
    uint8_t buf[64];                       /* P2PSessionState_t is 20 bytes; the slack covers any packing */
    memset(buf, 0, sizeof buf);
    uint8_t r = g_st.sessst(n, id, buf);
    *active = buf[0]; *connecting = buf[1];
    memset(buf, 0, sizeof buf);            /* the remote IP / port are not kept */
    __asm__ volatile("" : : "r"(buf) : "memory");   /* the clear is not optimized away */
    return r;
}
static const P2pApi k_steam_api = { st_networking, st_send, st_avail, st_read, "steam", st_local_id, st_close, st_session };

static void *gpa(HMODULE m, const char *name) { FARPROC f = GetProcAddress(m, name); void *p; memcpy(&p, &f, sizeof p); return p; }
#define GPA_INTO(dst, m, name) do { void *p_ = gpa((m), (name)); if (!p_) return P2P_NS_EXPORT; memcpy(&(dst), &p_, sizeof p_); } while (0)

/* steam_api64.dll as the game loaded it (M2-INTERFACES.md §4.1, verified read-only: timestamp 0x585894b1, the flat wrappers
 * forward every argument unchanged to ISteamNetworking vtbl +0x00 / +0x08 / +0x10 and ISteamClient vtbl +0x80). Returns P2P_NS_*. */
static int steam_resolve(void) {
    memset(&g_st, 0, sizeof g_st);
    HMODULE m = GetModuleHandleA("steam_api64.dll");   /* never LoadLibrary: only the copy the game already loaded */
    if (!m) return P2P_NS_NO_MODULE;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)(const void *)m;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < (LONG)sizeof *dos || dos->e_lfanew > 0x1000) return P2P_NS_DLL_TS;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(const void *)((const uint8_t *)m + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != P2P_STEAM_DLL_TS) return P2P_NS_DLL_TS;
    GPA_INTO(g_st.send, m, "SteamAPI_ISteamNetworking_SendP2PPacket");
    GPA_INTO(g_st.avail, m, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
    GPA_INTO(g_st.read, m, "SteamAPI_ISteamNetworking_ReadP2PPacket");
    GPA_INTO(g_st.getnet, m, "SteamAPI_ISteamClient_GetISteamNetworking");
    GPA_INTO(g_st.client, m, "SteamClient");
    GPA_INTO(g_st.user, m, "SteamAPI_GetHSteamUser");
    GPA_INTO(g_st.pipe, m, "SteamAPI_GetHSteamPipe");
    GPA_INTO(g_st.getuser, m, "SteamAPI_ISteamClient_GetISteamUser");                 /* v0.29.1 (review M1) */
    GPA_INTO(g_st.steamid, m, "SteamAPI_ISteamUser_GetSteamID");
    GPA_INTO(g_st.closech, m, "SteamAPI_ISteamNetworking_CloseP2PChannelWithUser");  /* v0.29.1 (review m3) */
    GPA_INTO(g_st.sessst, m, "SteamAPI_ISteamNetworking_GetP2PSessionState");        /* v0.29.7: the candidate check (missing: NOSTEAM export) */
    void *client = g_st.client();
    if (!client) return P2P_NS_NO_CLIENT;
    int32_t user = g_st.user(), pipe = g_st.pipe();
    if (!user || !pipe) return P2P_NS_NO_CLIENT;
    void *net = g_st.getnet(client, user, pipe, "SteamNetworking005");
    if (!net) return P2P_NS_NO_NETWORKING;
    void *ui = g_st.getuser(client, user, pipe, "SteamUser019");   /* v0.29.1 (review M1): the local SteamID for the self check */
    if (!ui) return P2P_NS_NO_USER;
    g_st.net = net; g_st.useri = ui;
    return P2P_NS_NONE;
}

/* ---------------------------------------------------------------- I/O thread state */
typedef struct { int used; uint32_t seq, sends; uint64_t first_us, last_us; RbmMsg m; } OutSlot;
enum { IO_IDLE = 0, IO_ACTIVE = 1, IO_FAILED = 2 };
static struct {
    int state;
    uint32_t gen;
    uint64_t peer;
    uint32_t my_nonce, peer_nonce;
    int peer_nonce_known;
    const P2pApi *api; void *net;
    int verified, budget_out, silent_reported, restart_reported;
    uint32_t probes;                       /* datagrams sent this open before verification */
    uint64_t open_us, last_valid_us, last_probe_us, ack_due_us;
    uint32_t tx_seq, out_lo, n_out;
    OutSlot out[P2P_MAX_INFLIGHT];
    RbmMsg unseq[P2P_UNSEQQ]; int unseq_head, unseq_n;
    uint32_t rx_cum;
    uint32_t rx_bm[P2P_RX_WINDOW / 32];
    double tokens; uint64_t tok_us; int rate_per_s, rate_burst;
    uint64_t corrupt_ctr;
    int log_n;
    P2pEvRec ctl[P2P_CTL_PENDING]; int ctl_n;
    uint32_t closed_seen;
} io;
static P2pStats io_tot, io_base;   /* process totals and their value at the last open (I/O thread only) */
/* v0.29.1 (review m3): the channel of the last ACTIVE open, closed with CloseP2PChannelWithUser a little after its BYEs (I/O thread only) */
static struct { const P2pApi *api; void *net; uint64_t peer, due_us; int pending; } io_chc;
static P2pStatsExt io_ext;
static int g_log_proc;
static uint8_t g_rxbuf[P2P_RXBUF];

static void plog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void plog(const char *fmt, ...) {
    if (io.log_n >= P2P_LOG_PER_OPEN || g_log_proc >= P2P_LOG_PER_PROC) return;
    io.log_n++; g_log_proc++;
    char b[256];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    LOG("%s\n", b);
}
static const char *why_name(int w) { return (w >= 0 && w < RBW_N && k_rbw_name[w]) ? k_rbw_name[w] : "?"; }   /* v0.29.3 (m7): NULL-safe */

static void publish(void) {
    io_tot.unacked = io.n_out;
    io_tot.verified = (uint32_t)io.verified;
    io_tot.open = io.state == IO_ACTIVE;
    io_tot.api_steam = (io.state == IO_ACTIVE && io.api == &k_steam_api);
    io_ext.peer_nonce = io.peer_nonce_known ? io.peer_nonce : 0;
    __atomic_add_fetch(&g_pub_seq, 1, __ATOMIC_ACQ_REL);   /* odd: writing */
    g_pub_tot = io_tot; g_pub_base = io_base; g_pub_ext = io_ext;
    __atomic_add_fetch(&g_pub_seq, 1, __ATOMIC_RELEASE);   /* even: stable */
}
static void pid_publish(const P2pPeerIdRes *r) {   /* I/O thread; before the open's control event is produced */
    __atomic_add_fetch(&g_pid_seq, 1, __ATOMIC_ACQ_REL);   /* odd: writing */
    g_pid = *r;
    __atomic_add_fetch(&g_pid_seq, 1, __ATOMIC_RELEASE);   /* even: stable */
}
static int snapshot(P2pStats *tot, P2pStats *base, P2pStatsExt *ext) {   /* bounded retries: never waits */
    for (int tries = 0; tries < 256; tries++) {
        int64_t s1 = __atomic_load_n(&g_pub_seq, __ATOMIC_ACQUIRE);
        if (s1 & 1) { YieldProcessor(); continue; }
        if (tot) *tot = g_pub_tot;
        if (base) *base = g_pub_base;
        if (ext) *ext = g_pub_ext;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_pub_seq, __ATOMIC_RELAXED) == s1) return 1;
    }
    return 0;   /* a torn copy (the writer was busy for 256 tries); the caller still gets numbers */
}

/* ---------------------------------------------------------------- events (I/O thread = the only producer) */
static int ev_push_raw(int kind, int detail, const RbmMsg *m) {
    P2pEvRec r; memset(&r, 0, sizeof r);
    r.gen = io.gen; r.ev.kind = kind; r.ev.detail = detail;
    if (m) r.ev.msg = *m;
    return evq_push(&r);
}
static void ev_ctl(int kind, int detail) {   /* control events never get lost: they wait in a short list when the ring is full */
    if (io.ctl_n == 0 && ev_push_raw(kind, detail, NULL)) return;
    if (io.ctl_n < P2P_CTL_PENDING) {
        P2pEvRec *r = &io.ctl[io.ctl_n++]; memset(r, 0, sizeof *r);
        r->gen = io.gen; r->ev.kind = kind; r->ev.detail = detail;
        io_ext.ev_ctl_pending++;
    }
}
static void ev_ctl_flush(void) {
    int k = 0;
    while (k < io.ctl_n && evq_push(&io.ctl[k])) k++;
    if (k) { memmove(io.ctl, io.ctl + k, (size_t)(io.ctl_n - k) * sizeof io.ctl[0]); io.ctl_n -= k; }
}

/* ---------------------------------------------------------------- receive window */
static int bm_get(uint32_t s) { uint32_t i = s % P2P_RX_WINDOW; return (int)((io.rx_bm[i >> 5] >> (i & 31)) & 1u); }
static void bm_put(uint32_t s, int v) {
    uint32_t i = s % P2P_RX_WINDOW;
    if (v) io.rx_bm[i >> 5] |= 1u << (i & 31); else io.rx_bm[i >> 5] &= ~(1u << (i & 31));
}
static uint32_t rx_ack_bits(void) {
    uint32_t b = 0;
    for (uint32_t i = 0; i < 32; i++) if (bm_get(io.rx_cum + 2 + i)) b |= 1u << i;
    return b;
}
/* 1 = new, 0 = duplicate, -1 = beyond the window */
static int rx_mark(uint32_t s) {
    if (s <= io.rx_cum) return 0;
    if (s - io.rx_cum > P2P_RX_WINDOW) return -1;
    if (bm_get(s)) return 0;
    bm_put(s, 1);
    while (bm_get(io.rx_cum + 1)) { bm_put(io.rx_cum + 1, 0); io.rx_cum++; }
    return 1;
}

/* ---------------------------------------------------------------- transmit */
static OutSlot *slot_of(uint32_t s) { return &io.out[s % P2P_MAX_INFLIGHT]; }
static void out_advance_lo(void) {
    while (io.out_lo <= io.tx_seq) { OutSlot *o = slot_of(io.out_lo); if (o->used && o->seq == io.out_lo) break; io.out_lo++; }
}
static size_t enc(uint8_t *dg, const RbmMsg *m, uint32_t seq) {
    RbmHdr h; memset(&h, 0, sizeof h);
    h.nonce_tx = io.my_nonce; h.nonce_rx = io.peer_nonce_known ? io.peer_nonce : 0;
    h.seq = seq; h.ack = io.rx_cum; h.ack_bits = rx_ack_bits();
    return rbm_encode(dg, RBM_MAXDG, &h, m);
}
static int take_token(uint64_t now) {
    if (io.tok_us) {
        io.tokens += (double)(now - io.tok_us) * (double)io.rate_per_s / 1e6;
        if (io.tokens > (double)io.rate_burst) io.tokens = (double)io.rate_burst;
    }
    io.tok_us = now;
    if (io.tokens >= 1.0) { io.tokens -= 1.0; return 1; }
    io_ext.tx_rate_deferred++;
    return 0;
}
static void tx_raw(uint8_t *dg, size_t n) {
    if (g_cfg_p2p.h_corrupt_every > 0 && (++io.corrupt_ctr % (uint64_t)g_cfg_p2p.h_corrupt_every) == 0) dg[28] ^= 0x01;   /* plant */
    uint8_t ok = io.api->send(io.net, io.peer, dg, (uint32_t)n, P2P_SEND_UNRELIABLE, g_cfg_p2p.channel);
    io_tot.tx_dgrams++; io_tot.tx_bytes += n;
    if (!io.verified) { io_tot.tx_probe++; io.probes++; }
    if (!ok) io_ext.tx_fail++;
    io.ack_due_us = 0;   /* this datagram carried the current ack */
}
static int send_slot(OutSlot *o, uint64_t now) {
    uint8_t dg[RBM_MAXDG];
    size_t n = enc(dg, &o->m, o->seq);
    if (!n) return 0;
    tx_raw(dg, n);
    if (o->sends) io_tot.tx_resends++; else o->first_us = now;
    o->sends++; o->last_us = now;
    return 1;
}

static void io_tx(uint64_t now) {
    if (io.state != IO_ACTIVE || io.budget_out) return;
    const uint64_t resend_us = (uint64_t)g_cfg_p2p.resend_ms * 1000u, gap_us = (uint64_t)g_cfg_p2p.probe_gap_ms * 1000u;
    if (!io.verified) {
        /* the unverified budget: only HELLO (p2p_send refuses the rest), one datagram per probe_gap_ms, probe_max per open */
        if (io.n_out == 0) return;
        if (io.last_probe_us && now - io.last_probe_us < gap_us) return;
        if (io.probes >= (uint32_t)g_cfg_p2p.probe_max) {
            if (!io.budget_out) {
                io.budget_out = 1; io_ext.budget++;
                __atomic_store_n(&g_sh_budget_gen, io.gen, __ATOMIC_RELEASE);
                ev_ctl(P2P_EV_BUDGET, 0);
                plog("P2P BUDGET gen=%u probes=%u", io.gen, io.probes);
            }
            return;
        }
        OutSlot *best = NULL;
        for (uint32_t s = io.out_lo; s <= io.tx_seq; s++) {
            OutSlot *o = slot_of(s);
            if (!o->used || o->seq != s || o->m.type != RBM_HELLO) continue;
            if (g_cfg_p2p.h_no_resend && o->sends) continue;
            if (!best || o->sends < best->sends) best = o;
        }
        if (best && take_token(now) && send_slot(best, now)) io.last_probe_us = now;
        return;
    }
    for (uint32_t s = io.out_lo; s <= io.tx_seq; s++) {
        OutSlot *o = slot_of(s);
        if (!o->used || o->seq != s) continue;
        if (o->sends && (g_cfg_p2p.h_no_resend || now - o->last_us < resend_us)) continue;
        if (!take_token(now)) return;
        send_slot(o, now);
    }
    while (io.unseq_n) {
        if (!take_token(now)) return;
        uint8_t dg[RBM_MAXDG];
        size_t n = enc(dg, &io.unseq[io.unseq_head], 0);
        io.unseq_head = (io.unseq_head + 1) % P2P_UNSEQQ; io.unseq_n--;
        if (n) tx_raw(dg, n);
    }
    if (io.ack_due_us && now >= io.ack_due_us) {
        if (!take_token(now)) return;
        uint8_t dg[RBM_MAXDG];
        size_t n = enc(dg, NULL, 0);
        if (n) { tx_raw(dg, n); io_tot.tx_acks++; }
    }
}

static void io_enqueue(const P2pCmd *c) {
    if (c->gen != io.gen || io.state != IO_ACTIVE || io.budget_out) { io_ext.cmd_stale++; return; }
    const RbmMsg *m = &c->msg;
    if (m->type == RBM_BEACON || m->type == RBM_TSYNC) {   /* v0.30.0: + TSYNC (unsequenced: sent once, never acked or resent) */
        if (io.unseq_n >= P2P_UNSEQQ) { io_ext.tx_unseq_drops++; return; }
        io.unseq[(io.unseq_head + io.unseq_n) % P2P_UNSEQQ] = *m; io.unseq_n++;
        return;
    }
    if (!rbm_sequenced(m->type)) { io_ext.cmd_stale++; return; }
    uint32_t s = io.tx_seq + 1;
    OutSlot *o = slot_of(s);
    if (o->used) { io_ext.tx_window_drops++; return; }
    memset(o, 0, sizeof *o);
    o->used = 1; o->seq = s; o->m = *m; o->m.seq = s;
    io.tx_seq = s; io.n_out++;
    if (io.out_lo == 0) io.out_lo = 1;
}

static void io_acked(uint32_t ack, uint32_t bits) {
    if (ack > io.tx_seq) { io_tot.rx_unsolicited++; ack = io.tx_seq; }   /* acks a seq we never sent: protocol confusion, clamp */
    for (uint32_t s = io.out_lo; s <= io.tx_seq && s; s++) {
        OutSlot *o = slot_of(s);
        if (!o->used || o->seq != s) continue;
        int acked = s <= ack;
        if (!acked && s >= ack + 2 && s - ack - 2 < 32) acked = (int)((bits >> (s - ack - 2)) & 1u);
        if (acked) { o->used = 0; io.n_out--; }
    }
    out_advance_lo();
}

/* ---------------------------------------------------------------- receive */
static int size_plausible(uint32_t sz) {
    for (uint8_t t = 1; t < RBM_NTYPES; t++) if (sz == RBM_HDR + rbm_len_of(t)) return 1;
    return 0;
}
static void rx_one(const uint8_t *dg, uint32_t n, uint64_t rid, uint64_t now) {
    io_tot.rx_dgrams++;
    if (io.budget_out) { io_ext.rx_after_budget++; return; }
    if (rid != io.peer && !g_cfg_p2p.h_accept_any_id) { io_tot.rx_foreign_id++; return; }   /* never logged with the id */
    RbmHdr h; RbmMsg m;
    int rc = rbm_decode(dg, n, &h, &m);
    if (rc != RBM_OK) { io_tot.rx_bad++; if (rc > 0 && rc < RBM_NERR) io_tot.bad_err[rc]++; return; }
    if (h.nonce_tx == 0 || (h.nonce_rx != 0 && h.nonce_rx != io.my_nonce)) { io_tot.rx_stale_nonce++; return; }
    if (h.nonce_tx == io.my_nonce && !g_cfg_p2p.h_no_self_check) { io_ext.rx_self++; return; }   /* v0.29.1 (review M1): our own datagram looped back */
    if (!io.peer_nonce_known) {
        io.peer_nonce = h.nonce_tx; io.peer_nonce_known = 1;
        __atomic_store_n(&g_sh_peer_nonce, h.nonce_tx, __ATOMIC_RELEASE);
    } else if (h.nonce_tx != io.peer_nonce) {
        if (h.nonce_rx == 0 && !io.restart_reported) {   /* a new nonce that has not seen ours: the peer's plugin restarted */
            io.restart_reported = 1; io_ext.restart++;
            ev_push_raw(P2P_EV_PEER_RESTART, 0, NULL);
            plog("P2P RESTART gen=%u", io.gen);
        }
        io_tot.rx_stale_nonce++;
        return;
    }
    io_tot.rx_ok++;
    io.last_valid_us = now;
    if (!io.verified) {
        io.verified = 1; io_ext.verified++;
        __atomic_store_n(&g_sh_verified_gen, io.gen, __ATOMIC_RELEASE);   /* before the event: p2p_send allows every type from here */
        ev_push_raw(P2P_EV_VERIFIED, 0, NULL);
        plog("P2P VERIFIED gen=%u ms=%.1f probes=%u", io.gen, (double)(now - io.open_us) / 1000.0, io.probes);
    }
    io_acked(h.ack, h.ack_bits);
    if (h.type == RBM_ACK) return;
    if (h.type == RBM_BEACON || h.type == RBM_TSYNC) { ev_push_raw(P2P_EV_MSG, (int)h.nonce_tx, &m); return; }   /* v0.30.0: + TSYNC, delivered as received */
    /* sequenced */
    int r = rx_mark(h.seq);
    if (r < 0) { io_ext.rx_window_drops++; return; }
    if (!io.ack_due_us) {
        uint64_t d = (uint64_t)g_cfg_p2p.resend_ms * 1000u / 4u;
        if (d > (uint64_t)P2P_ACK_DELAY_MS * 1000u) d = (uint64_t)P2P_ACK_DELAY_MS * 1000u;
        io.ack_due_us = now + d;
    }
    if (r == 0) { io_tot.rx_dup++; return; }
    ev_push_raw(P2P_EV_MSG, (int)h.nonce_tx, &m);
}
static void io_rx(uint64_t now) {
    if (io.state != IO_ACTIVE) return;
    for (int k = 0; k < P2P_RX_PER_IT; k++) {
        if (evq_free() < P2P_EV_RESERVE) { io_ext.rx_backpressure++; return; }
        uint32_t sz = 0;
        if (!io.api->avail(io.net, &sz, g_cfg_p2p.channel)) return;
        uint32_t got = 0; uint64_t rid = 0;
        if (!io.api->read(io.net, g_rxbuf, (uint32_t)sizeof g_rxbuf, &got, &rid, g_cfg_p2p.channel)) { io_ext.rx_read_fail++; return; }
        if (got > sizeof g_rxbuf) got = sizeof g_rxbuf;
        rx_one(g_rxbuf, got, rid, now);
    }
}

/* ---------------------------------------------------------------- open / close */
static void reset_open_state(void) {
    io.state = IO_IDLE; io.peer = 0; io.my_nonce = io.peer_nonce = 0; io.peer_nonce_known = 0;
    io.api = NULL; io.net = NULL;
    io.verified = io.budget_out = io.silent_reported = io.restart_reported = 0;
    io.probes = 0; io.open_us = io.last_valid_us = io.last_probe_us = io.ack_due_us = 0;
    io.tx_seq = 0; io.out_lo = 1; io.n_out = 0;
    memset(io.out, 0, sizeof io.out);
    io.unseq_head = io.unseq_n = 0;
    io.rx_cum = 0; memset(io.rx_bm, 0, sizeof io.rx_bm);
    io.corrupt_ctr = 0; io.log_n = 0;
    __atomic_store_n(&g_sh_peer_nonce, 0, __ATOMIC_RELEASE);
}

static void io_close(int why, int send_bye) {
    if (io.state == IO_IDLE) return;
    int byes = 0;
    if (io.state == IO_ACTIVE && io.verified && !io.budget_out && send_bye) {
        RbmMsg m; memset(&m, 0, sizeof m);
        m.type = RBM_BYE; m.u.bye.reason = (uint8_t)why;
        uint8_t dg[RBM_MAXDG];
        size_t n = enc(dg, &m, io.tx_seq + 1);   /* the next seq; sent three times, never resent, never waited for */
        if (n) {
            for (int k = 0; k < 3; k++) { uint8_t c[RBM_MAXDG]; memcpy(c, dg, n); tx_raw(c, n); byes++; }
            io.tx_seq++;
        }
        io_ext.tx_bye += (uint64_t)byes;
    }
    io_ext.closes++;
    if (io.state == IO_ACTIVE && io.api && io.api->close_channel && io.net) {   /* v0.29.1 (review m3): our channel, a little after the BYEs */
        io_chc.api = io.api; io_chc.net = io.net; io_chc.peer = io.peer; io_chc.due_us = us_now() + (uint64_t)P2P_CHCLOSE_DELAY_MS * 1000u; io_chc.pending = 1;
    }
    plog("P2P CLOSE gen=%u why=%s bye=%d tx=%llu rx=%llu resends=%llu unacked=%u verified=%d", io.gen, why_name(why), byes,
         (unsigned long long)(io_tot.tx_dgrams - io_base.tx_dgrams), (unsigned long long)(io_tot.rx_dgrams - io_base.rx_dgrams),
         (unsigned long long)(io_tot.tx_resends - io_base.tx_resends), io.n_out, io.verified);
    uint32_t gen = io.gen;
    reset_open_state();
    io.gen = gen;          /* keeps stamping nothing: no event is produced while IDLE */
    io.ctl_n = 0;          /* pending control events of the closed open are dropped */
}

static void chc_run(uint64_t now, int force) {   /* the pending CloseP2PChannelWithUser (I/O thread) */
    if (!io_chc.pending || (!force && now < io_chc.due_us)) return;
    io_chc.pending = 0;
    io_chc.api->close_channel(io_chc.net, io_chc.peer, g_cfg_p2p.channel);
    io_ext.ch_close++;
}
static void io_open(const P2pCmd *c, uint64_t now) {
    if (io.state != IO_IDLE) io_close(RBW_INTERNAL, 1);   /* an open without a close: close the old one first */
    chc_run(now, 1);                                      /* v0.29.1: the previous open's channel close never lands inside this open */
    io_base = io_tot;
    reset_open_state();
    io.gen = c->gen; io.peer = c->peer; io.my_nonce = c->nonce; io.open_us = now;
    io.rate_per_s = g_rate_per_s > 0 ? g_rate_per_s : P2P_RATE_PER_S;
    io.rate_burst = g_rate_burst > 0 ? g_rate_burst : P2P_RATE_BURST;
    io.tokens = (double)io.rate_burst; io.tok_us = now;
    io_ext.opens++;
    const P2pApi *ov = g_api_override;
    int ns = P2P_NS_NONE;
    if (ov) {
        io.api = ov; io.net = ov->networking ? ov->networking() : NULL;
        if (!io.net) ns = P2P_NS_NO_NETWORKING;
    } else {
        ns = steam_resolve();
        if (ns == P2P_NS_NONE) { io.api = &k_steam_api; io.net = g_st.net; }
    }
    io_ext.last_ns = ns;
    const char *apiname = ov ? (ov->name ? ov->name : "?") : ns == P2P_NS_NONE ? "steam" : "none";
    __atomic_store_n(&g_sh_api_name, apiname, __ATOMIC_RELEASE);   /* before OPENED / NOSTEAM / FOREIGN is pushed */
    uint32_t nc = c->ncand > P2P_CAND_MAX ? P2P_CAND_MAX : c->ncand;
    P2pPeerIdRes pr; memset(&pr, 0, sizeof pr);
    pr.gen = io.gen; pr.n = (uint8_t)nc;
    uint64_t me = 0;
    if (ns == P2P_NS_NONE && !g_cfg_p2p.h_no_self_check) {   /* v0.29.1 (review M1): the local SteamID must be known (fail closed) */
        me = io.api->local_id ? io.api->local_id(io.net) : 0;
        if (!me) ns = P2P_NS_NO_USER;
    }
    if (ns != P2P_NS_NONE) {
        pid_publish(&pr);   /* ran = 0: no candidate was checked */
        io.state = IO_FAILED; io_ext.nosteam++;
        __atomic_store_n(&g_sh_failed_gen, io.gen, __ATOMIC_RELEASE);
        ev_ctl(P2P_EV_NOSTEAM, ns);
        plog("P2P OPEN gen=%u api=%s ns=%s result=nosteam", io.gen, apiname, k_p2p_ns[ns]);
        return;
    }
    /* v0.29.7 (OPTA-M2-SPEC.md §18.10): the candidates, in order. A candidate is the peer only if it is not this PC's own SteamID and Steam
     * has a P2P session with it (the user the game itself talks to). Two different passing ids: none is chosen (ambiguous). Steam queries
     * only, on this thread; nothing is sent before a peer is chosen; no id is ever logged (counts only). */
    pr.ran = 1; pr.sess_api = io.api->session_state != NULL;
    int chosen = -1; uint32_t distinct = 0;
    for (uint32_t i = 0; i < nc; i++) {
        uint64_t id = c->cands[i];
        int dup = 0;
        for (uint32_t j = 0; j < i; j++) if (c->cands[j] == id) dup = 1;
        if (dup) continue;   /* the same id from two sources is one candidate */
        distinct++;
        if (!g_cfg_p2p.h_no_self_check && id == me) { pr.self++; continue; }
        int ok = 0;
        if (g_cfg_p2p.h_no_sess_check) ok = 1;   /* harness control: the session check off */
        else if (io.api->session_state) {
            uint8_t act = 0, conn = 0;
            uint8_t r = io.api->session_state(io.net, id, &act, &conn);
            io_ext.sess_calls++;
            ok = r && (act || conn);
        }
        if (!ok) continue;
        pr.sess++;
        if (chosen < 0) chosen = (int)i;
        else if (!g_cfg_p2p.h_amb_first) pr.ambiguous = 1;
    }
    if (chosen >= 0 && !pr.ambiguous) {
        pr.chosen = (uint8_t)(chosen + 1);
        io.peer = c->cands[chosen];
        pid_publish(&pr);
    } else if (pr.self > 0 && pr.self == distinct) {
        /* every candidate is this PC itself (v0.29.1's SELF): never send, never pair (no id is logged) */
        pid_publish(&pr);
        io.state = IO_FAILED; io_ext.self++;
        __atomic_store_n(&g_sh_failed_gen, io.gen, __ATOMIC_RELEASE);
        ev_ctl(P2P_EV_SELF, 0);
        plog("P2P OPEN gen=%u api=%s ns=- result=self (the expected peer id is this PC's own: nothing sent)", io.gen, apiname);
        return;
    } else {
        pid_publish(&pr);
        io.state = IO_FAILED; io_ext.badid++;
        __atomic_store_n(&g_sh_failed_gen, io.gen, __ATOMIC_RELEASE);
        ev_ctl(P2P_EV_BADID, 0);
        plog("P2P OPEN gen=%u api=%s ns=- cand=%u self=%u sess=%u ambiguous=%u sess_api=%u result=badid (nothing sent)", io.gen, apiname,
             (unsigned)pr.n, (unsigned)pr.self, (unsigned)pr.sess, (unsigned)pr.ambiguous, (unsigned)pr.sess_api);
        return;
    }
    /* the guard (T-M2-10): what is already waiting on our channel */
    uint32_t drained = 0, stale = 0, foreign = 0;
    for (int k = 0; k < P2P_DRAIN_MAX; k++) {
        uint32_t sz = 0;
        if (!io.api->avail(io.net, &sz, g_cfg_p2p.channel)) break;
        drained++; io_ext.open_drained++;
        int is_foreign = 0;
        if (!size_plausible(sz)) {
            is_foreign = 1;
            if (!g_cfg_p2p.h_no_guard) { foreign++; io_ext.open_foreign++; break; }   /* not read: never consume another program's packet */
        }
        uint32_t got = 0; uint64_t rid = 0;
        if (!io.api->read(io.net, g_rxbuf, (uint32_t)sizeof g_rxbuf, &got, &rid, g_cfg_p2p.channel)) {   /* v0.29.1 (review m3): fail closed */
            io_ext.rx_read_fail++; foreign++; io_ext.open_foreign++;
            if (!g_cfg_p2p.h_no_guard) break;
            continue;
        }
        if (got > sizeof g_rxbuf) got = sizeof g_rxbuf;
        io_tot.rx_dgrams++;
        if (!is_foreign) {
            if (rid != io.peer) { is_foreign = 1; io_tot.rx_foreign_id++; }
            else {
                RbmHdr h; int rc = rbm_decode(g_rxbuf, got, &h, NULL);
                if (rc != RBM_OK) { is_foreign = 1; io_tot.rx_bad++; if (rc > 0 && rc < RBM_NERR) io_tot.bad_err[rc]++; }
            }
        } else io_tot.rx_bad++;
        if (is_foreign) {
            foreign++; io_ext.open_foreign++;
            if (!g_cfg_p2p.h_no_guard) break;
            continue;
        }
        stale++; io_ext.open_stale++; io_tot.rx_stale_nonce++;   /* ours, from the expected id, older than this open's nonce */
    }
    if (foreign && !g_cfg_p2p.h_no_guard) {
        io.state = IO_FAILED; io_ext.foreign++;
        __atomic_store_n(&g_sh_failed_gen, io.gen, __ATOMIC_RELEASE);
        ev_ctl(P2P_EV_FOREIGN, 0);
        plog("P2P OPEN gen=%u api=%s ns=- drained=%u stale=%u foreign=%u result=foreign", io.gen, apiname, drained, stale, foreign);
        return;
    }
    io.state = IO_ACTIVE;
    ev_ctl(P2P_EV_OPENED, 0);
    plog("P2P OPEN gen=%u api=%s ns=- drained=%u stale=%u foreign=%u result=opened", io.gen, apiname, drained, stale, foreign);
}

/* ---------------------------------------------------------------- the I/O thread */
static void io_timers(uint64_t now) {
    if (io.state != IO_ACTIVE) return;
    if (io.verified && !io.silent_reported && now - io.last_valid_us >= (uint64_t)g_cfg_p2p.peer_timeout_ms * 1000u) {
        io.silent_reported = 1; io_ext.silent++;
        ev_ctl(P2P_EV_SILENT, 0);
        plog("P2P SILENT gen=%u ms_since_rx=%.0f unacked=%u", io.gen, (double)(now - io.last_valid_us) / 1000.0, io.n_out);
    }
}
static void io_iteration(void) {
    uint64_t now = us_now();
    ev_ctl_flush();
    uint32_t cg = __atomic_load_n(&g_close_gen, __ATOMIC_ACQUIRE);
    if (cg != io.closed_seen) {
        io.closed_seen = cg;
        if (io.state != IO_IDLE && io.gen < cg) io_close((int)__atomic_load_n(&g_close_reason, __ATOMIC_RELAXED), 1);
    }
    P2pCmd c;
    for (int k = 0; k < P2P_CMDQ && cmdq_pop(&c); k++) {
        if (c.kind == CMD_OPEN) {
            if (c.gen < __atomic_load_n(&g_close_gen, __ATOMIC_ACQUIRE)) { io_ext.cmd_stale++; continue; }   /* closed before we saw it */
            io_open(&c, now);
        } else if (c.kind == CMD_SEND) io_enqueue(&c);
    }
    io_rx(now);
    now = us_now();
    io_timers(now);
    io_tx(now);
    chc_run(now, 0);
    publish();
}

static DWORD WINAPI io_main(void *arg) {
    LONG epoch = (LONG)(intptr_t)arg;
    __atomic_store_n(&g_io_tid, (uint32_t)GetCurrentThreadId(), __ATOMIC_RELEASE);
    io_ext.io_tid = (uint32_t)GetCurrentThreadId();
    HANDLE tmr = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    publish();
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) && __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE) == epoch) {
        io_iteration();
        int ms = g_cfg_p2p.poll_ms;
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)ms * 10000;
        if (tmr && SetWaitableTimer(tmr, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(tmr, (DWORD)ms + 100);
        else Sleep((DWORD)ms);
    }
    if (!__atomic_load_n(&g_stop_exit, __ATOMIC_ACQUIRE) && io.state != IO_IDLE) {   /* a clean stop mid-battle: say goodbye */
        int pending = __atomic_load_n(&g_close_gen, __ATOMIC_ACQUIRE) > io.gen;      /* the core closed and we had not seen it yet */
        io_close(pending ? (int)__atomic_load_n(&g_close_reason, __ATOMIC_RELAXED) : RBW_ENDPLAY, 1);
    }
    if (!__atomic_load_n(&g_stop_exit, __ATOMIC_ACQUIRE)) chc_run(us_now(), 1);   /* v0.29.1: a pending channel close is not left behind */
    publish();
    if (tmr) CloseHandle(tmr);
    __atomic_store_n(&g_io_tid, 0, __ATOMIC_RELEASE);
    return 0;
}

/* ---------------------------------------------------------------- public API */
void p2p_set_api(const P2pApi *api) { g_api_override = api; }
void p2p_set_rate_cap(int per_s, int burst) { g_rate_per_s = per_s; g_rate_burst = burst; }

int p2p_init(const P2pCfg *cfg) {
    if (!cfg || g_inited) return 0;
    if (cfg->channel <= 65535 || cfg->channel == 0x484F) return 0;   /* the core refuses these first; this is the second line */
    if (g_zombie) {   /* a previous thread whose shutdown wait timed out: it must be gone before the state is reused */
        if (WaitForSingleObject(g_zombie, 10000) != WAIT_OBJECT_0) return 0;
        CloseHandle(g_zombie); g_zombie = NULL;
    }
    P2pCfg c = *cfg;
    if (c.probe_max <= 0) c.probe_max = 10;
    if (c.probe_gap_ms <= 0) c.probe_gap_ms = 500;
    if (c.resend_ms <= 0) c.resend_ms = 250;
    if (c.peer_timeout_ms <= 0) c.peer_timeout_ms = 3000;
    if (c.poll_ms <= 0) c.poll_ms = 5;
    if (c.poll_ms > 100) c.poll_ms = 100;
    g_cfg_p2p = c;
    QueryPerformanceFrequency(&g_qpf);
    (void)np_crc32("", 0, 0);   /* netproto.c fills its CRC table lazily and unsynchronized: fill it before any second thread exists */
    cmdq_reset(); evq_reset();
    memset(&io, 0, sizeof io); reset_open_state(); memset(&io_chc, 0, sizeof io_chc);
    memset(&io_tot, 0, sizeof io_tot); memset(&io_base, 0, sizeof io_base); memset(&io_ext, 0, sizeof io_ext);
    g_log_proc = 0;
    g_gen = 0; g_is_open = 0; g_close_gen = 0; g_close_reason = 0;
    g_sh_verified_gen = g_sh_budget_gen = g_sh_failed_gen = 0; g_sh_peer_nonce = 0; g_sh_api_name = NULL;
    memset(&g_pid, 0, sizeof g_pid); g_pid_seq = 0;   /* v0.29.7 */
    g_q_drops_total = g_q_drops_base = g_tx_refused_total = g_ev_stale_total = 0;
    g_shutdown_state = 0;
    publish();
    __atomic_store_n(&g_stop, 0, __ATOMIC_RELEASE); __atomic_store_n(&g_stop_exit, 0, __ATOMIC_RELEASE);
    LONG epoch = InterlockedIncrement(&g_epoch);
    g_thread = CreateThread(NULL, 256 * 1024, io_main, (void *)(intptr_t)epoch, 0, NULL);
    if (!g_thread) return 0;
    g_inited = 1;
    return 1;
}

void p2p_shutdown(int process_exit) {
    if (!g_inited) return;
    g_inited = 0;
    g_is_open = 0;
    __atomic_store_n(&g_stop_exit, process_exit ? 1 : 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    if (!g_thread) return;
    if (process_exit) {   /* DllMain at process exit: the OS already ended the thread; never wait, never call Steam here */
        if (WaitForSingleObject(g_thread, 0) == WAIT_OBJECT_0) CloseHandle(g_thread); else g_zombie = g_thread;
        g_thread = NULL; g_shutdown_state = 3;
        return;
    }
    if (WaitForSingleObject(g_thread, P2P_SHUTDOWN_WAIT_MS) == WAIT_OBJECT_0) { CloseHandle(g_thread); g_shutdown_state = 1; }
    else { g_zombie = g_thread; g_shutdown_state = 2; }   /* stuck in a Steam call: left running, joined by the next p2p_init */
    g_thread = NULL;
}

int p2p_open(uint64_t peer_steamid, uint32_t my_nonce) { return p2p_open_cands(&peer_steamid, 1, my_nonce); }   /* v0.29.7: one candidate */
int p2p_open_cands(const uint64_t *ids, int n, uint32_t my_nonce) {
    int ok = g_inited && my_nonce && ids && n >= 1 && n <= P2P_CAND_MAX;
    for (int i = 0; ok && i < n; i++) if ((uint32_t)(ids[i] >> 32) != P2P_ID_PATTERN) ok = 0;
    if (!ok) { __atomic_add_fetch(&g_tx_refused_total, 1, __ATOMIC_RELAXED); return 0; }
    uint32_t gen = g_gen + 1;
    g_gen = gen;
    g_q_drops_base = __atomic_load_n(&g_q_drops_total, __ATOMIC_RELAXED);
    P2pCmd c; memset(&c, 0, sizeof c);
    c.kind = CMD_OPEN; c.gen = gen; c.peer = ids[0]; c.nonce = my_nonce;
    c.ncand = (uint32_t)n;
    for (int i = 0; i < n; i++) c.cands[i] = ids[i];
    if (!cmdq_push(&c)) {   /* no room: make sure the I/O thread closes whatever it has, and report failure */
        __atomic_add_fetch(&g_q_drops_total, 1, __ATOMIC_RELAXED);
        g_is_open = 0;
        g_close_reason = RBW_INTERNAL; __atomic_store_n(&g_close_gen, gen, __ATOMIC_RELEASE);
        return 0;
    }
    g_is_open = 1;
    return 1;
}

void p2p_close(int bye_reason) {
    if (!g_inited || !g_is_open) return;
    g_is_open = 0;
    uint32_t gen = g_gen + 1;
    g_gen = gen;   /* from here p2p_poll drops every event of the closed open */
    __atomic_store_n(&g_close_reason, (uint32_t)bye_reason, __ATOMIC_RELAXED);
    __atomic_store_n(&g_close_gen, gen, __ATOMIC_RELEASE);
}

int p2p_send(const RbmMsg *m) {
    uint32_t gen = g_gen;
    int ok = g_inited && m && g_is_open && m->type >= RBM_HELLO && m->type < RBM_NTYPES && m->type != RBM_ACK;
    if (ok && (__atomic_load_n(&g_sh_failed_gen, __ATOMIC_ACQUIRE) == gen || __atomic_load_n(&g_sh_budget_gen, __ATOMIC_ACQUIRE) == gen)) ok = 0;
    if (ok && m->type != RBM_HELLO && __atomic_load_n(&g_sh_verified_gen, __ATOMIC_ACQUIRE) != gen) ok = 0;   /* unverified: HELLO only */
    if (!ok) { __atomic_add_fetch(&g_tx_refused_total, 1, __ATOMIC_RELAXED); return 0; }
    P2pCmd c;
    c.kind = CMD_SEND; c.gen = gen; c.peer = 0; c.nonce = 0; c.msg = *m;
    if (!cmdq_push(&c)) { __atomic_add_fetch(&g_q_drops_total, 1, __ATOMIC_RELAXED); return 0; }
    return 1;
}

int p2p_poll(P2pEvent *ev) {
    if (!ev) return 0;
    P2pEvRec r;
    for (int k = 0; k < P2P_EVQ && evq_pop(&r); k++) {
        if (r.gen != g_gen) { __atomic_add_fetch(&g_ev_stale_total, 1, __ATOMIC_RELAXED); continue; }
        *ev = r.ev;
        return 1;
    }
    return 0;
}

void p2p_stats(P2pStats *out) {
    if (!out) return;
    P2pStats t, b;
    snapshot(&t, &b, NULL);
    memset(out, 0, sizeof *out);
    out->tx_dgrams = t.tx_dgrams - b.tx_dgrams; out->tx_bytes = t.tx_bytes - b.tx_bytes; out->tx_resends = t.tx_resends - b.tx_resends;
    out->tx_acks = t.tx_acks - b.tx_acks; out->tx_probe = t.tx_probe - b.tx_probe;
    out->rx_dgrams = t.rx_dgrams - b.rx_dgrams; out->rx_ok = t.rx_ok - b.rx_ok; out->rx_dup = t.rx_dup - b.rx_dup; out->rx_bad = t.rx_bad - b.rx_bad;
    out->rx_foreign_id = t.rx_foreign_id - b.rx_foreign_id; out->rx_stale_nonce = t.rx_stale_nonce - b.rx_stale_nonce;
    out->rx_unsolicited = t.rx_unsolicited - b.rx_unsolicited;
    out->q_drops = (uint64_t)(__atomic_load_n(&g_q_drops_total, __ATOMIC_RELAXED) - g_q_drops_base);
    out->unacked = t.unacked; out->verified = t.verified; out->open = t.open; out->api_steam = t.api_steam;
    for (int i = 0; i < RBM_NERR; i++) out->bad_err[i] = t.bad_err[i] - b.bad_err[i];
}
void p2p_stats_total(P2pStats *out) {
    if (!out) return;
    snapshot(out, NULL, NULL);
    out->q_drops = (uint64_t)__atomic_load_n(&g_q_drops_total, __ATOMIC_RELAXED);
}
void p2p_stats_ext(P2pStatsExt *out) {
    if (!out) return;
    snapshot(NULL, NULL, out);
    out->tx_refused = (uint64_t)__atomic_load_n(&g_tx_refused_total, __ATOMIC_RELAXED);
    out->ev_stale = (uint64_t)__atomic_load_n(&g_ev_stale_total, __ATOMIC_RELAXED);
    out->io_tid = __atomic_load_n(&g_io_tid, __ATOMIC_ACQUIRE);
    out->shutdown_state = g_shutdown_state;
}
int p2p_peerid_result(P2pPeerIdRes *out) {   /* v0.29.7: bounded seqlock read (never waits) */
    if (!out) return 0;
    P2pPeerIdRes r; memset(&r, 0, sizeof r);
    for (int tries = 0; tries < 256; tries++) {
        int64_t s1 = __atomic_load_n(&g_pid_seq, __ATOMIC_ACQUIRE);
        if (s1 & 1) { YieldProcessor(); continue; }
        r = g_pid;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_pid_seq, __ATOMIC_RELAXED) == s1) break;
    }
    int cur = r.gen != 0 && r.gen == g_gen;
    if (!cur) memset(&r, 0, sizeof r);
    *out = r;
    return cur;
}
uint32_t p2p_peer_nonce(void) { return __atomic_load_n(&g_sh_peer_nonce, __ATOMIC_ACQUIRE); }
uint32_t p2p_io_thread_id(void) { return __atomic_load_n(&g_io_tid, __ATOMIC_ACQUIRE); }
const char *p2p_api_name(void) { const char *n = __atomic_load_n(&g_sh_api_name, __ATOMIC_ACQUIRE); return n ? n : "none"; }
