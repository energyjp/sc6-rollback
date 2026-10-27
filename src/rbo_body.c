/* Option A M2: the rbonline hook bodies and every PASSIVE game read of mode=rbonline (sc6rb/M2-INTERFACES.md §3.3, §3.7). Part of the
 * rollback core.
 *
 * Built as its own object with nobs_body.c's flags (build.sh rbo_body: -fno-builtin -mgeneral-regs-only -fno-stack-protector, cast-qual /
 * discards-qualifiers / int-conversion as errors): no vector instruction here, and every game address is a read-only `const volatile`
 * pointer (gptr), so this file cannot write game memory by construction (tools/rbo_static.py checks the object: its disassembly and its
 * imports). No struct copy, no loop the compiler could turn into memcpy / memset (the object must import nothing from the CRT).
 *
 * rbo_dispatch runs on the thunk's private stack with every register saved (src/rbo_thunks.S): first M1's body (nobs_dispatch, unchanged:
 * the NOBS records, REGSEQ, D2, ...), then rbonline's per-hook body: the reads below, then the state machine in src/rbo.c (a normal TU:
 * the thunk's XSAVE64 / XRSTOR64 covers any vector use there). The state machine writes game memory only when ARMED (rbo.c counts every
 * write); while PASSIVE / PROBE / PAIRED it only pushes events and calls the p2p queue functions. */
#include "rbo_int.inc"
#include <intrin.h>

typedef const volatile uint8_t *gptr;
static inline gptr G(uintptr_t a) { return (gptr)a; }                       /* cast helper 1 (reads only) */
static inline const void *Q(uintptr_t a) { return (const void *)a; }        /* cast helper 2 (VirtualQuery argument only) */
static inline uint8_t  g8 (gptr p) { return p[0]; }
static inline uint32_t g32(gptr p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t g64(gptr p) { return (uint64_t)g32(p) | (uint64_t)g32(p + 4) << 32; }

RboCtxAcc rbo_ctx_acc;
volatile int rbo_ctx_on;
#ifdef SC6RB_HARNESS
void (*rbo_h_body_enter_fn)(int hook);   /* RBO-GUARD: around the rbonline body (M1's nobs_h_body_*_fn wrap only M1's body) */
void (*rbo_h_body_exit_fn)(int hook);
#endif

/* ---------------------------------------------------------------- VirtualQuery-based readability (no CRT), with a small page cache */
static int readable(uintptr_t a, size_t n) {
    if (a < 0x10000 || a + n < a || a + n > 0x7ffffffeffffULL) return 0;
    uintptr_t end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(Q(a), &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return 0;
        uintptr_t rend = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (rend <= a) return 0;
        a = rend;
    }
    return 1;
}
typedef struct { uintptr_t a; size_t n; int ok; } RdCache;
static int readable_c(RdCache *c, uintptr_t a, size_t n) {
    if (c->a != a || c->n != n) { c->a = a; c->n = n; c->ok = readable(a, n); }
    return c->ok;
}
/* a 64-entry page cache for the tree / list walks (one VirtualQuery per new page; pages are never "un-cached" inside one walk) */
#define PGC_N 64
static uintptr_t g_pgc[PGC_N]; static int g_pgc_n;
static int page_ok(uintptr_t a, size_t n) {
    if (a < 0x10000 || a + n < a || a + n > 0x7ffffffeffffULL) return 0;
    uintptr_t p0 = a & ~(uintptr_t)0xfff, p1 = (a + n - 1) & ~(uintptr_t)0xfff;
    for (uintptr_t p = p0; p <= p1; p += 0x1000) {
        int hit = 0;
        for (int i = 0; i < g_pgc_n; i++) if (g_pgc[i] == p) { hit = 1; break; }
        if (hit) continue;
        if (!readable(p, 0x1000)) return 0;
        if (g_pgc_n < PGC_N) g_pgc[g_pgc_n++] = p;
    }
    return 1;
}

static inline DWORD tid_now(void) { return __readgsdword(0x48); }   /* TEB ClientId.UniqueThread */

/* ---------------------------------------------------------------- the observation of an InputSync object / the manager */
static RdCache c_mgr, c_obj, c_sync;
static void obs_zero(RboObs *o) {
    o->mgr = 0; o->obj = 0; o->mgr_ok = 0; o->obj_ok = 0; o->cls = 0;
    o->bonline = o->onlinesync = o->state = o->result = o->remote = o->local = o->players = 0;
    o->round = o->gt = o->synctime = o->sendtime = o->pausetime = o->lag = o->lagmin = o->lagmax = 0;
    o->phase = o->m12f8 = o->mgrstate = 0;
    o->rtt_bits = 0;
}
static void obs_fill_obj(RboObs *o, gptr b) {
    o->bonline = g8(b + NOBS_O_BONLINE);
    o->onlinesync = g8(b + NOBS_O_ONLINESYNC);
    o->state = g8(b + NOBS_O_STATE);
    o->result = g8(b + NOBS_O_RESULT);
    o->remote = g32(b + NOBS_O_REMOTEFLAGS);
    o->local = g32(b + NOBS_O_LOCALFLAGS);
    o->players = g32(b + NOBS_O_PLAYERS);
    o->round = g32(b + NOBS_O_GAMEROUND);
    o->gt = g32(b + NOBS_O_GAMETIME);
    o->synctime = g32(b + NOBS_O_SYNCTIME);
    o->sendtime = g32(b + NOBS_O_SENDTIME);
    o->pausetime = g32(b + NOBS_O_PAUSETIME);
    o->lag = g32(b + NOBS_O_INPUTLAG);
    o->lagmin = g32(b + NOBS_O_LAGMIN);
    o->lagmax = g32(b + NOBS_O_LAGMAX);
    o->rtt_bits = g32(b + NOBS_O_RTT);   /* v0.30.0: a read (the RTT float's bits; never converted here: no vector register in this object) */
}
int rbo_read_obj(uintptr_t obj, RboObs *o) {
    obs_zero(o);
    if (!readable_c(&c_sync, obj, NOBS_OBJ_SIZE)) return 0;
    gptr b = G(obj);
    if (g64(b) != VA(NOBS_A_INPUTSYNC_VT)) return 0;
    o->obj = obj; o->obj_ok = 1; o->cls = 2;
    obs_fill_obj(o, b);
    return 1;
}
static void read_mgr(uint64_t mgr, RboObs *o) {
    obs_zero(o);
    o->mgr = mgr;
    o->mgr_ok = readable_c(&c_mgr, mgr, NOBS_MGR_READ);
    if (!o->mgr_ok) return;
    gptr m = G(mgr);
    o->phase = g8(m + NOBS_M_PHASE1480);
    o->m12f8 = g32(m + NOBS_M_MASK12F8);
    o->mgrstate = g8(m + NOBS_M_STATEBYTE);
    uint64_t obj = g64(m + NOBS_M_INPUTOBJ);
    if (!obj) return;
    if (!readable_c(&c_obj, obj, NOBS_OBJ_SIZE)) { o->cls = 3; return; }
    uint64_t vt = g64(G(obj));
    o->cls = vt == VA(NOBS_A_INPUTSYNC_VT) ? 2 : vt == VA(NOBS_A_INPUTLOG_VT) ? 1 : 3;
    o->obj = obj;
    if (o->cls != 2) return;
    o->obj_ok = 1;
    obs_fill_obj(o, G(obj));
}

/* ---------------------------------------------------------------- the opponent id (as nobs_read_sel5, returning the value; never logged) */
int rbo_read_opp_id(uint64_t *id) {
    *id = 0;
    uintptr_t hp = VA(RBO_A_SESS_HOLDER);
    if (!readable(hp, 8)) return 0;
    uint64_t holder = g64(G(hp));
    if (!holder || !readable(holder, 16)) return 0;
    uint64_t sess = g64(G(holder)), ref = g64(G(holder + 8));
    if (!sess || !ref || !readable(ref, 12) || (int32_t)g32(G(ref + 8)) <= 0) return 0;   /* the weak pointer must be alive */
    if (!readable(sess, RBO_SESS_OPP_ID + 8)) return 0;
    *id = g64(G(sess + RBO_SESS_OPP_ID));
    return 1;
}

/* ---------------------------------------------------------------- v0.29.7: the opponent's SteamID candidates (OPTA-M2-SPEC.md §18.10)
 * The static layout is in rbo.h (RBO_A_COMMSYS ... RBO_PSRC_*). Source 1 (RBO_PSRC_PEER): the CommSys peer the game's own send resolves the
 * session's opponent address id to (the same rule as 0x142e01f80: the peer's address object id == the destination id), and that peer's UE
 * address when it is an FInternetAddrSteam: its SteamID. Every object is type-checked by its vtable before a field is read. Source 2
 * (RBO_PSRC_RAW): the opponent id itself (v0.29.6). Reads only (gptr), at most RBO_PEER_SCAN vector elements, one VirtualQuery-checked read
 * per object; no id is logged anywhere. */
static void cand_add(uint64_t *ids, uint8_t *srcs, int cap, int *n, uint64_t id, int src) {
    for (int k = 0; k < *n; k++) if (ids[k] == id) return;   /* the same id from a later source: counted by the caller, kept once */
    if (*n >= cap) return;
    ids[*n] = id; srcs[*n] = (uint8_t)src; (*n)++;
}
int rbo_read_opp_cands(uint64_t *ids, uint8_t *srcs, int cap, uint32_t *n_src, int old_rule) {
    for (int s = 0; s < RBO_PSRC_N; s++) n_src[s] = 0;
    int n = 0;
    uint64_t opp = 0;
    if (!rbo_read_opp_id(&opp)) return 0;   /* no live session: nothing to look up */
    if (!old_rule) {
        uintptr_t nv = VA(RBO_A_ADDR_NONE), e0 = VA(RBO_A_ADDR_EL0);
        int skip = (readable(nv, 8) && opp == g64(G(nv))) || (readable(e0, 8) && opp == g64(G(e0)));   /* "no address" / selector 5's element 0 */
        uintptr_t cs = VA(RBO_A_COMMSYS);
        if (!skip && readable(cs + RBO_CS_PEERS_BEG, 16)) {
            uint64_t b = g64(G(cs + RBO_CS_PEERS_BEG)), e = g64(G(cs + RBO_CS_PEERS_END));
            if (b && e > b && ((e - b) & 0xf) == 0 && (e - b) / 16 <= RBO_PEER_SCAN && readable((uintptr_t)b, (size_t)(e - b))) {
                for (uint64_t p = b; p < e; p += 16) {
                    uint64_t peer = g64(G((uintptr_t)p));
                    if (!peer || !readable((uintptr_t)peer, 8) || g64(G((uintptr_t)peer)) != VA(RBO_A_PEER_VT)) continue;
                    if (!readable((uintptr_t)peer + RBO_PEER_UADDR, RBO_PEER_AID + 8 - RBO_PEER_UADDR)) continue;
                    if (g64(G((uintptr_t)peer + RBO_PEER_AOBJ)) != VA(RBO_A_ADDROBJ_VT)) continue;
                    if (g64(G((uintptr_t)peer + RBO_PEER_AID)) != opp) continue;
                    uint64_t ua = g64(G((uintptr_t)peer + RBO_PEER_UADDR));
                    if (!ua || !readable((uintptr_t)ua, RBO_UADDR_STEAM_ID + 8) || g64(G((uintptr_t)ua)) != VA(RBO_A_UADDR_STEAM_VT)) continue;
                    n_src[RBO_PSRC_PEER]++;
                    cand_add(ids, srcs, cap, &n, g64(G((uintptr_t)ua + RBO_UADDR_STEAM_ID)), RBO_PSRC_PEER);
                }
            }
        }
    }
    n_src[RBO_PSRC_RAW] = 1;
    cand_add(ids, srcs, cap, &n, opp, RBO_PSRC_RAW);
    return n;
}

/* ---------------------------------------------------------------- the content digest (spec §4; C1b's content gate in passive form)
 * v0.29.9 (OPTA-M2-SPEC.md §18.12): the HELLO digest is base-independent. Per player: the move-record tree at PLAYER+0x455d0 (std::map:
 * the size at +0x455d8, then an in-order walk hashing the 4-byte key at node+0x20, bounded by 4n + 16 steps, every node readable, then the
 * visited count), and the three KHit lists at PLAYER+0x44478 / +0x44498 / +0x444b8 (next at +0x18; at most 512 nodes): per node IN LIST
 * ORDER cd_auth (the list, the class as a vtable RVA, the authored fields the deserializer writes once at load), then the list's count.
 * Never an absolute address, a link, a world buffer, the per-state flag +0x14, a live (modifier-scaled) copy or a never-written byte.
 * walk_ok bit p*2 = player p's tree walk completed, bit p*2+1 = its lists. A failed walk hashes only what it saw (deterministic for equal
 * content); the digest covers the walk_ok bits so a failure on one side can only refuse pairing (fail safe), never pair wrongly.
 * v0.29.7 / v0.29.8's digest (the vtable RVA + the raw 0x1c bytes at +0x7c per node, which hold the next node's absolute vtable or the
 * list's node-pointer array behind a KHitSphere: module-base dependent, §18.11) is still computed in the same pass, as a diagnostic only
 * (rbo_cdiag.digest_v8: the dump's H line and RB CONTENT v8=), never on the wire (the harness control XCDBASE sends it). */
#define PL_VA(p, off) (VA(NOBS_A_PL0) + (uintptr_t)(p) * NOBS_PL_STRIDE + (uintptr_t)(off))
#define NOBS_A_PL0 0x1447156f0ULL
#define NOBS_PL_STRIDE 0x973f0ULL
/* v0.29.8 (OPTA-M2-SPEC.md §18.11): the diagnosis (rbo_int.inc RboCdiag). Written only into the plugin's own rbo_cdiag; every game read stays a
 * gptr read, and the digest's reads and their order are v0.29.7's (each value read once, then hashed into the digest AND the sub-digest). */
RboCdiag rbo_cdiag;
int rbo_cd_capture;
#ifdef SC6RB_HARNESS
int rbo_h_content_old;   /* v0.29.9 harness only (h_rbo_content_old=1, set by rbo.c): the HELLO carries v0.29.8's digest (XCDBASE, PROTO5's old peer) */
uint64_t rbo_h_content_v8(void) { return rbo_cdiag.digest_v8; }   /* the last read's v0.29.8 digest (the in-process reference check) */
uint64_t rbo_h_content_v9(void) { return rbo_cdiag.digest_v9; }   /* the last read's v0.29.9 digest (whatever the HELLO carried) */
#endif
#define CD_RVA_SPHERE  0x3e877f0ULL   /* KHitSphere  vt 0x143e877f0 (V: 0x14030e0d0 stores it at +0x00) */
#define CD_RVA_AREA    0x3e877a8ULL   /* KHitArea    vt 0x143e877a8 (V: 0x14030e3a0) */
#define CD_RVA_FIXAREA 0x3e87760ULL   /* KHitFixArea vt 0x143e87760 (V: 0x14030c940's inline branch) */
#define CD_BASIS 0xcbf29ce484222325ULL
static void cd_pl_zero(RboCdPl *d) {
    d->read = d->tree_sz = d->tree_n = d->tree_ok = 0;
    for (int q = 0; q < 3; q++) { d->list_ok[q] = d->kn[q] = d->kabs[q] = 0; d->kvt[q] = d->kspan[q] = d->kreb[q] = d->ka[q] = CD_BASIS; }
    d->tree_h = d->auth = CD_BASIS;
}
static inline void fnv2(uint64_t h[2], uint64_t v, int n) { h[0] = rbo_fnv(h[0], v, n); h[1] = rbo_fnv(h[1], v, n); }   /* [0] v0.29.9, [1] v0.29.8 */
static void cd_key(RboCdPl *d, int p, int cap, uint32_t k) {
    d->tree_h = rbo_fnv(d->tree_h, k, 4); d->tree_n++;
    if (!cap) return;
    if (rbo_cdiag.nkeys[p] < RBO_CD_KEYS) rbo_cdiag.keys[p][rbo_cdiag.nkeys[p]++] = k; else rbo_cdiag.trunc_keys[p]++;
}
/* the authored fields of one node = what the HELLO digest hashes per node (v0.29.9; OPTA-M2-SPEC.md §18.12 has the per-class table). Each
 * is written ONCE at load by the deserializer 0x14030c940 / the constructors 0x14030e0d0 (Sphere), 0x14030e3a0 (Area), the inline FixArea
 * branch (V), and by no KHit vtable method nor any code at the 14 list-head references in the image (9 functions; V: their only node store
 * is +0x14); a writer through another pointer: H. Common header: +0x08 the kind bit (1 << record+2, a full qword), +0x10 the flags
 * (record+4), +0x16 the record type, +0x17 the kind. Sphere: the authored local center +0x40 / +0x44 / +0x48 (record+8..+0x10; v0.29.8's
 * auth hashed +0x30.., the LIVE copy that the vtable slot 5 0x14030e2f0 rewrites from +0x40 plus a modifier offset), the authored radius
 * +0x74 (not +0x70, the live one 0x14030e2f0 rewrites), +0x78 (record+0x18), the bone +0x7c. Area: the local points +0x30..+0x38 /
 * +0x40..+0x48, the bones +0x90 / +0x94 (its slot 5 is `ret`). FixArea: the local points +0x30 / +0x40 / +0x50 (xyz), the bone +0x90.
 * Never: +0x00 (an absolute vtable: the class goes in as the RVA vr), +0x14 (the per-state active flag: the stores 0x1402feca6 / fecd6 /
 * fee3e / ff16a, 0x140300d8d / ea9 / ed6, 0x140308817 / 884a / 8d54 / 8da4, 0x14033dbed), +0x18 / +0x20 (links), the w = 1.0 constants,
 * the world buffers (Sphere +0x50..+0x6f, Area +0x50..+0x8f, FixArea +0x60..+0x8f: the per-tick updates 0x14030e1a0 / 0x14030e480 /
 * 0x14030e690), FixArea +0x94..+0x9f and Area +0x98..+0x9f (never written), or a byte past a Sphere's 0x80. */
static uint64_t cd_auth(uint64_t a, uintptr_t o, uint64_t vr, int q) {
    a = rbo_fnv(a, (uint64_t)q, 1);
    a = rbo_fnv(a, vr, 8);
    a = rbo_fnv(a, g64(G(o + 0x08)), 8);
    a = rbo_fnv(a, g32(G(o + 0x10)), 4);
    a = rbo_fnv(a, g8(G(o + 0x16)), 1);
    a = rbo_fnv(a, g8(G(o + 0x17)), 1);
    static const uint8_t sph[] = { 0x40, 0x44, 0x48, 0x74, 0x78, 0x7c }, area[] = { 0x30, 0x34, 0x38, 0x40, 0x44, 0x48, 0x90, 0x94 },
                         fix[] = { 0x30, 0x34, 0x38, 0x40, 0x44, 0x48, 0x50, 0x54, 0x58, 0x90 };
    const uint8_t *f = 0; int nf = 0;
    if (vr == CD_RVA_SPHERE) { f = sph; nf = (int)sizeof sph; }
    else if (vr == CD_RVA_AREA) { f = area; nf = (int)sizeof area; }
    else if (vr == CD_RVA_FIXAREA) { f = fix; nf = (int)sizeof fix; }
    for (int i = 0; i < nf; i++) a = rbo_fnv(a, g32(G(o + f[i])), 4);
    return a;
}
static void cd_node(RboCdPl *d, int p, int cap, int q, uint32_t idx, uintptr_t o, uint64_t vr, const uint32_t s[7]) {
    d->kvt[q] = rbo_fnv(d->kvt[q], vr, 8);
    for (int i = 0; i < 7; i++) d->kspan[q] = rbo_fnv(d->kspan[q], s[i], 4);
    d->kreb[q] = rbo_fnv(d->kreb[q], s[0], 4);
    for (int k = 0; k < 3; k++) {   /* the span's 8-aligned qwords: node+0x80 / +0x88 / +0x90 */
        uint64_t v = (uint64_t)s[1 + 2 * k] | (uint64_t)s[2 + 2 * k] << 32;
        if (v >= g_base && v < g_base + g_image_size) { v -= g_base; d->kabs[q]++; }
        d->kreb[q] = rbo_fnv(d->kreb[q], v, 8);
    }
    d->auth = cd_auth(d->auth, o, vr, q);   /* v0.29.9: the same per-node values the HELLO digest hashes (all lists) */
    d->ka[q] = cd_auth(d->ka[q], o, vr, q); /* v0.29.9: per list (the count follows in khit_lists) = this list's part of the HELLO digest */
    if (!cap) return;
    if (rbo_cdiag.nnodes[p] >= RBO_CD_NODES) { rbo_cdiag.trunc_nodes[p]++; return; }
    RboCdNode *c = &rbo_cdiag.nodes[p][rbo_cdiag.nnodes[p]++];
    c->q = (uint8_t)q; c->rsv = 0; c->idx = (uint16_t)idx; c->vt_rva = (uint32_t)vr; c->node = (uint64_t)o;
    for (int i = 0; i < RBO_CD_NB; i++) c->b[i] = g8(G(o + (uintptr_t)i));   /* page_ok(o, 0xa0) held for this node */
}
static void tree_keys(uintptr_t pl, uint64_t n, uint64_t h[2], int *ok, RboCdPl *d, int p, int cap) {
    *ok = 0;
    uintptr_t head = (uintptr_t)g64(G(pl + 0x455d0));
    if (!head && !n) { *ok = 1; return; }
    if (!page_ok(head, 0x30) || !g8(G(head + 0x19))) return;   /* the sentinel is _Isnil */
    uint64_t limit = 4 * n + 16, steps = 0, visited = 0;
    uintptr_t x = (uintptr_t)g64(G(head));                      /* _Left of the sentinel = the leftmost node */
    if (!page_ok(x, 0x30)) return;
    while (x != head) {
        if (++steps > limit || g8(G(x + 0x19))) return;
        uint32_t k = g32(G(x + 0x20));
        fnv2(h, k, 4); visited++;
        cd_key(d, p, cap, k);
        uintptr_t r = (uintptr_t)g64(G(x + 0x10));
        if (!page_ok(r, 0x30)) return;
        if (!g8(G(r + 0x19))) {
            x = r;
            for (;;) {
                uintptr_t l = (uintptr_t)g64(G(x));
                if (!page_ok(l, 0x30)) return;
                if (g8(G(l + 0x19))) break;
                x = l;
                if (++steps > limit) return;
            }
        } else {
            uintptr_t y = (uintptr_t)g64(G(x + 8));
            for (;;) {
                if (!page_ok(y, 0x30)) return;
                if (g8(G(y + 0x19)) || x != (uintptr_t)g64(G(y + 0x10))) break;
                x = y; y = (uintptr_t)g64(G(y + 8));
                if (++steps > limit) return;
            }
            x = y;
        }
    }
    *ok = visited == n;
    fnv2(h, visited, 8);
}
/* v0.29.8 (OPTA-M2-SPEC.md §18.11, V by static analysis of the deserializer 0x14030c940): the span +0x7c..+0x97 v0.29.7's digest hashed is
 * NOT bone ints for every class. The nodes are packed into a static per-player pool in the image (0x14485ebd0 + p * 0x4000; body, hurtbox,
 * attack lists in that order, each followed by an array of absolute node pointers): a Sphere is 0x80 bytes, so its +0x80..+0x97 is the NEXT
 * node's header (its vtable as an absolute address) or, for a tail Sphere, that pointer array; KHitArea +0x7c..+0x8f / KHitFixArea
 * +0x7c..+0x8f are per-frame world buffers; KHitFixArea +0x94 is never written (pool leftover). v0.29.9: h[0] (the HELLO digest) takes
 * cd_auth per node in list order instead; h[1] keeps v0.29.8's span (the diagnostic digest_v8), each span dword read once as before.
 * The list ORDER stays in the digest: the game's own iteration order decides gameplay values (V: 0x140308677 takes the FIRST node of a kind
 * in list order; 0x1403452f0 sums the world centers of a kind's nodes in list order with float adds, which are order-dependent). */
static void khit_lists(uintptr_t pl, uint64_t h[2], int *ok, RboCdPl *d, int p, int cap) {
    static const uint32_t heads[3] = { 0x44478, 0x44498, 0x444b8 };
    *ok = 1;
    for (int q = 0; q < 3; q++) {
        uintptr_t o = (uintptr_t)g64(G(pl + heads[q]));
        uint32_t cnt = 0;
        d->list_ok[q] = 1;
        for (int guard = 0; o && guard < 512; guard++) {
            if ((o & 7) || !page_ok(o, 0xa0)) { *ok = 0; d->list_ok[q] = 0; break; }
            uint64_t vt = g64(G(o));
            uint64_t vr = vt >= g_base && vt < g_base + g_image_size ? vt - g_base : 0;   /* image vtables as RVAs; heap ones as 0 */
            h[0] = cd_auth(h[0], o, vr, q);   /* v0.29.9: the class + the authored fields */
            h[1] = rbo_fnv(h[1], vr, 8);
            uint32_t s[7];
            for (int i = 0; i < 7; i++) { s[i] = g32(G(o + 0x7c + (uintptr_t)(4 * i))); h[1] = rbo_fnv(h[1], s[i], 4); }   /* v0.29.7's span, same order */
            cd_node(d, p, cap, q, cnt, o, vr, s);
            cnt++;
            o = (uintptr_t)g64(G(o + 0x18));
        }
        fnv2(h, cnt, 4);
        d->ka[q] = rbo_fnv(d->ka[q], cnt, 4);
        d->kn[q] = cnt;
    }
}
void rbo_read_content(uint64_t *digest, uint32_t n[2], uint32_t *walk_ok) {
    uint64_t h[2] = { CD_BASIS, CD_BASIS };
    uint32_t okb = 0;
    int cap = rbo_cd_capture && !rbo_cdiag.busy;
    g_pgc_n = 0;
    rbo_cdiag.captured = 0;
    {   uintptr_t t = VA(0x14470dec4ULL);   /* g_LuxKHitArea_DoubleBufferToggle (V: 0x14030e484 reads it) */
        rbo_cdiag.toggle_ok = (uint32_t)readable(t, 4); rbo_cdiag.toggle = rbo_cdiag.toggle_ok ? g32(G(t)) : 0; }
    for (int p = 0; p < 2; p++) {
        RboCdPl *d = &rbo_cdiag.pl[p];
        cd_pl_zero(d);
        if (cap) rbo_cdiag.nkeys[p] = rbo_cdiag.nnodes[p] = rbo_cdiag.trunc_keys[p] = rbo_cdiag.trunc_nodes[p] = 0;   /* a capture the writer still reads stays */
        uintptr_t pl = PL_VA(p, 0);
        n[p] = 0;
        if (!readable(pl + 0x44478, 0x455e0 - 0x44478)) continue;
        d->read = 1;
        uint64_t sz = g64(G(pl + 0x455d8));
        n[p] = (uint32_t)sz; d->tree_sz = (uint32_t)sz;
        fnv2(h, sz, 8);
        int ok1 = 0, ok2 = 0;
        tree_keys(pl, sz, h, &ok1, d, p, cap);
        khit_lists(pl, h, &ok2, d, p, cap);
        d->tree_ok = (uint32_t)ok1;
        okb |= (uint32_t)ok1 << (p * 2) | (uint32_t)ok2 << (p * 2 + 1);
    }
    rbo_cdiag.digest_v9 = rbo_fnv(h[0], okb, 4);
    rbo_cdiag.digest_v8 = rbo_fnv(h[1], okb, 4);
    *digest = rbo_cdiag.digest_v9;
#ifdef SC6RB_HARNESS
    if (rbo_h_content_old) *digest = rbo_cdiag.digest_v8;   /* XCDBASE / XPROTO5 / PROTO5's old peer: a v0.29.8 build's HELLO digest */
#endif
    *walk_ok = okb;
    if (cap) { rbo_cdiag.captured = 1; rbo_cdiag.busy = 1; }   /* the writer clears busy once the dump file is written */
}

/* ---------------------------------------------------------------- the bodies */
static void body_tick(NobsCtx *ctx) {
    RboObs o;
    read_mgr(ctx->rcx, &o);
    rbo_on_tick(&o);
}
static void body_step(NobsCtx *ctx) {
    if (rbo_ctx_on) rbo_ctx_add(&rbo_ctx_acc, nobs_xarea[NOBS_H_IFC_STEP], nobs_xoff_avx);
    rbo_on_step(ctx);
}
static void body_ispaused(NobsCtx *ctx) {
    RboObs o;
    if (!rbo_read_obj(ctx->rcx, &o)) return;
    rbo_on_ispaused(&o);
}
static void body_setstate(NobsCtx *ctx) {
    RboObs o;
    if (!rbo_read_obj(ctx->rcx, &o)) return;
    rbo_on_setstate(&o, (uint32_t)(ctx->rdx & 0xff));
}
static void body_rreset(NobsCtx *ctx) {
    RboObs o;
    if (!rbo_read_obj(ctx->rcx, &o)) return;
    rbo_on_rreset(&o, (uint32_t)ctx->rdx);
}

/* ---------------------------------------------------------------- the dispatcher the thunks call (rcx = id, rdx = ctx) */
void rbo_dispatch(int hook, NobsCtx *ctx) {
    nobs_dispatch(hook, ctx);                    /* M1's body first, unchanged */
    if (rbo_off) return;
    DWORD tid = tid_now();
    if (nobs_game_tid && tid != (DWORD)nobs_game_tid) return;    /* another thread (M1 recorded it): no rbonline body. Before the first
                                                                  * Tick the game thread is not known yet (the first battle's Init runs
                                                                  * before it): like M1's body, the hook's own thread is accepted */
#ifdef SC6RB_HARNESS
    if (rbo_h_body_enter_fn) rbo_h_body_enter_fn(hook);
#endif
    rbo_on_any_body(hook);
    switch (hook) {
    case NOBS_H_TICK:         body_tick(ctx); break;
    case NOBS_H_IFC_INIT:     rbo_on_init(ctx->rdx); break;
    case NOBS_H_IFC_SHUTDOWN: rbo_on_shutdown(); break;
    case NOBS_H_IFC_STEP:     body_step(ctx); break;
    case NOBS_H_ISPAUSED:     body_ispaused(ctx); break;
    case NOBS_H_SETSTATE:     body_setstate(ctx); break;
    case NOBS_H_RRESET:       body_rreset(ctx); break;
    default: break;                              /* SendControl, SSM: M1's body only */
    }
#ifdef SC6RB_HARNESS
    if (rbo_h_body_exit_fn) rbo_h_body_exit_fn(hook);
#endif
}
