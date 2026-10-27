/* C2 (C2-SPEC.md §2.2): the wire codec. See netproto.h. */
#include "netproto.h"
#include <string.h>

const char *const k_np_type[NP_NTYPES] = { "-", "HELLO", "HELLO_ACK", "PING", "PONG", "SEED", "SEED_ACK", "SETUP_PART", "SETUP_ACK", "READY", "INPUT", "ZONE",
                                           "PAUSE", "RESUME", "DESYNC_NOTE", "DUMP_REQ", "DUMP_ACK", "QUIT", "ABORT" };
const char *const k_np_reason[NPR_N] = { "-", "quit_f3", "desync", "peer_timeout", "refused", "setup", "ctltype", "barrier_mispredict", "depth", "pause_cap",
                                         "peer_restart", "netq_overflow", "rec_end", "internal", "peer_left" };

static uint32_t g_crc_tab[256]; static int g_crc_init;
uint32_t np_crc32(const void *p, size_t n, uint32_t crc) {
    if (!g_crc_init) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; g_crc_tab[i] = c; }
        g_crc_init = 1;
    }
    const uint8_t *b = (const uint8_t *)p;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = g_crc_tab[(crc ^ b[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}
uint64_t np_fnv64(const void *p, size_t n, uint64_t h) { const uint8_t *b = (const uint8_t *)p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ULL; } return h; }
uint32_t np_fnv32(const void *p, size_t n, uint32_t h) { const uint8_t *b = (const uint8_t *)p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x01000193u; } return h; }

void np_w8(NpW *w, uint8_t v) { if (w->n + 1 > w->cap) { w->over = 1; return; } w->p[w->n++] = v; }
void np_w16(NpW *w, uint16_t v) { np_w8(w, (uint8_t)v); np_w8(w, (uint8_t)(v >> 8)); }
void np_w32(NpW *w, uint32_t v) { np_w16(w, (uint16_t)v); np_w16(w, (uint16_t)(v >> 16)); }
void np_w64(NpW *w, uint64_t v) { np_w32(w, (uint32_t)v); np_w32(w, (uint32_t)(v >> 32)); }
void np_wb(NpW *w, const void *b, size_t n) { if (w->n + n > w->cap) { w->over = 1; return; } memcpy(w->p + w->n, b, n); w->n += n; }
uint8_t np_r8(NpR *r) { if (r->n + 1 > r->len) { r->under = 1; return 0; } return r->p[r->n++]; }
uint16_t np_r16(NpR *r) { uint16_t a = np_r8(r); return (uint16_t)(a | (uint16_t)np_r8(r) << 8); }
uint32_t np_r32(NpR *r) { uint32_t a = np_r16(r); return a | (uint32_t)np_r16(r) << 16; }
uint64_t np_r64(NpR *r) { uint64_t a = np_r32(r); return a | (uint64_t)np_r32(r) << 32; }
void np_rb(NpR *r, void *b, size_t n) { if (r->n + n > r->len) { r->under = 1; memset(b, 0, n); return; } memcpy(b, r->p + r->n, n); r->n += n; }

size_t np_encode(uint8_t *out, size_t cap, NpHdr *h, const void *payload, size_t len) {
    if (len > NP_MAXPAY || NP_HDR + len > cap) return 0;
    h->proto = NP_PROTO; h->len = (uint16_t)len;
    NpW w = { out, 0, cap, 0 };
    np_w8(&w, 'S'); np_w8(&w, 'C'); np_w8(&w, '6'); np_w8(&w, 'R');
    np_w16(&w, h->proto); np_w8(&w, h->type); np_w8(&w, h->flags);
    np_w32(&w, h->nonce_tx); np_w32(&w, h->nonce_rx); np_w16(&w, h->battle); np_w16(&w, h->len); np_w32(&w, h->seq);
    if (len) memcpy(out + NP_HDR, payload, len);
    uint32_t c = np_crc32(out + NP_HDR, len, np_crc32(out, 24, 0));   /* one running CRC over bytes 0..23 then the payload (chainable form) */
    h->crc = c;
    w.n = 24; np_w32(&w, c);
    return w.over ? 0 : NP_HDR + len;
}
int np_decode(const uint8_t *in, size_t n, NpHdr *h, const uint8_t **payload) {
    memset(h, 0, sizeof *h);
    if (n < NP_HDR || in[0] != 'S' || in[1] != 'C' || in[2] != '6' || in[3] != 'R') return NP_EMAGIC;
    NpR r = { in, 4, n, 0 };
    h->proto = np_r16(&r); h->type = np_r8(&r); h->flags = np_r8(&r);
    h->nonce_tx = np_r32(&r); h->nonce_rx = np_r32(&r); h->battle = np_r16(&r); h->len = np_r16(&r); h->seq = np_r32(&r); h->crc = np_r32(&r);
    if ((size_t)NP_HDR + h->len != n) return NP_ELEN;
    uint32_t c = np_crc32(in + NP_HDR, h->len, np_crc32(in, 24, 0));
    if (c != h->crc) return NP_ECRC;
    *payload = in + NP_HDR;
    if (h->proto != NP_PROTO) return NP_EPROTO;
    return NP_OK;
}

/* ---------------------------------------------------------------- HELLO */
size_t np_hello_put(uint8_t *out, const NpHello *h) {
    NpW w = { out, 0, NP_HELLO_LEN, 0 };
    np_w8(&w, h->role); np_w8(&w, 0); np_w16(&w, 0);
    char v[24]; memset(v, 0, sizeof v); memcpy(v, h->version, strnlen(h->version, sizeof h->version)); np_wb(&w, v, 24);
    np_w64(&w, h->plugin_sha); np_w32(&w, h->exe_ts); np_w32(&w, h->exe_soi);
    np_w64(&w, h->modset2); np_w64(&w, h->plugins_list); np_w64(&w, h->simcfg_net);
    np_w32(&w, h->mxcsr); np_w16(&w, h->x87cw); np_w16(&w, 0);
    np_w32(&w, h->ucrt_ms); np_w32(&w, h->ucrt_ls); np_w32(&w, h->win_build);
    np_wb(&w, h->cpu_vendor, 12); np_w32(&w, h->cpu_fms); np_w32(&w, h->cpu_feat);
    np_w32(&w, 0);   /* reserved: the 112-byte payload */
    return w.over ? 0 : w.n;
}
int np_hello_get(const uint8_t *in, size_t n, NpHello *h) {
    if (n != NP_HELLO_LEN) return 0;
    NpR r = { in, 0, n, 0 };
    memset(h, 0, sizeof *h);
    h->role = np_r8(&r); np_r8(&r); np_r16(&r);
    np_rb(&r, h->version, 24); h->version[23] = 0;
    h->plugin_sha = np_r64(&r); h->exe_ts = np_r32(&r); h->exe_soi = np_r32(&r);
    h->modset2 = np_r64(&r); h->plugins_list = np_r64(&r); h->simcfg_net = np_r64(&r);
    h->mxcsr = np_r32(&r); h->x87cw = np_r16(&r); np_r16(&r);
    h->ucrt_ms = np_r32(&r); h->ucrt_ls = np_r32(&r); h->win_build = np_r32(&r);
    np_rb(&r, h->cpu_vendor, 12); h->cpu_fms = np_r32(&r); h->cpu_feat = np_r32(&r); np_r32(&r);
    return !r.under;
}

/* ---------------------------------------------------------------- INPUT */
size_t np_input_put(uint8_t *out, size_t cap, const NpInput *in) {
    if (in->count > NP_IN_MAXREC || in->ccount > NP_IN_MAXCHK || (in->rec_size != NP_REC_WORD && in->rec_size != NP_REC_WORDX)) return 0;
    NpW w = { out, 0, cap, 0 };
    np_w32(&w, in->first); np_w8(&w, in->count); np_w8(&w, in->rec_size); np_w16(&w, in->iflags);
    np_w32(&w, in->ack); np_w32(&w, in->s); np_w16(&w, (uint16_t)in->adv16); np_w16(&w, 0);
    np_w32(&w, in->ping_id); np_w32(&w, in->echo_id); np_w32(&w, in->echo_hold_us); np_w32(&w, in->cconf);
    for (int i = 0; i < in->count; i++) { np_w64(&w, in->rec[i].w); if (in->rec_size == NP_REC_WORDX) np_wb(&w, in->rec[i].x, 24); }
    np_w32(&w, in->cfirst); np_w8(&w, in->ccount); np_w8(&w, 0); np_w16(&w, 0);
    for (int i = 0; i < in->ccount; i++) { np_w64(&w, in->chk[i].t1); np_w64(&w, in->chk[i].t15); np_w32(&w, in->chk[i].ued); np_w32(&w, 0); }
    np_w32(&w, in->cack);
    return w.over ? 0 : w.n;
}
int np_input_get(const uint8_t *in, size_t n, NpInput *o) {
    NpR r = { in, 0, n, 0 };
    memset(o, 0, sizeof *o);
    o->first = np_r32(&r); o->count = np_r8(&r); o->rec_size = np_r8(&r); o->iflags = np_r16(&r);
    o->ack = np_r32(&r); o->s = np_r32(&r); o->adv16 = (int16_t)np_r16(&r); np_r16(&r);
    o->ping_id = np_r32(&r); o->echo_id = np_r32(&r); o->echo_hold_us = np_r32(&r); o->cconf = np_r32(&r);
    if (o->count > NP_IN_MAXREC || (o->rec_size != NP_REC_WORD && o->rec_size != NP_REC_WORDX)) return 0;
    for (int i = 0; i < o->count; i++) { o->rec[i].w = np_r64(&r); if (o->rec_size == NP_REC_WORDX) np_rb(&r, o->rec[i].x, 24); }
    o->cfirst = np_r32(&r); o->ccount = np_r8(&r); np_r8(&r); np_r16(&r);
    if (o->ccount > NP_IN_MAXCHK) return 0;
    for (int i = 0; i < o->ccount; i++) { o->chk[i].t1 = np_r64(&r); o->chk[i].t15 = np_r64(&r); o->chk[i].ued = np_r32(&r); np_r32(&r); }
    o->cack = np_r32(&r);
    return !r.under && r.n == n;
}

/* ---------------------------------------------------------------- small messages */
size_t np_ping_put(uint8_t *o, const NpPing *p) { NpW w = { o, 0, 16, 0 }; np_w32(&w, p->id); np_w64(&w, p->t_us); np_w32(&w, p->hold_us); return w.n; }
int np_ping_get(const uint8_t *i, size_t n, NpPing *p) { NpR r = { i, 0, n, 0 }; p->id = np_r32(&r); p->t_us = np_r64(&r); p->hold_us = n >= 16 ? np_r32(&r) : 0; return !r.under && n >= 12; }
size_t np_seed_put(uint8_t *o, const NpSeed *p) { NpW w = { o, 0, 8, 0 }; np_w32(&w, p->seq); np_w32(&w, p->seed); return w.n; }
int np_seed_get(const uint8_t *i, size_t n, NpSeed *p) { NpR r = { i, 0, n, 0 }; p->seq = np_r32(&r); p->seed = n >= 8 ? np_r32(&r) : 0; return !r.under && n >= 4; }
size_t np_setup_part_put(uint8_t *o, size_t cap, const NpSetupPart *p) {
    if (p->dlen > NP_SETUP_FRAG) return 0;
    NpW w = { o, 0, cap, 0 };
    np_w16(&w, p->idx); np_w16(&w, p->count); np_w32(&w, p->total_len); np_w64(&w, p->total_fnv); np_wb(&w, p->data, p->dlen);
    return w.over ? 0 : w.n;
}
int np_setup_part_get(const uint8_t *i, size_t n, NpSetupPart *p) {
    if (n < 16) return 0;
    NpR r = { i, 0, n, 0 };
    p->idx = np_r16(&r); p->count = np_r16(&r); p->total_len = np_r32(&r); p->total_fnv = np_r64(&r);
    p->dlen = (uint16_t)(n - 16); p->data = i + 16;
    return p->dlen <= NP_SETUP_FRAG && p->count >= 1 && p->idx < p->count;
}
size_t np_zone_put(uint8_t *o, const NpZone *p) { NpW w = { o, 0, 10, 0 }; np_w16(&w, p->idx); np_w32(&w, p->z); np_w32(&w, p->e); return w.n; }
int np_zone_get(const uint8_t *i, size_t n, NpZone *p) { NpR r = { i, 0, n, 0 }; p->idx = np_r16(&r); p->z = np_r32(&r); p->e = np_r32(&r); return !r.under; }
size_t np_desync_put(uint8_t *o, const NpDesyncNote *p) {
    NpW w = { o, 0, 4 + 8 + 8 + 1 + 25 * 8, 0 };
    np_w32(&w, p->f); np_w64(&w, p->local_t1); np_w64(&w, p->peer_t1); np_w8(&w, p->comps_ok); for (int k = 0; k < 25; k++) np_w64(&w, p->comps[k]);
    return w.n;
}
int np_desync_get(const uint8_t *i, size_t n, NpDesyncNote *p) {
    NpR r = { i, 0, n, 0 };
    p->f = np_r32(&r); p->local_t1 = np_r64(&r); p->peer_t1 = np_r64(&r); p->comps_ok = np_r8(&r); for (int k = 0; k < 25; k++) p->comps[k] = np_r64(&r);
    return !r.under;
}
size_t np_dumpreq_put(uint8_t *o, const NpDumpReq *p) {
    NpW w = { o, 0, 1 + 4 + 4 + 1 + 25 * 8, 0 };
    np_w8(&w, p->kind); np_w32(&w, p->f); np_w32(&w, p->g); np_w8(&w, p->comps_ok); for (int k = 0; k < 25; k++) np_w64(&w, p->comps[k]);
    return w.n;
}
int np_dumpreq_get(const uint8_t *i, size_t n, NpDumpReq *p) {
    NpR r = { i, 0, n, 0 };
    p->kind = np_r8(&r); p->f = np_r32(&r); p->g = np_r32(&r); p->comps_ok = np_r8(&r); for (int k = 0; k < 25; k++) p->comps[k] = np_r64(&r);
    return !r.under;
}
size_t np_dumpack_put(uint8_t *o, const NpDumpAck *p) { NpW w = { o, 0, 6, 0 }; np_w8(&w, p->kind); np_w32(&w, p->g); np_w8(&w, p->ok); return w.n; }
int np_dumpack_get(const uint8_t *i, size_t n, NpDumpAck *p) { NpR r = { i, 0, n, 0 }; p->kind = np_r8(&r); p->g = np_r32(&r); p->ok = np_r8(&r); return !r.under; }
size_t np_end_put(uint8_t *o, const NpEnd *p) { NpW w = { o, 0, 5, 0 }; np_w8(&w, p->reason); np_w32(&w, p->frame); return w.n; }
int np_end_get(const uint8_t *i, size_t n, NpEnd *p) { NpR r = { i, 0, n, 0 }; p->reason = np_r8(&r); p->frame = np_r32(&r); return !r.under; }
size_t np_pause_put(uint8_t *o, const NpPause *p) { NpW w = { o, 0, 8, 0 }; np_w32(&w, p->frame); np_w32(&w, p->paused_ms); return w.n; }
int np_pause_get(const uint8_t *i, size_t n, NpPause *p) { NpR r = { i, 0, n, 0 }; p->frame = np_r32(&r); p->paused_ms = n >= 8 ? np_r32(&r) : 0; return !r.under && n >= 4; }
