/* Option A M2: the rbonline side-channel codec (src/rbomsg.h; sc6rb/M2-INTERFACES.md §5). Part of the transport layer.
 * Pure C: no Windows, no log, no allocation, no game state. Every field is written / read byte by byte, little-endian, in the order of
 * the structs in rbomsg.h (np_w* / np_r* cursors of src/netproto.c; never a struct memcpy). One message per datagram.
 *
 * Canonical form (what rbm_encode writes, so encode -> decode -> encode is byte-exact):
 *   - every pad / pad2 field is written as 0 and decoded as 0 (the wire bytes of a pad are covered by the CRC but otherwise ignored);
 *   - RbmHello.plugin_version: at most 23 characters, NUL padded to 24 bytes (byte 23 is always 0 on the wire); the decoder forces
 *     plugin_version[23] = 0 so a caller can always print it with %s.
 * Decoder checks, in this order (the first failure is the result; nothing in h / m may be used after a failure):
 *   n < 32 -> RBM_ESHORT; magic -> RBM_EMAGIC; proto -> RBM_EPROTO; type unknown -> RBM_ETYPE; len != the type's fixed length ->
 *   RBM_ELEN; n < 32 + len -> RBM_ESHORT; n > 32 + len -> RBM_ELEN; CRC -> RBM_ECRC; a sequenced type with seq 0, or an unsequenced
 *   type (ACK / BEACON / TSYNC) with seq != 0 -> RBM_ETYPE. The decoder never reads in[n] or beyond (RBO-P2P-CODEC checks it with a guard page). */
#include <string.h>
#include "rbomsg.h"
#include "netproto.h"

const char *const k_rbw_name[RBW_N] = {
    "-", "desync", "fault", "peer_silent", "depth", "tripwire", "mask", "reset_wall", "peer_drain", "asym", "khit", "cost",
    "peer_restart", "internal", "endplay", "state5", "onlinesync0", "no_answer", "mismatch", "foreign", "nosteam", "badid", "cfg",
    "not_online", "spectator", "taint", "peer_bye", "armed", "paired", "probe",
    "arm_fail", "self", "ranked", "abort", "timers", "c3",   /* v0.29.1 */
    "q1sig",     /* v0.29.3 (review m7 = CQ-1): RBW_Q1SIG (36) had no name: p2p.c why_name(36) returned NULL to a %s */
    "d_disagree",   /* v0.33.0 (§22.2): RBW_DARM */
    "argc_off" };   /* v0.34.0 (§23.1): RBW_ARGC */
const char *const k_rbm_type[RBM_NTYPES] = { "-", "hello", "hello_ack", "arm", "arm_ack", "check", "desync", "drain", "bye", "ack", "beacon", "tsync" };   /* v0.30.0: + tsync */
const char *const k_rbm_err[RBM_NERR] = { "ok", "magic", "proto", "type", "len", "crc", "short" };
/* v0.29.6 (review MINOR 7): the version this process encodes and accepts. The plugin build: RBM_PROTO, a constant. Harness builds only: a
 * variable (rbomsg.h), so the PROTO4 case can run one peer at 3 (a v0.29.5-or-older build) against this build's 4. */
#ifdef SC6RB_HARNESS
int rbm_h_proto = RBM_PROTO;
int rbm_h_notsync;   /* v0.30.0 harness only (h_rbo_ts_notype, XPROTO6): this process knows no RBM_TSYNC (a v0.29.9 decoder at proto 6): type 11 is unknown */
#define RBM_PROTO_CUR ((uint8_t)rbm_h_proto)
#else
#define RBM_PROTO_CUR RBM_PROTO
#endif

size_t rbm_len_of(uint8_t type) {
    switch (type) {
    case RBM_HELLO: return RBM_LEN_HELLO;
    case RBM_HELLO_ACK: return RBM_LEN_HELLO_ACK;
    case RBM_ARM: case RBM_ARM_ACK: return RBM_LEN_ARM;
    case RBM_CHECK: return RBM_LEN_CHECK;
    case RBM_DESYNC: return RBM_LEN_DESYNC;
    case RBM_DRAIN: return RBM_LEN_DRAIN;
    case RBM_BYE: return RBM_LEN_BYE;
    case RBM_ACK: return RBM_LEN_ACK;
    case RBM_BEACON: return RBM_LEN_BEACON;
#ifdef SC6RB_HARNESS
    case RBM_TSYNC: return rbm_h_notsync ? (size_t)-1 : RBM_LEN_TSYNC;
#else
    case RBM_TSYNC: return RBM_LEN_TSYNC;   /* v0.30.0 */
#endif
    default: return (size_t)-1;
    }
}
int rbm_sequenced(uint8_t type) { return type >= RBM_HELLO && type <= RBM_BYE; }

/* ---------------------------------------------------------------- payload writers (field order = rbomsg.h) */
static void put_payload(NpW *w, uint8_t type, const RbmMsg *m) {
    switch (type) {
    case RBM_HELLO: {
        const RbmHello *p = &m->u.hello;
        np_w16(w, p->proto); np_w16(w, p->flags);
        char v[24]; memset(v, 0, sizeof v);
        size_t k = 0; while (k < 23 && p->plugin_version[k]) { v[k] = p->plugin_version[k]; k++; }
        np_wb(w, v, sizeof v);
        np_w32(w, p->exe_ts); np_w32(w, p->exe_soi);
        np_w64(w, p->content); np_w32(w, p->content_n[0]); np_w32(w, p->content_n[1]);
        for (int i = 0; i < 10; i++) np_w32(w, p->ctx_xmm[i]);
        np_w32(w, p->ctx_mxcsr); np_w16(w, p->ctx_fcw); np_w8(w, p->ctx_upper_zero); np_w8(w, p->ctx_const_ok);
        np_w32(w, p->ctx_samples);
        np_w8(w, p->d); np_w8(w, p->max_rb); np_w16(w, p->check_every);
        np_w32(w, p->arm_round_min);
        np_w16(w, p->rtt_ms); np_w16(w, 0);   /* v0.33.0 (RBM_PROTO 9) */
        break;
    }
    case RBM_HELLO_ACK: np_w32(w, m->u.hack.hello_nonce); np_w16(w, m->u.hack.verdict); np_w16(w, 0); break;
    case RBM_ARM: case RBM_ARM_ACK:
        np_w8(w, m->u.arm.round); np_w8(w, m->u.arm.d); np_w8(w, m->u.arm.max_rb); np_w8(w, 0);
        np_w16(w, m->u.arm.check_every); np_w16(w, 0);
        break;
    case RBM_CHECK:   /* v0.32.0 (RBM_PROTO 8): + n (was pad2), chain, fset, pad */
        np_w8(w, m->u.check.round); np_w8(w, 0); np_w16(w, 0);
        np_w32(w, m->u.check.gt); np_w32(w, m->u.check.f); np_w32(w, m->u.check.n); np_w64(w, m->u.check.hash);
        np_w64(w, m->u.check.chain); np_w32(w, m->u.check.fset); np_w32(w, 0);
        break;
    case RBM_DESYNC:
        np_w8(w, m->u.desync.round); np_w8(w, 0); np_w16(w, 0);
        np_w32(w, m->u.desync.gt); np_w64(w, m->u.desync.mine); np_w64(w, m->u.desync.theirs);
        break;
    case RBM_DRAIN: np_w8(w, m->u.drain.reason); np_w8(w, m->u.drain.round); np_w16(w, 0); np_w32(w, m->u.drain.gt); break;
    case RBM_BYE: np_w8(w, m->u.bye.reason); np_w8(w, 0); np_w16(w, 0); break;
    case RBM_BEACON:
        np_w8(w, m->u.beacon.state); np_w8(w, m->u.beacon.armed_round); np_w8(w, m->u.beacon.drained_reason); np_w8(w, m->u.beacon.round);
        np_w32(w, m->u.beacon.gt); np_w32(w, m->u.beacon.confirmed_gt);
        break;
    case RBM_TSYNC:   /* v0.30.0 */
        np_w8(w, m->u.tsync.round); np_w8(w, m->u.tsync.flags); np_w16(w, m->u.tsync.n);
        np_w32(w, (uint32_t)m->u.tsync.adv_sum); np_w32(w, m->u.tsync.gt); np_w16(w, m->u.tsync.waits); np_w16(w, m->u.tsync.rtt_ms);
        break;
    default: break;   /* RBM_ACK: no payload */
    }
}

static void get_payload(NpR *r, uint8_t type, RbmMsg *m) {
    switch (type) {
    case RBM_HELLO: {
        RbmHello *p = &m->u.hello;
        p->proto = np_r16(r); p->flags = np_r16(r);
        np_rb(r, p->plugin_version, sizeof p->plugin_version); p->plugin_version[sizeof p->plugin_version - 1] = 0;
        p->exe_ts = np_r32(r); p->exe_soi = np_r32(r);
        p->content = np_r64(r); p->content_n[0] = np_r32(r); p->content_n[1] = np_r32(r);
        for (int i = 0; i < 10; i++) p->ctx_xmm[i] = np_r32(r);
        p->ctx_mxcsr = np_r32(r); p->ctx_fcw = np_r16(r); p->ctx_upper_zero = np_r8(r); p->ctx_const_ok = np_r8(r);
        p->ctx_samples = np_r32(r);
        p->d = np_r8(r); p->max_rb = np_r8(r); p->check_every = np_r16(r);
        p->arm_round_min = np_r32(r);
        p->rtt_ms = np_r16(r); p->pad3 = 0; (void)np_r16(r);   /* v0.33.0 (RBM_PROTO 9) */
        break;
    }
    case RBM_HELLO_ACK: m->u.hack.hello_nonce = np_r32(r); m->u.hack.verdict = np_r16(r); (void)np_r16(r); break;
    case RBM_ARM: case RBM_ARM_ACK:
        m->u.arm.round = np_r8(r); m->u.arm.d = np_r8(r); m->u.arm.max_rb = np_r8(r); (void)np_r8(r);
        m->u.arm.check_every = np_r16(r); (void)np_r16(r);
        break;
    case RBM_CHECK:   /* v0.32.0 (RBM_PROTO 8) */
        m->u.check.round = np_r8(r); (void)np_r8(r); (void)np_r16(r);
        m->u.check.gt = np_r32(r); m->u.check.f = np_r32(r); m->u.check.n = np_r32(r); m->u.check.hash = np_r64(r);
        m->u.check.chain = np_r64(r); m->u.check.fset = np_r32(r); (void)np_r32(r);
        break;
    case RBM_DESYNC:
        m->u.desync.round = np_r8(r); (void)np_r8(r); (void)np_r16(r);
        m->u.desync.gt = np_r32(r); m->u.desync.mine = np_r64(r); m->u.desync.theirs = np_r64(r);
        break;
    case RBM_DRAIN: m->u.drain.reason = np_r8(r); m->u.drain.round = np_r8(r); (void)np_r16(r); m->u.drain.gt = np_r32(r); break;
    case RBM_BYE: m->u.bye.reason = np_r8(r); (void)np_r8(r); (void)np_r16(r); break;
    case RBM_BEACON:
        m->u.beacon.state = np_r8(r); m->u.beacon.armed_round = np_r8(r); m->u.beacon.drained_reason = np_r8(r); m->u.beacon.round = np_r8(r);
        m->u.beacon.gt = np_r32(r); m->u.beacon.confirmed_gt = np_r32(r);
        break;
    case RBM_TSYNC:   /* v0.30.0 */
        m->u.tsync.round = np_r8(r); m->u.tsync.flags = np_r8(r); m->u.tsync.n = np_r16(r);
        m->u.tsync.adv_sum = (int32_t)np_r32(r); m->u.tsync.gt = np_r32(r); m->u.tsync.waits = np_r16(r); m->u.tsync.rtt_ms = np_r16(r);
        break;
    default: break;
    }
}

size_t rbm_encode(uint8_t *out, size_t cap, RbmHdr *h, const RbmMsg *m) {
    if (!out || !h) return 0;
    uint8_t type = m ? m->type : (uint8_t)RBM_ACK;
    size_t len = rbm_len_of(type);
    if (type == 0 || len == (size_t)-1) return 0;
    if (cap < RBM_HDR + len || RBM_HDR + len > RBM_MAXDG) return 0;
    h->proto = RBM_PROTO_CUR; h->type = type; h->len = (uint16_t)len;
    NpW w = { out, 0, cap, 0 };
    np_w32(&w, RBM_MAGIC);                                     /* bytes 52 42 4F 4E */
    np_w8(&w, h->proto); np_w8(&w, h->type); np_w16(&w, h->len);
    np_w32(&w, h->nonce_tx); np_w32(&w, h->nonce_rx); np_w32(&w, h->seq); np_w32(&w, h->ack); np_w32(&w, h->ack_bits);
    np_w32(&w, 0);                                             /* the CRC, written below */
    if (m) put_payload(&w, type, m);
    if (w.over || w.n != RBM_HDR + len) return 0;
    uint32_t c = np_crc32(out + RBM_HDR, len, np_crc32(out, 28, 0));
    h->crc = c;
    out[28] = (uint8_t)c; out[29] = (uint8_t)(c >> 8); out[30] = (uint8_t)(c >> 16); out[31] = (uint8_t)(c >> 24);
    return RBM_HDR + len;
}

int rbm_decode(const uint8_t *in, size_t n, RbmHdr *h, RbmMsg *m) {
    if (!in || n < RBM_HDR) return RBM_ESHORT;
    NpR r = { in, 0, RBM_HDR, 0 };                             /* the header cursor never goes past byte 31 */
    if (np_r32(&r) != RBM_MAGIC) return RBM_EMAGIC;
    RbmHdr t; memset(&t, 0, sizeof t);
    t.proto = np_r8(&r); t.type = np_r8(&r); t.len = np_r16(&r);
    if (t.proto != RBM_PROTO_CUR) return RBM_EPROTO;
    size_t want = rbm_len_of(t.type);
    if (t.type == 0 || want == (size_t)-1) return RBM_ETYPE;
    if (t.len != want) return RBM_ELEN;
    if (n < RBM_HDR + (size_t)t.len) return RBM_ESHORT;
    if (n > RBM_HDR + (size_t)t.len) return RBM_ELEN;
    t.nonce_tx = np_r32(&r); t.nonce_rx = np_r32(&r); t.seq = np_r32(&r); t.ack = np_r32(&r); t.ack_bits = np_r32(&r); t.crc = np_r32(&r);
    if (np_crc32(in + RBM_HDR, t.len, np_crc32(in, 28, 0)) != t.crc) return RBM_ECRC;
    if (rbm_sequenced(t.type) ? t.seq == 0 : t.seq != 0) return RBM_ETYPE;
    if (h) *h = t;
    if (m) {
        memset(m, 0, sizeof *m);
        m->type = t.type; m->seq = t.seq;
        NpR p = { in + RBM_HDR, 0, t.len, 0 };
        get_payload(&p, t.type, m);
    }
    return RBM_OK;
}
