/* v0.34.0 (OPTA-M2-SPEC.md §23.1): the EvaluateIf argc fix (src/argcfix.h has the bug, the bytes and the rules of the core).
 *
 * WHERE IT IS ON (DECISIONS items 2-3; BRIEF goal 1):
 *   rbreplay   argc_fix=1 (the default): applied once at install (DllMain's plugin_init, before the game runs a battle: no move script can run then)
 *              and kept for the whole process; argc_fix=0 = the stock bytes (the control of session step 0b). Restored at detach.
 *   rbonline   never at install and never while passive. rbo_install only checks that the four sites are stock and readable (g_argc_cap). The
 *              HELLO advertises RBM_HF_ARGC when argc_fix=1 and g_argc_cap; the verdict compares the flag (RBM_V_ARGC): two PCs that differ never
 *              pair (PASSIVE why=argc_off on both, src/rbo.c). Two PCs that both advertise it apply it at the agreed ARM frame (the D-C switch,
 *              in the same IsPaused pre that switches the step context, on the game thread outside a step) and restore it at the battle's END.
 *   any other mode: nothing (the key is logged as ignored).
 * Every write goes through argc_apply / argc_restore: the four signatures first, all or nothing, the protection restored. */
#include "common.h"
#include "argcfix.h"

const ArgcSite k_argc_sites[ARGC_NSITES] = {
    { 0x1403739bfULL, { 0x0f, 0x88, 0x05, 0x1a, 0x00, 0x00 }, { 0x0f, 0x8e, 0x05, 0x1a, 0x00, 0x00 }, "sub25_entry_js_to_jle" },
    { 0x140373a5eULL, { 0x0f, 0x8e, 0x71, 0xff, 0xff, 0xff }, { 0x0f, 0x8c, 0x71, 0xff, 0xff, 0xff }, "sub25_loop_jle_to_jl" },
    { 0x140373b30ULL, { 0x0f, 0x88, 0x94, 0x18, 0x00, 0x00 }, { 0x0f, 0x8e, 0x94, 0x18, 0x00, 0x00 }, "sub91_entry_js_to_jle" },
    { 0x140373bddULL, { 0x0f, 0x8e, 0x6d, 0xff, 0xff, 0xff }, { 0x0f, 0x8c, 0x6d, 0xff, 0xff, 0xff }, "sub91_loop_jle_to_jl" },
};
const char *const k_argc_site_nm[4] = { "stock", "fixed", "other", "unreadable" };
const char *const k_argc_why[ARGC_W_N] = { "ok", "already", "signature", "protect", "verify" };
#ifdef SC6RB_HARNESS
int argc_h_skip_write = -1, argc_h_vp_fail, argc_h_no_rollback, argc_h_no_precheck;
#endif

static uintptr_t site_va(uintptr_t base, int i) { return base + (uintptr_t)(k_argc_sites[i].va - 0x140000000ULL); }
static int readable6(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mi; uintptr_t e = a + 6;
    while (a < e) {
        if (!VirtualQuery((const void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        a = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
}
int argc_site_state_raw(uintptr_t base, int i) {
    /* no CRT call (src/rbo.c reads the sites on a passive END path): byte loops only */
    const volatile uint8_t *p = (const volatile uint8_t *)site_va(base, i);
    int st = 1, fx = 1;
    for (int k = 0; k < 6; k++) { uint8_t b = p[k]; if (b != k_argc_sites[i].stock[k]) st = 0; if (b != k_argc_sites[i].fixed[k]) fx = 0; }
    return st ? ARGC_SITE_STOCK : fx ? ARGC_SITE_FIXED : ARGC_SITE_OTHER;
}
int argc_site_state(uintptr_t base, int i) {
    if (!readable6(site_va(base, i))) return ARGC_SITE_UNREADABLE;
    return argc_site_state_raw(base, i);
}
void argc_states_str(const int st[ARGC_NSITES], char *out, size_t cap) {
    size_t o = 0; if (cap) out[0] = 0;
    for (int i = 0; i < ARGC_NSITES; i++) {
        int s = st[i] >= 0 && st[i] < 4 ? st[i] : 2;
        int k = snprintf(out + o, cap > o ? cap - o : 0, "%s%s", i ? "," : "", k_argc_site_nm[s]);
        if (k > 0 && o + (size_t)k < cap) o += (size_t)k; else break;
    }
}

/* the distinct pages of the four stores (the byte at site + 1); real exe: one page */
static int pages_of(uintptr_t base, uintptr_t pg[ARGC_NSITES]) {
    int n = 0;
    for (int i = 0; i < ARGC_NSITES; i++) {
        uintptr_t p = (site_va(base, i) + 1) & ~(uintptr_t)0xfff; int dup = 0;
        for (int k = 0; k < n; k++) if (pg[k] == p) dup = 1;
        if (!dup) pg[n++] = p;
    }
    return n;
}
static int prot_open(uintptr_t pg[], int n, DWORD old[]) {
    for (int k = 0; k < n; k++) {
        BOOL ok = VirtualProtect((void *)pg[k], 0x1000, PAGE_EXECUTE_READWRITE, &old[k]);
#ifdef SC6RB_HARNESS
        if (argc_h_vp_fail && k == n - 1) { if (ok) { DWORD t; VirtualProtect((void *)pg[k], 0x1000, old[k], &t); } ok = FALSE; }
#endif
        if (!ok) { for (int j = 0; j < k; j++) { DWORD t; VirtualProtect((void *)pg[j], 0x1000, old[j], &t); } return 0; }
    }
    return 1;
}
static void prot_close(uintptr_t pg[], int n, const DWORD old[]) {
    for (int k = 0; k < n; k++) { DWORD t; VirtualProtect((void *)pg[k], 0x1000, old[k], &t); }
    for (int k = 0; k < n; k++) FlushInstructionCache(GetCurrentProcess(), (void *)pg[k], 0x1000);
}
/* one transition: from = the state every site must hold before any write, to = the state wanted. All four signatures are checked first; a store that
 * does not read back puts every site back to `from` (the rollback) */
static int transition(uintptr_t base, int from, int to, ArgcRes *r) {
    memset(r, 0, sizeof *r); r->first_bad = -1;
    int st[ARGC_NSITES], n_to = 0, n_from = 0;
    for (int i = 0; i < ARGC_NSITES; i++) { st[i] = argc_site_state(base, i); if (st[i] == to) n_to++; else if (st[i] == from) n_from++; }
    memcpy(r->st, st, sizeof st);
    if (n_to == ARGC_NSITES) { r->ok = 1; r->why = ARGC_W_ALREADY; return 1; }
    int pre = 1;
#ifdef SC6RB_HARNESS
    if (argc_h_no_precheck) pre = 0;   /* control XSIGPRE: every site that holds `from` is written, whatever the others hold */
#endif
    if (pre && n_from != ARGC_NSITES) {
        for (int i = 0; i < ARGC_NSITES && r->first_bad < 0; i++) if (st[i] != from) r->first_bad = i;
        r->why = ARGC_W_SIG; return 0;
    }
    uintptr_t pg[ARGC_NSITES]; DWORD old[ARGC_NSITES]; int np = pages_of(base, pg);
    if (!prot_open(pg, np, old)) { r->why = ARGC_W_PROTECT; return 0; }
    for (int i = 0; i < ARGC_NSITES; i++) {
        if (st[i] != from) continue;   /* only reached without the pre-check (the control) */
#ifdef SC6RB_HARNESS
        if (argc_h_skip_write == i) continue;   /* a store that does not take */
#endif
        const uint8_t *want = to == ARGC_SITE_FIXED ? k_argc_sites[i].fixed : k_argc_sites[i].stock;
        *(volatile uint8_t *)(site_va(base, i) + 1) = want[1];   /* the jcc's second opcode byte: one aligned-or-not byte store (atomic) */
        r->written++;
    }
    int bad = 0;
    for (int i = 0; i < ARGC_NSITES; i++) { st[i] = argc_site_state_raw(base, i); if (st[i] != to) bad = 1; }
    int roll = bad;
#ifdef SC6RB_HARNESS
    if (argc_h_no_rollback) roll = 0;   /* control XROLL: a partial write is left in place */
#endif
    if (roll) {
        for (int i = 0; i < ARGC_NSITES; i++) {
            if (st[i] != to) continue;
            const uint8_t *back = from == ARGC_SITE_FIXED ? k_argc_sites[i].fixed : k_argc_sites[i].stock;
            *(volatile uint8_t *)(site_va(base, i) + 1) = back[1]; r->written++;
        }
        r->rolled_back = 1;
        for (int i = 0; i < ARGC_NSITES; i++) st[i] = argc_site_state_raw(base, i);
    }
    prot_close(pg, np, old);
    memcpy(r->st, st, sizeof st);
    if (bad) { r->why = ARGC_W_VERIFY; for (int i = 0; i < ARGC_NSITES && r->first_bad < 0; i++) if (st[i] != to) r->first_bad = i; if (r->first_bad < 0) r->first_bad = 0; return 0; }
    r->ok = 1; r->why = ARGC_W_OK;
    return 1;
}
int argc_apply(uintptr_t base, ArgcRes *r) { return transition(base, ARGC_SITE_STOCK, ARGC_SITE_FIXED, r); }
int argc_restore(uintptr_t base, ArgcRes *r) { return transition(base, ARGC_SITE_FIXED, ARGC_SITE_STOCK, r); }

int argc_pred(const ArgcIn *in, const uint16_t *args, int argc, int fixed) {
    int last = fixed ? argc - 1 : argc;
    for (int i = 1; i <= last; i++) {
        uint16_t w = args[i]; uint32_t cls = w & 0xf000u;
        if (cls == 0x3000u) { if (!(in->c3 & w)) return 0; continue; }
        if (cls == 0x4000u) { if (!(in->c4 & w)) return 0; continue; }
        if (cls != 0x1000u && cls != 0x2000u) continue;
        uint16_t f = cls == 0x1000u ? in->c1 : in->c2, low = (uint16_t)(w & 0xfffu);
        if (low == 0) { if (f != 0) return 0; }
        else if (low == 5) { }
        else if (low == 0xa) { if (f == 0) return 0; }
        else if (f != low) return 0;
    }
    return 1;
}

/* ================================================================== the plugin glue (g_base) */
volatile int g_argc_cap, g_argc_broken;
static int g_rb_on, g_rb_key = -1, g_rb_why;   /* rbreplay: applied; the key; why off */
enum { RBW_A_ON = 0, RBW_A_KEY, RBW_A_BADKEY, RBW_A_FAIL };
static const char *const k_rb_why[] = { "applied", "argc_fix=0", "bad_key", "apply_failed" };

static int key_now(void) { return cfg_geti("argc_fix", 1); }
void argc_install(void) {
    int key = key_now();
    if (g_mode != MODE_RBREPLAY) {
        LOG("ARGC INSTALL none mode=%s argc_fix=%d (v0.34.0: the EvaluateIf argc fix runs in rbreplay for the whole process and in an armed rbonline battle "
            "only; nothing patched)\n", g_mode_names[g_mode], key);
        return;
    }
    g_rb_key = key;
    int st[ARGC_NSITES]; char ss[64];
    if (key != 0 && key != 1) {
        for (int i = 0; i < ARGC_NSITES; i++) st[i] = argc_site_state(g_base, i);
        argc_states_str(st, ss, sizeof ss); g_rb_why = RBW_A_BADKEY;
        LOG("ARGC INSTALL off mode=rbreplay argc_fix=%d why=bad_key need=0|1 sites=%s (nothing patched: the stock bytes)\n", key, ss);
        return;
    }
    if (!key) {
        for (int i = 0; i < ARGC_NSITES; i++) st[i] = argc_site_state(g_base, i);
        argc_states_str(st, ss, sizeof ss); g_rb_why = RBW_A_KEY;
        LOG("ARGC INSTALL off mode=rbreplay argc_fix=0 why=key sites=%s (the stock bytes: sub-ops 0x25 / 0x91 read one argument past argc; the control)\n", ss);
        return;
    }
    ArgcRes r; argc_apply(g_base, &r);
    argc_states_str(r.st, ss, sizeof ss);
    if (r.ok) {
        g_rb_on = 1; g_rb_why = RBW_A_ON;
        LOG("ARGC INSTALL ok mode=rbreplay argc_fix=1 result=%s sites=%s written=%d page=0x%llx (sub-ops 0x25 / 0x91 now stop at args[argc-1] as 0x24 / 0x90 do; "
            "for the whole process)\n", k_argc_why[r.why], ss, r.written, (unsigned long long)(g_base + (ARGC_A_PAGE - 0x140000000ULL)));
    } else {
        g_rb_why = RBW_A_FAIL;
        LOG("ARGC INSTALL FAIL mode=rbreplay argc_fix=1 why=%s first_bad=%d sites=%s written=%d rolled_back=%d (nothing changed: the stock bytes for this process)\n",
            k_argc_why[r.why], r.first_bad, ss, r.written, r.rolled_back);
    }
}
void argc_detach(void) {
    if (!g_rb_on) return;
    ArgcRes r; argc_restore(g_base, &r); char ss[64]; argc_states_str(r.st, ss, sizeof ss);
    g_rb_on = 0;
    LOG("ARGC RESTORE %s why=%s sites=%s written=%d\n", r.ok ? "ok" : "FAIL", k_argc_why[r.why], ss, r.written);
}
void argc_battle_line(uint64_t battle) {
    if (g_mode != MODE_RBREPLAY || g_rb_key < 0) return;
    int st[ARGC_NSITES], nf = 0; char ss[64];
    for (int i = 0; i < ARGC_NSITES; i++) { st[i] = argc_site_state(g_base, i); if (st[i] == ARGC_SITE_FIXED) nf++; }
    argc_states_str(st, ss, sizeof ss);
    int ok = g_rb_on ? nf == ARGC_NSITES : nf == 0;
    LOG("ARGC b=%llu state=%s why=%s mode=rbreplay sites=%s sites_ok=%d\n", (unsigned long long)battle, g_rb_on ? "on" : "off", k_rb_why[g_rb_why], ss, ok);
}
void argc_rbo_install(void) {
    int key = key_now(), st[ARGC_NSITES], ns = 0; char ss[64];
    for (int i = 0; i < ARGC_NSITES; i++) { st[i] = argc_site_state(g_base, i); if (st[i] == ARGC_SITE_STOCK) ns++; }
    argc_states_str(st, ss, sizeof ss);
    int cap = key == 1 && ns == ARGC_NSITES;
#ifdef SC6RB_HARNESS
    if (key == 1 && ns != ARGC_NSITES && cfg_geti("h_argc_sig_ignore", 0)) { cap = 1; LOG("RB ARGC harness control h_argc_sig_ignore=1: advertised although the sites are not stock (XARGCSIG)\n"); }
#endif
    g_argc_cap = cap;
    LOG("RB ARGC INSTALL argc_fix=%d sites=%s cap=%d (%s)\n", key, ss, cap,
        cap ? "the HELLO advertises the fix; it is applied only at the ARM of a battle whose two PCs both advertise it, and restored at its END" :
        key != 1 ? "argc_fix is not 1: the HELLO does not advertise the fix (a peer that does refuses to pair: why=argc_off)" :
                   "a site is not the verified stock jcc: the HELLO does not advertise the fix (a peer that does refuses to pair: why=argc_off)");
}
