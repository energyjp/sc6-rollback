/* rbo_q1_table.h — the Q1 root-walk signature table (OPTA-M2-SPEC.md §16.3.1, copied verbatim; append-only after that).
 *
 * X-macro rows: the file that includes this one defines Q1SIG, Q1PTR and Q1VT (and #undefs them afterwards).
 *   Q1SIG(name, va, "hex")                        exact instruction bytes at that VA (checked against the image at process start).
 *                                                 NEVER a byte the loader relocates: the exe is DYNAMIC_BASE and loads at a randomized base
 *                                                 in every real process (tools/q1_static.py QS1 checks every row against the DIR64 list)
 *   Q1PTR(name, va, target_va)                    v0.29.3 (review B1): an absolute pointer the loader relocates (a DIR64 target, e.g. a
 *                                                 vtable slot in .rdata): checked as [VA(va)] == VA(target_va) at the module's real base
 *   Q1VT(entry, rtti, vtable_va, size, size_sig)  a vtable and its object size, tied to the Q1SIG row that proves the size
 * Every byte is V (§16.3.1: pefile + capstone on the exe, read-only). A row may not be changed without being re-verified against the
 * exe. The harness (test/harness_rbo*.inc, test/harness_c1b.inc) #includes this file to plant the same rows into every fake image (a
 * Q1PTR row as the loader writes it: the fake image's own address of the target).
 * No include guard on purpose: an X-macro table is expanded once per Q1SIG/Q1PTR/Q1VT definition (an identical #define is a legal
 * redefinition). The macros default to nothing, so including the file only for Q1_TABLE_VERSION is harmless. */
#ifndef Q1SIG
#define Q1SIG(name, va, hex)
#define Q1_TABLE_UNDEF_SIG 1
#endif
#ifndef Q1PTR
#define Q1PTR(name, va, target)
#define Q1_TABLE_UNDEF_PTR 1
#endif
#ifndef Q1VT
#define Q1VT(entry, rtti, vt, size, size_sig)
#define Q1_TABLE_UNDEF_VT 1
#endif
#define Q1_TABLE_VERSION 3   /* 2 = v0.29.3: + the Q1PTR row kind (OPTA-M2-SPEC.md §16.14 B1); 3 = v0.29.4 Q1: + the E5 / E7 / E8 rows */
Q1SIG("E1.ctrl_size",  0x1402de0a3ULL, "b9c0000000")       /* mov ecx,0xc0 */
Q1SIG("E1.ctrl_alloc", 0x1402de0a8ULL, "e813811c00")       /* call FMemory::Malloc 0x1404a61c0 */
Q1SIG("E1.ctrl_vt",    0x1402de0c8ULL, "488d054974ba03")   /* lea rax,[0x143e85518] */
Q1SIG("E1.node_size",  0x1402ddfdaULL, "b920000000")       /* mov ecx,0x20 */
Q1SIG("E1.node_alloc", 0x1402ddfe5ULL, "e8d6811c00")
Q1SIG("E1.root",       0x1402da009ULL, "488b3db0514304")   /* mov rdi,[0x14470f1c0] */
Q1SIG("E1.count",      0x1402da024ULL, "488b159d514304")   /* mov rdx,[0x14470f1c8] */
Q1SIG("E2.mgr_lea",    0x1402da310ULL, "488d054968e203")   /* lea rax,[0x144100b60] */
Q1SIG("E2.mgr_store",  0x1402da317ULL, "4889051a3d4304")   /* mov [0x14470e038],rax */
Q1SIG("E2.ro_root",    0x1403345a3ULL, "488b1d8e9a3d04")   /* mov rbx,[0x14470e038] */
Q1SIG("E2.ro_size",    0x1403345aaULL, "b930010000")
Q1SIG("E2.ro_alloc",   0x1403345afULL, "e80c1c1700")
Q1SIG("E2.ro_vt",      0x14033461aULL, "488d059746b503")   /* lea rax,[0x143e88cb8] */
Q1SIG("E2.owner",      0x140334628ULL, "48895e28")         /* mov [rsi+0x28],rbx */
Q1SIG("E2.next",       0x140334633ULL, "48894610")         /* mov [rsi+0x10],rax */
Q1SIG("E2.prev",       0x14033463fULL, "48897018")         /* mov [rax+0x18],rsi */
Q1SIG("E2.head",       0x140334643ULL, "488933")           /* mov [rbx],rsi */
Q1SIG("E2.par_size",   0x14033450cULL, "b930010000")
Q1SIG("E2.par_alloc",  0x140334511ULL, "e8aa1c1700")
Q1SIG("E2.par_vt",     0x14033455cULL, "488d052547b503")   /* lea rax,[0x143e88c88] */
Q1SIG("E2.gust_vt",    0x140334989ULL, "4c8d3d5843b503")   /* lea r15,[0x143e88ce8] */
Q1SIG("E2.gust_root",  0x1403349e1ULL, "488b3550963d04")   /* mov rsi,[0x14470e038] */
Q1SIG("E2.gust_size",  0x140334a14ULL, "b9e0010000")
Q1SIG("E2.gust_alloc", 0x140334a19ULL, "e8a2171700")
Q1SIG("E2.sw_size",    0x1402fc70eULL, "b980010000")
Q1SIG("E2.sw_alloc",   0x1402fc71bULL, "e8a09a1a00")
Q1SIG("E2.sw_vt",      0x1402fc767ULL, "488d05aac5b803")   /* lea rax,[0x143e88d18] */
Q1SIG("E2.sw_owner",   0x1402fc776ULL, "48897328")         /* mov [rbx+0x28],rsi */
Q1SIG("E3.mgr",        0x1402fa6bbULL, "488d8e20a10a00")   /* lea rcx,[rsi+0xaa120] */
Q1SIG("E3.res_ring",   0x14037d425ULL, "bad0020000")       /* mov edx,0x2d0 (720 items) */
Q1SIG("E3.res_key",    0x14037d453ULL, "ba94110000")       /* mov edx,0x1194 (4500 items) */
Q1SIG("E3.res_parity", 0x14037d493ULL, "ba8c000000")       /* mov edx,0x8c (140 items) */
Q1SIG("E3.alloc_sel",  0x14037f68cULL, "488b0d35e83804")   /* mov rcx,[0x14470dec8] */
Q1SIG("E3.crt_malloc", 0x14037f69bULL, "ff1557dcea02")     /* call [0x14322d2f8] (malloc IAT) */
Q1SIG("E3.cap_lea",    0x14037f6f5ULL, "498d043e")         /* lea rax,[r14+rdi] */
Q1SIG("E3.cap_store",  0x14037f6f9ULL, "48894310")         /* mov [rbx+0x10],rax */
Q1SIG("E4.size",       0x14038c4e6ULL, "b980000000")
Q1SIG("E4.alloc",      0x14038c4ebULL, "e8d09c1100")
Q1SIG("E4.vt",         0x14038c4fdULL, "488d0574d6af03")   /* lea rax,[0x143e89b78] */
Q1SIG("E4.list",       0x14038c507ULL, "488d5e70")         /* lea rbx,[rsi+0x70] */
Q1SIG("E4.list_node",  0x14038c51cULL, "e8af1af5ff")       /* call 0x1402ddfd0 */
Q1SIG("E4.store",      0x14038c531ULL, "48897730")         /* mov [rdi+0x30],rsi */
Q1SIG("E6.obj_size",   0x14037f811ULL, "b910000000")
Q1SIG("E6.obj_alloc",  0x14037f816ULL, "e8a5691200")
Q1SIG("E6.head",       0x14037f831ULL, "e8da270000")       /* call 0x140382010 */
Q1SIG("E6.store",      0x14037f842ULL, "48897320")         /* mov [rbx+0x20],rsi */
Q1SIG("E6.node_size",  0x140382014ULL, "b938000000")
Q1SIG("E6.node_alloc", 0x140382019ULL, "e8a2411200")
Q1VT("E1", ".?AV?$_Ref_count_obj@VIw_WindEmitter@@@std@@",             0x143e85518ULL, 0xc0,  "E1.ctrl_size")
Q1VT("E2", ".?AVIw_WindParallel@@",                                    0x143e88c88ULL, 0x130, "E2.par_size")
Q1VT("E2", ".?AVIw_WindRingOut@@",                                     0x143e88cb8ULL, 0x130, "E2.ro_size")
Q1VT("E2", ".?AVIw_WindForStage@@",                                    0x143e88ce8ULL, 0x1e0, "E2.gust_size")
Q1VT("E2", ".?AVIw_WindShockWave@@",                                   0x143e88d18ULL, 0x180, "E2.sw_size")
Q1VT("E3", ".?AV?$CRecordArrayBuffer@UstBattleFixedKeyRecordItem@@@@", 0x143e894c8ULL, 9000,  "E3.res_key")
Q1VT("E3", ".?AV?$CRecordArrayBuffer@UstHeaderParity@@@@",             0x143e89578ULL, 280,   "E3.res_parity")
Q1VT("E3", ".?AV?$CRecordRingBuffer@UstBattleFixedKeyRecordItem@@@@",  0x143e89470ULL, 1440,  "E3.res_ring")
Q1VT("E4", ".?AVKN_EnshutsuHeader@KN_EnshutsuHeaderManager@@",         0x143e89b78ULL, 0x80,  "E4.size")
/* ---- appended rows (rollback core) below this line; each re-verified against the exe ---- */
/* v0.29.2 core (§16.12 item 4: "each vector sits at obj+8"). V (2026-10-18, pefile + capstone, read-only): the record buffers' reserve
 * method (vtable slot +0x10) passes obj+8 to the u16 vector growth FUN_14037f670, which stores cap at [vector+0x10] (E3.cap_store) */
/* v0.29.3 (review B1): the three vtable slots below are absolute pointers in .rdata, DIR64 relocation targets: v0.29.2 declared them as
 * Q1SIG with the preferred-base bytes, so the gate failed (Q1 SIG ok=0 -> rb=OFF:q1sig / rb=REFUSED:q1_roots) at every randomized base the
 * real game loads at. Now Q1PTR rows: [VA(va)] == VA(target). V (2026-10-18, pefile, read-only): each va is exactly one DIR64 target and
 * the file's qword there is the target (tools/q1_static.py QS1; QS5 runs the plugin's own q1_sig_check over a relocated image) */
Q1PTR("E3.vt_key_res",    0x143e894d8ULL, 0x14037ed90ULL)       /* [vt 0x143e894c8 + 0x10] = 0x14037ed90 (the array buffer reserve) */
Q1PTR("E3.vt_par_res",    0x143e89588ULL, 0x14037ed90ULL)       /* [vt 0x143e89578 + 0x10] = 0x14037ed90 */
Q1PTR("E3.vt_ring_res",   0x143e89480ULL, 0x14037f020ULL)       /* [vt 0x143e89470 + 0x10] = 0x14037f020 (the ring buffer reserve) */
Q1SIG("E3.vec_arr",       0x14037edb2ULL, "4883c108")           /* add rcx,8: the vector at obj+8 */
Q1SIG("E3.vec_arr_grow",  0x14037edb6ULL, "e8b5080000")         /* call 0x14037f670 */
Q1SIG("E3.vec_ring",      0x14037f05dULL, "4883c108")           /* add rcx,8: the vector at obj+8 */
Q1SIG("E3.vec_ring_grow", 0x14037f074ULL, "e8f7050000")         /* call 0x14037f670 */
/* ---- v0.29.4 Q1 (Q1, 2026-10-18): E5 the vtb voice tables, E7 the container census, E8 the controllers' CPU direct-command objects.
 * V (2026-10-18, pefile + capstone, read-only): every row's bytes read from the exe, and the DIR64 list of the pages each row lies in parsed:
 * no row overlaps a relocated qword (tools/q1_static.py QS1 re-checks both). */
Q1SIG("E5.own0",      0x140341d04ULL, "488d8b88570900")       /* lea rcx,[rbx+0x95788]: sequencer object 0 (the step's ticker call) */
Q1SIG("E5.own1",      0x140341d10ULL, "488d8bf0590900")       /* lea rcx,[rbx+0x959f0]: object 1 (stride 0x268) */
Q1SIG("E5.own2",      0x1402d479aULL, "4881c1585c0900")       /* add rcx,0x95c58: object 2 */
Q1SIG("E5.asset",     0x14038efcdULL, "4889b940020000")       /* mov [rcx+0x240],rdi: the asset pointer */
Q1SIG("E5.off",       0x14038efe2ULL, "48636a10")             /* movsxd rbp,[rdx+0x10]: the event table offset */
Q1SIG("E5.ver_ld",    0x14038efebULL, "8b7208")               /* mov esi,[rdx+8]: the version */
Q1SIG("E5.cnt",       0x14038eff3ULL, "448b720c")             /* mov r14d,[rdx+0xc]: the event count */
Q1SIG("E5.magic_lea", 0x14038eff7ULL, "488d1582abaf03")       /* lea rdx,[0x143e89b80] (the "vtb" literal, a 4-byte compare) */
Q1SIG("E5.magic",     0x143e89b80ULL, "76746200")             /* .rdata "vtb", NUL-terminated (not a DIR64 target) */
Q1SIG("E5.ver",       0x14038f00cULL, "81fe02100000")         /* cmp esi,0x1002 */
Q1SIG("E5.active",    0x14038f03bULL, "c7835802000001000000") /* mov dword [rbx+0x258],1: the active flag */
Q1SIG("E5.sel",       0x14038f06aULL, "488b0d57ee3704")       /* mov rcx,[0x14470dec8]: the allocator selector (0: CRT malloc) */
Q1SIG("E5.hdr",       0x14038f071ULL, "83c5e8")               /* add ebp,-0x18 */
Q1SIG("E5.mul",       0x14038f074ULL, "4169fe84000000")       /* imul edi,r14d,0x84 */
Q1SIG("E5.add",       0x14038f07bULL, "03fd")                 /* add edi,ebp: size = n * 0x84 + off - 0x18 */
Q1SIG("E5.alloc",     0x14038f084ULL, "ff156ee2e902")         /* call [0x14322d2f8] (CRT malloc; its return address is caller RVA 0x38f08a) */
Q1SIG("E5.store",     0x14038f09aULL, "48898360020000")       /* mov [rbx+0x260],rax: the table */
Q1SIG("E5.grp",       0x14038f0c4ULL, "48898b48020000")       /* mov [rbx+0x248],rcx: the group table = the table */
Q1SIG("E5.evt",       0x14038f0d3ULL, "48898350020000")       /* mov [rbx+0x250],rax: the event table = the table + off - 0x18 */
Q1SIG("E5.groups",    0x14038f1a4ULL, "443b4014")             /* cmp r8d,[rax+0x14]: every pick reads the asset's group count */
Q1SIG("E5.rr_write",  0x14038f204ULL, "668954f102")           /* mov [rcx+rsi*8+2],dx: the step's round-robin cursor write into the table */
Q1SIG("E5.rand",      0x14038f216ULL, "e815fffbff")           /* call 0x14034f130 (the TT800 draw of the random pick) */
Q1SIG("E5.last",      0x14038f242ULL, "44898b5c020000")       /* mov [rbx+0x25c],r9d: the last chosen variant */
Q1SIG("E5.tick_act",  0x14038f2f4ULL, "83b95802000000")       /* cmp dword [rcx+0x258],0: the ticker's active gate */
Q1SIG("E7.p_head",      0x1403038f2ULL, "e8d9a6fdff")         /* call 0x1402ddfd0: the PLAYER list head (0x20, in the PLAYER constructor) */
Q1SIG("E7.p_push",      0x140376a83ULL, "488bb100b50200")     /* mov rsi,[rcx+0x2b500]: push_back */
Q1SIG("E7.p_clear0",    0x14034ff10ULL, "4881c100b50200")     /* add rcx,0x2b500: the step clears the list (player 0) */
Q1SIG("E7.p_clear1",    0x14034ff31ULL, "4881c100b50200")     /* add rcx,0x2b500 (player 1) */
Q1SIG("E7.node",        0x1402de227ULL, "e8a4fdffff")         /* call 0x1402ddfd0: _Buynode, list nodes of 0x20 */
Q1SIG("E7.p_ctrl_size", 0x14037c0ceULL, "b918000000")         /* mov ecx,0x18: _Ref_count<HeaderGaugeResource> */
Q1SIG("E7.p_ctrl_vt",   0x14037c0ebULL, "488d0de6d1b003")     /* lea rcx,[0x143e892d8] */
Q1SIG("E7.g_kind",      0x140379869ULL, "4183ff05")           /* cmp r15d,5 */
Q1SIG("E7.g_size",      0x140379873ULL, "418d4f0b")           /* lea ecx,[r15+0xb]: the gauge object, 5 + 0xb = 0x10 */
Q1SIG("E7.q_ctrl_size", 0x1402de153ULL, "b960000000")         /* mov ecx,0x60: _Ref_count_obj<StageWindParam> */
Q1SIG("E7.q_ctrl_vt",   0x1402de178ULL, "488d05c173ba03")     /* lea rax,[0x143e85540] */
Q1SIG("E7.q_head",      0x140103eeeULL, "e8dda01d00")         /* call 0x1402ddfd0: the static-init head (0x20) of the wind parameter list */
Q1SIG("E7.q_head_st",   0x140103efaULL, "488905dfaa6004")     /* mov [0x14470e9e0],rax */
Q1SIG("E7.q_push",      0x1402daf45ULL, "488b1d943a4304")     /* mov rbx,[0x14470e9e0]: IFC +0x58 push_back */
Q1SIG("E7.map_erase",   0x1402ddd0aULL, "488b05ff154304")     /* mov rax,[0x14470f310]: a red-black erase (an empty std::map head is 0x1a) */
Q1SIG("E8.ctl_lea",  0x1402db66cULL, "4c8d3d8d9d4304")        /* lea r15,[0x144715400]: the two player controllers (stride 0x60) */
Q1SIG("E8.ctl_call", 0x1402db685ULL, "e8166f0000")            /* call 0x1402e25a0: the controller's direct-command setup */
Q1SIG("E8.size",     0x1402e25efULL, "b968000000")            /* mov ecx,0x68 */
Q1SIG("E8.alloc",    0x1402e25f4ULL, "e8c73b1c00")            /* call FMemory::Malloc 0x1404a61c0 (its return address is caller RVA 0x2e25f9) */
Q1SIG("E8.vt",       0x1402e2635ULL, "488d0db435ba03")        /* lea rcx,[0x143e85bf0] */
Q1SIG("E8.vt_store", 0x1402e263cULL, "488908")                /* mov [rax],rcx */
Q1SIG("E8.store",    0x1402e2648ULL, "48894350")              /* mov [rbx+0x50],rax: controller + 0x50 */
Q1VT("E7", ".?AV?$_Ref_count@UHeaderGaugeResource@PLAYER@@@std@@",     0x143e892d8ULL, 0x18,  "E7.p_ctrl_size")
Q1VT("E7", ".?AV?$_Ref_count_obj@UStageWindParam@scbattle@@@std@@",    0x143e85540ULL, 0x60,  "E7.q_ctrl_size")
Q1VT("E8", ".?AVHgCpuDirectStand@@",                                   0x143e85bf0ULL, 0x68,  "E8.size")
#ifdef Q1_TABLE_UNDEF_SIG
#undef Q1SIG
#undef Q1_TABLE_UNDEF_SIG
#endif
#ifdef Q1_TABLE_UNDEF_PTR
#undef Q1PTR
#undef Q1_TABLE_UNDEF_PTR
#endif
#ifdef Q1_TABLE_UNDEF_VT
#undef Q1VT
#undef Q1_TABLE_UNDEF_VT
#endif
