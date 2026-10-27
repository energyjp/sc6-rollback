/* Option A M1 (OPTA-M0.md §2.3, §5): the ONLY netobserve translation unit that patches the game. It verifies every signature
 * first, wires each naked thunk (src/nobs_thunks.S) to its original + private stack, then enables the patches: vtable-slot
 * writes for P1-P4 and P6-P10, and the rel32 retarget at 0x1403eed2f for P5 (no MinHook in this mode). Runs once at process
 * start, after the version guard, and returns. Its imports are an exact allowlist (tools/nobs_static.py): VirtualProtect /
 * VirtualAlloc / VirtualFree / FlushInstructionCache / GetCurrentProcess / GetCurrentProcessId / GetSystemInfo + the log and
 * mem_readable helpers; no networking, thread, ns, rb, snap or MinHook symbol. CHAN4 / SEL5 are NOT read here any more (M3: at
 * process start CommSystem is still zero-filled); the writer asks nobs_body.c for them at the first online tick of a battle. */
#include "nobs.h"

static void *thunk_entry(int hook) {
    switch (hook) {
    case NOBS_H_TICK: return (void *)nobs_th_tick;
    case NOBS_H_IFC_INIT: return (void *)nobs_th_ifc_init;
    case NOBS_H_IFC_SHUTDOWN: return (void *)nobs_th_ifc_shutdown;
    case NOBS_H_IFC_STEP: return (void *)nobs_th_ifc_step;
    case NOBS_H_SENDCTL: return (void *)nobs_th_sendctl;
    case NOBS_H_SSM: return (void *)nobs_th_ssm;
    case NOBS_H_ISPAUSED: return (void *)nobs_th_ispaused;
    case NOBS_H_SETSTATE: return (void *)nobs_th_setstate;
    case NOBS_H_RRESET: return (void *)nobs_th_rreset;
    default: return NULL;
    }
}
void *nobs_thunk_entry(int hook) { return thunk_entry(hook); }
/* nobs_hooktab[id] = { orig +0, pstk +8, xarea +16 } (nobs_thunks.S reads it; written once here, before the slot is enabled) */
void nobs_thunk_wire(int hook, uint64_t orig, void *pstk, void *xarea) {
    nobs_hooktab[hook * 4 + 0] = orig; nobs_hooktab[hook * 4 + 1] = (uint64_t)(uintptr_t)pstk; nobs_hooktab[hook * 4 + 2] = (uint64_t)(uintptr_t)xarea;
}
/* the plugin image range, for the record-only VEH's "is this fault ours" filter */
extern IMAGE_DOS_HEADER __ImageBase;
void nobs_self_range(uintptr_t *lo, uintptr_t *hi) {
    const uint8_t *b = (const uint8_t *)&__ImageBase;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(b + __ImageBase.e_lfanew);
    *lo = (uintptr_t)b; *hi = (uintptr_t)b + nt->OptionalHeader.SizeOfImage;
}

static int np_write(void *dst, const void *src, size_t n) {
    DWORD old;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(dst, src, n);
    DWORD tmp; VirtualProtect(dst, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return 1;
}
static int np_patch_ptr(void *slot, void *val) { return np_write(slot, &val, sizeof val); }

/* A rel32 `call` reaches only +-2 GiB, and the plugin (nobs_enq_hook) can be farther than that from the call site (certainly in
 * the harness, and possible for a DLL in the real process). So the rel32 is retargeted to a 12-byte absolute-jump trampoline
 * placed within +-2 GiB of the call site (the same thing MinHook does for a far hook). This is still observe-only: the
 * trampoline just forwards to nobs_enq_hook. Returns the trampoline address, or NULL. */
static void *alloc_near(uintptr_t anchor) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    for (int dir = 0; dir < 2; dir++)
        for (uintptr_t d = gran; d < 0x60000000ULL; d += gran) {
            uintptr_t a = dir ? anchor + d : anchor - d;
            a &= ~(gran - 1);
            /* the x64 user-address limit, the same constant readable() uses. It was 0x7ff000000000 until the first live session:
             * the game's exe loads at about 0x7ff6_132f_0000, ABOVE that, so every candidate was skipped and the trampoline
             * could never be allocated (GATE FAIL enq_trampoline). The harness never saw it because its fake image is allocated
             * low. NOBS-NEAR now covers a high anchor. */
            if (a < 0x10000 || a > 0x7ffffffeffffULL) continue;
            void *p = VirtualAlloc((void *)a, gran, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) {
                intptr_t off = (intptr_t)p - (intptr_t)anchor;
                if (off > -0x7ff00000LL && off < 0x7ff00000LL) return p;
                VirtualFree(p, 0, MEM_RELEASE);
            }
        }
    return NULL;
}

#ifdef SC6RB_HARNESS
void *nobs_h_alloc_near(uintptr_t anchor) { return alloc_near(anchor); }
#endif

static int hexbyte2(const char *h) { int v = 0; sscanf(h, "%2x", &v); return v; }
static int prologue_ok(uint64_t va, const char *hex) {
    const uint8_t *p = (const uint8_t *)VA(va);
    size_t n = strlen(hex) / 2;
    if (!mem_readable(p, n)) return 0;
    for (size_t i = 0; i < n; i++) if (p[i] != hexbyte2(hex + i * 2)) return 0;
    return 1;
}

/* the patch set (OPTA-M0.md §2.3, as amended by §5 item 2) */
typedef struct { int hook; const char *name; uint64_t slot, orig; const char *prologue; int core; } Patch;
static const Patch k_patch[] = {
    { NOBS_H_TICK,         "tick",         NOBS_A_TICK_SLOT,                 NOBS_A_TICK,       "40534883ec30488bd9",           1 },
    { NOBS_H_IFC_INIT,     "ifc_init",     NOBS_A_IFC_VT + NOBS_IFC_INIT,    NOBS_A_INIT_THUNK, "488bcae9586bf1ff",             0 },
    { NOBS_H_IFC_SHUTDOWN, "ifc_shutdown", NOBS_A_IFC_VT + NOBS_IFC_SHUTDOWN,NOBS_A_SHUT_THUNK, "e93bd6f1ff",                   0 },
    { NOBS_H_IFC_STEP,     "ifc_step",     NOBS_A_IFC_VT + NOBS_IFC_STEP,    NOBS_A_STEP_THUNK, "488bcae93892f0ff",             1 },
    { NOBS_H_SENDCTL,      "sendctl",      NOBS_A_SYNC_VT + NOBS_SYNC_SENDCTL,  NOBS_A_SENDCTL,  "488bc453574881ec88000000",     0 },
    { NOBS_H_SSM,          "ssm",          NOBS_A_SYNC_VT + NOBS_SYNC_SSM,      NOBS_A_SSM,      "40574883ec300fb6812444",       0 },
    { NOBS_H_ISPAUSED,     "ispaused",     NOBS_A_SYNC_VT + NOBS_SYNC_ISPAUSED, NOBS_A_ISPAUSED, "40534883ec3080b904440000",     0 },
    { NOBS_H_SETSTATE,     "setstate",     NOBS_A_SYNC_VT + NOBS_SYNC_SETSTATE, NOBS_A_SETSTATE, "48895c2408574883ec30",         0 },
    { NOBS_H_RRESET,       "rreset",       NOBS_A_SYNC_VT + NOBS_SYNC_RRESET,   NOBS_A_RRESET,   "40534883ec20488bd9",           0 },
};
#define NPATCH ((int)(sizeof k_patch / sizeof k_patch[0]))

/* the retarget of the enqueue call (P5, OPTA-M0.md §5 item 2): rewrite the rel32 of the `call` at 0x1403eed2f */
static int retarget_enq(void) {
    const uint8_t *cs = (const uint8_t *)VA(NOBS_A_ENQ_CALL);
    if (!mem_readable(cs, 5) || cs[0] != 0xe8) { nobs_gate_fail("enq", 0); return 0; }
    int32_t cur = (int32_t)(cs[1] | cs[2] << 8 | cs[3] << 16 | (uint32_t)cs[4] << 24);
    uint64_t cur_target = (uint64_t)((intptr_t)cs + 5 + cur);
    if (cur_target != VA(NOBS_A_ENQ)) { nobs_gate_fail("enq_target", 0); return 0; }
    if (!prologue_ok(NOBS_A_ENQ, "48895c241048896c2418")) { nobs_gate_fail("enq_sig", 0); return 0; }
    nobs_o_enq = (NobsEnqFn)VA(NOBS_A_ENQ);   /* the original the hook forwards to (the only cast of a game address to a function) */
    void *target = (void *)nobs_enq_hook;
    intptr_t direct = (intptr_t)target - ((intptr_t)cs + 5);
    if (direct <= -0x7ff00000LL || direct >= 0x7ff00000LL) {
        void *tr = alloc_near((uintptr_t)cs);
        if (!tr) { nobs_gate_fail("enq_trampoline", 0); return 0; }
        uint8_t jmp[12] = { 0x48, 0xb8, 0,0,0,0,0,0,0,0, 0xff, 0xe0 };   /* mov rax, imm64 ; jmp rax */
        memcpy(jmp + 2, &target, 8);
        memcpy(tr, jmp, sizeof jmp);
        FlushInstructionCache(GetCurrentProcess(), tr, sizeof jmp);
        target = tr;
    }
    int32_t rel = (int32_t)((intptr_t)target - ((intptr_t)cs + 5));
    uint8_t ins[5]; ins[0] = 0xe8; ins[1] = rel & 0xff; ins[2] = (rel >> 8) & 0xff; ins[3] = (rel >> 16) & 0xff; ins[4] = (rel >> 24) & 0xff;
    int ok = np_write((void *)cs, ins, 5);
    nobs_note_patch("enq", NOBS_A_ENQ, "rel32", 1, VA(NOBS_A_ENQ), cur_target, ok);
    return ok;
}

void nobs_install(void) {
    LOG("NOBS INSTALL begin version=%s pid=%lu\n", SC6RB_VERSION, GetCurrentProcessId());
    nobs_cfg_ignored();
    if (!nobs_runtime_init()) { nobs_gate_fail("runtime_init", 1); LOG("NOBS INSTALL abort: runtime_init failed\n"); log_flush(); return; }

    /* phase 1: verify every signature (slot holds the expected original AND the target prologue matches T-M1-1) */
    int core_bad = 0, any_bad = 0;
    int sig_ok[NPATCH];
    for (int i = 0; i < NPATCH; i++) {
        uint64_t slotv = 0; int slot_ok = mem_readable((const void *)VA(k_patch[i].slot), 8);
        if (slot_ok) slotv = *(uint64_t *)VA(k_patch[i].slot);
        int ok = slot_ok && slotv == VA(k_patch[i].orig) && prologue_ok(k_patch[i].orig, k_patch[i].prologue);
#ifdef SC6RB_HARNESS
        if (g_cfg.h_nobs_plant_sig && k_patch[i].hook == NOBS_H_SSM) ok = 0;   /* negative control: corrupt P7 */
#endif
        sig_ok[i] = ok;
        nobs_note_patch(k_patch[i].name, k_patch[i].orig, "vtbl", ok, VA(k_patch[i].orig), slotv, 0);
        if (!ok) { any_bad = 1; if (k_patch[i].core) core_bad = 1; nobs_gate_fail(k_patch[i].name, k_patch[i].core); }
    }
    if (core_bad) { LOG("NOBS INSTALL abort: a core signature failed; nothing patched\n"); log_flush(); return; }

    /* phase 2: wire each thunk (orig + private stack), only for hooks whose signature passed */
    for (int i = 0; i < NPATCH; i++)
        if (sig_ok[i]) nobs_thunk_wire(k_patch[i].hook, VA(k_patch[i].orig), nobs_pstack_top(k_patch[i].hook), nobs_xarea[k_patch[i].hook]);

    /* phase 3: enable (patch the slots). Core hooks first so a later non-core failure still leaves them observing. */
    int n_installed = 0; char names[256] = ""; size_t no = 0;
    for (int i = 0; i < NPATCH; i++) {
        if (!sig_ok[i]) continue;
        if (np_patch_ptr((void *)VA(k_patch[i].slot), thunk_entry(k_patch[i].hook))) { n_installed++; no += (size_t)snprintf(names + no, sizeof names - no, "%s%s", no ? "," : "", k_patch[i].name); }
        else nobs_gate_fail(k_patch[i].name, k_patch[i].core);
    }
    /* P5 enqueue retarget */
    int enq_ok = retarget_enq();
    if (enq_ok) no += (size_t)snprintf(names + no, sizeof names - no, "%s%s", no ? "," : "", "enq");

    nobs_patchset(n_installed + (enq_ok ? 1 : 0), names, !any_bad && enq_ok);
    nobs_ready();
    char tok[96]; nobs_title_token(tok, sizeof tok);
    LOG("NOBS TITLE %s\n", tok);
    LOG("NOBS INSTALL done installed=%d enq=%d any_bad=%d\n", n_installed, enq_ok, any_bad);
    log_flush();
}
