# SC6 rollback: reverse-engineering notes, a stock-game bug first

Notes from building this plugin, for anyone working on SoulCalibur VI rollback. I first wrote them for the authors of SC6 Astral and
HorseMod, the other two SC6 rollback projects I know of. The useful thing is at the top: **two direction checks in the move-script VM
read one argument too many, and the extra value is a leftover stack word that changes per launch.** On one PC I've shown it decide whether a Reversal Edge dodge registers. Between two PCs it should be able to make a clash come out
differently, with or without rollback, though I haven't caught that in the act yet. The fix is four bytes.

**Conventions**
- **verified** = I can show it in the game's code or in my own in-game logs. **hypothesis** = I can't yet.
- Addresses assume the image base `0x140000000`; the exe is ASLR-enabled (DYNAMIC_BASE), so rebase at runtime. Steam build,
  SoulcaliburVI.exe sha256 `F8904E4B04BCA3B47BC52A683F6190365D2EB89EE8F44F8072759E9C5E04A553`.
- Function names follow Fotten's [SC6ModdingDocs](https://github.com/FottenSC/SC6ModdingDocs) (`move-system.md`, `symbol-index.md`) where
  they have them. I call the fighter object **P** (see 1.1).
- "the step" = the per-frame battle update, Fotten's `LuxBattle_PerFrameTick` (`0x1402DBC60`).

Part 1 is the bug and stands alone. Part 2 is a short status. Part 3 is older notes, re-checked; it's long, so treat it as an appendix.

# Part 1. Two direction checks read one argument too many

## 1.1 The bug (verified, static)

- The inner bytecode VM (Fotten's `LuxMoveVM_ExecuteBytecode`, `0x1402E5A30`) runs its `CALLCOND` opcode through the dispatch table at
  `0x143E83A90`. Entries `0x00` / `0x01` are `LuxMoveVM_EvaluateIfOpcode` (`0x1403732F0`), called as `(pVM, argc, local_68)`. `local_68` is
  the VM's stack array of 16-bit arguments: `local_68[0]` is the predicate sub-op, `local_68[1..]` are its arguments, so `argc` counts the
  sub-op word too.
- **P** is the function's first argument (rcx; Fotten's `pVM`), used as the fighter object: `rdi = rcx`, or the global at `0x144846470`
  when rcx is null. It reads the opponent from `P+0x973e8` and stores the sub-op to `P+0x1c6a` (Fotten's `LastVMOpcode`). `PLAYER` in
  Part 3 is this same object.
- Sub-op `0x24` (held direction, self) and `0x90` (held direction, opponent) loop over `local_68[1 .. argc-1]`.
- Sub-op `0x25` (newly pressed direction, self) and `0x91` (newly pressed direction, opponent) loop over `local_68[1 .. argc]`: **one slot
  past the arguments.**
- To avoid a name clash: `0x25` / `0x91` here are the first word `local_68[0]` selects inside `EvaluateIfOpcode`, not the `CALLCOND`
  opcode byte `0x25` or dispatch slot `0x25`.

The two loops side by side. `ebp` = argc on entry; after the decrement `r10 = argc - 1`; the counter `r9` starts at 0 (from `rbx`, zeroed in
the prologue) while `rsi` walks `local_68` from `[1]`, so pass `i` reads `local_68[i+1]`:

```
sub 0x24 (correct)                         sub 0x25 (one too many)
140373909  dec   ebp                       1403739bc  sub   ebp, 1
14037390b  test  ebp, ebp                  1403739bf  js    0x1403753ca      ; 0f 88
14037390d  jle   0x1403753ca               1403739c5  movsxd r10, ebp
140373913  movsxd r10, ebp                 1403739cb  add   rsi, 2
140373919  add   rsi, 2                    ...
...                                        140373a5b  cmp   r9, r10
1403739a9  cmp   r9, r10                    140373a5e  jle   0x1403739d5     ; 0f 8e
1403739ac  jl    0x140373923
```

With argc = 1, `0x24` returns 1 (an empty AND) at the entry `jle`; `0x25`'s `js` is not taken, so it runs the body once and reads
`local_68[1]`. `0x90` (`0x140373a6e`) and `0x91` (`0x140373b2d`) are the same pair for the opponent (read through `r11 = [P+0x973e8]`);
`0x91`'s `js` is at `0x140373b30` and its `jle` at `0x140373bdd`.

**What each argument tests (verified).** All arguments must pass (an AND): one failure returns 0 (`0x1403733aa`). The high nibble picks the
field and the test:

| Word | Reads | Passes when |
|---|---|---|
| `0x1xxx` | `[P+0x217c]`, the side-adjusted newly pressed direction (a numpad digit) | it equals the low 12 bits (low part 0 = no new direction, 5 = any/don't-care, `0xa` = any direction) |
| `0x2xxx` | `[P+0x2168]`, the same before the side rotation | it equals the low 12 bits |
| `0x3xxx` | `[P+0x2180]`, the side-adjusted new-press mask | `test` with the word is non-zero |
| `0x4xxx` | `[P+0x216c]`, the raw new-press direction bits (before the side rotation) | `test` with the word is non-zero |
| any other high nibble | nothing | always (ignored) |

So the extra slot can only add a failing condition; it can never make a check pass.

## 1.2 The fix (verified, static)

Change the second opcode byte of four `jcc rel32` instructions, so `0x25` / `0x91` stop exactly where `0x24` / `0x90` do:

| Address | Stock | Fixed | Effect |
|---|---|---|---|
| `0x1403739bf` | `0f 88 05 1a 00 00` (js) | `0f 8e ...` (jle) | `0x25` with no arguments returns 1, as `0x24` does |
| `0x140373a5e` | `0f 8e 71 ff ff ff` (jle) | `0f 8c ...` (jl) | `0x25` stops at `local_68[argc-1]` |
| `0x140373b30` | `0f 88 94 18 00 00` (js) | `0f 8e ...` (jle) | the same for `0x91` |
| `0x140373bdd` | `0f 8e 6d ff ff ff` (jle) | `0f 8c ...` (jl) | the same for `0x91` |

- **Writing it:** all four sites are on one `.text` page (`0x140373000`, not writable). VirtualProtect the page, four one-byte stores, the
  old protection back, FlushInstructionCache. No relocation touches these bytes, and the rel32 displacements don't change. I check all four
  6-byte signatures first and write all or nothing.
- **Cost and behavior:** no runtime cost. The game then behaves as it does on a launch whose leftover word happens to be harmless; the only
  change is that the silent non-registrations go away.
- **Completeness (verified, static):** of every `EvaluateIf` sub-op that loops over its arguments by argc, only `0x25` / `0x91` read one too
  many. The game's own authors got it right elsewhere: `0x13af` (through the second dispatch table at `0x1403768c0`) tests the same four
  new-press fields and stops at `local_68[argc-1]` (`cmp rcx, r9 ; jl` at `0x140376081`). Separately, some fixed-arity sub-ops load a fixed
  argument slot without checking argc; they'd act on a word past the arguments only if a script passed fewer than they take (hypothesis; I
  haven't seen it).
- **Another option:** zero `local_68[argc]` before each check. A zero is in an ignored class and kept my recording's outcome, but it needs a
  hook on every dispatch. The byte patch is simpler.

## 1.3 How to check it yourself

1. **Static (a few minutes):** disassemble the four cases. The jump table at `0x140376568` holds 32-bit image-relative offsets indexed by
   the sub-op (for sub-ops `0..0x9d`): `0x24` -> `0x140373909`, `0x25` -> `0x1403739bc`, `0x90` -> `0x140373a6e`, `0x91` -> `0x140373b2d`.
   Compare the loop bounds against the listing in 1.1.
2. **One PC, logging:** at the entry of `0x1403732F0`, when `local_68[0]` is `0x25` or `0x91`, log `local_68[argc]` (the u16 at
   `r8 + 2*edx` at function entry). Log P's frame counter and move id on the same line, to find the clash frames. In my traces of Reversal
   Edge clashes that slot was either 0 (an earlier call on that frame had overwritten it) or one value for the whole launch, different on
   the next launch.
3. **One PC, forced:** in the same entry hook, write a word to `local_68[argc]` before calling the original (I did this for every `0x25` /
   `0x91` call, both players), then try a direction input in a clash.
   - A side-independent pair: `0x1000` fails on any frame with a new direction press (class 1, low part 0 = "no new direction", so the test
     `[P+0x217c] == 0` fails), and `0x0000` always passes (ignored class). With `0x1000` the direction choice should not register; with
     `0x0000` it should. With the four bytes from 1.2 applied, it registers either way.
   - (My own proof in 1.5 used `0x400b` / `0x400f`, which test the raw bits in class 4. Those depend on the side, because the raw bits come
     before the side rotation, so I'd steer you to the class-1 pair above for a quick repro.)

# Part 1 continued: the evidence

## 1.4 What's in the extra slot (verified on my traces, for the frames I traced)

- On the two frames where I traced the CPU's second-stage clash decision (frames 10948 and 11511, in two launches), the slot held
  **bits 32..47 of `__security_cookie XOR rsp`**: a nested `LuxMoveVM_ExecuteBankSlotScript` (`0x1402FCC30`) had stored `cookie XOR rsp` at
  its /GS slot on the same stack address (`0x1402fcc3b..0x1402fcc45`).
- For this slot's stack position on the game thread it's **one value per launch**: the cookie is random per launch, and bits 32..47 of the
  thread's stack address don't change from call to call. My two launches gave `0xb5fb` and `0x593a`. **Both happen to be in ignored classes
  (high nibble `0xb` and `0x5`), so on those two launches the extra slot was harmless.** I have not yet caught a naturally failing word; the
  failure in 1.5 is forced.
- The address `r8 + 2*argc` moves with argc, so the leftover word is per call path, not literally one word for the whole process. A
  different sub-op can pick up a different cookie fragment.
- **Hypothesis:** a rollback re-simulation that runs on a different thread (a different stack) could read a different word than the live
  pass on the same PC. HorseMod's notes say the game thread alone runs native simulation, so it's moot there; it may matter for
  threaded designs.

## 1.5 One-PC proof (verified, in the game)

I replay a recorded real fight hands-off on one PC, with no emulated delay: me as Voldo against the CPU's Cervantes, 16,286 frames (a
different recording from the 8,698-frame one in Part 3). My plugin can fill the unused tail of the `CALLCOND` local (`local_68[argc..15]`)
with a chosen word before every dispatch, for both players. "Matches" below means my per-frame gameplay checksum equals the recording's, on
every frame of the fight (the controller block differs on frames 1-843, before the fight, by design of the replay rig).

| Run | Fill word | 4-byte fix | My 6 (forward) input in the round-3 clash, frame 6095 | The rest of the fight |
|---|---|---|---|---|
| A | `0x400f` (passes a forward press) | absent | registers, Voldo wins the exchange | matches the recording |
| B | `0x400b` (one bit less) | absent | doesn't register, Cervantes wins | splits at 6095; identical on a repeat |
| C | `0x400b` | on | registers, Voldo wins | matches the recording |
| D | `0x400b` | off | doesn't register, Cervantes wins | splits at 6095, identical to B |

- A and B were an older build of my plugin without the fix; C and D were one newer build, fix on (C) or off (D).
- `0x4xxx` tests the raw new-press bits. On that frame my forward input set the raw value `0x4` (bit 2), with the input-mirror flag
  `[P+0x16e4] = 0`. `0x400b` and `0x400f` differ only in that `0x4` bit, so **that one bit decides whether the dodge registers**, and the
  four bytes in 1.2 make the leftover word irrelevant.

## 1.6 Two PCs

- **Before the fix (the link to this bug is a hypothesis):** one of my two-PC sessions desynced in a Reversal Edge clash at about 370 ms
  round trip, with identical inputs on both. From frame 6999, part of P2's hashed gameplay state (the same state the forced word flips at
  6095 in 1.5) differed between the PCs for single frames, on each of P2's six direction presses in the clash's second stage; at 7031 the
  clash outcome split (move `0x275` on one PC, `0x27d` on the other). That fits this bug. It also fits a state-restore gap: PC 2 had re-run
  each of those frames after a misprediction and PC 1 hadn't, and I didn't log the leftover words in that session, so those two causes can't be
  told apart from that session.
- **With the fix on both PCs (verified):** three battles in a Player Match, one at about 110 ms and two at about 330 ms (PC 2 restarted its
  connection between the second and third).
  - All 355 cross-PC checks agreed on both PCs. Each check compares a hash chained over the gameplay state of every rollback-active frame
    outside the round-start, KO and replay stretches (about three quarters of the frames), once every 60 frames.
  - There were 14 Reversal Edge clashes, 5 reaching the second stage, all outside those stretches. Both PCs' records show the same moves on
    the same frames for every one.
  - The four bytes applied at the agreed frame and were put back at each battle's end, cleanly, on both PCs.
- **What this does not show:** how often these two PCs would split without the fix (I haven't measured it). The one-PC runs in 1.5 are the
  proof that the bytes decide the outcome; this session shows the fix applies and restores cleanly online and that 14 clashes stayed in sync
  with it on.

## 1.7 What it can do in a fight, and how often

- **Verified in the code:** if the extra word's high nibble is 1 to 4 and its test fails, the whole `0x25` / `0x91` check returns 0.
- **Verified in the game, one case:** with `0x400b` forced, my forward dodge in a clash's first stage didn't register (1.5).
- **Hypothesis:** other clash direction choices that go through `0x25` / `0x91` drop the same way. My traces show the CPU's second-stage
  checks reading the slot, but I haven't forced a failure there.
- **Hypothesis, the rate:** if the cookie's class nibble (bits 44..47) is uniform, about 3 in 16 launches get a word that fails a forward
  press (classes 1 and 2 almost always fail, classes 3 and 4 half the time: `(1 + 1 + 1/2 + 1/2) / 16`). That uniformity isn't shown; my
  two launches' cookies even share bits 32..39 (`0x4e`), which would be a coincidence under uniform bits.
- **Hypothesis, online:** each PC reads its own word, so one PC can register a direction choice and the other not, at any ping, with or
  without rollback. With the rate above, a pair of launches would disagree on a forward choice about `2p(1-p)` ≈ 3 in 10 of the time,
  whenever its check reads the word. Fotten's notes describe the native netcode as delay / catch-up based with each PC simulating on its own, so
  I'd expect it to be exposed too, but I haven't tested two unmodded copies.
- If you've ever seen a Reversal Edge clash end differently on two PCs, this might be worth ruling out early. I lost a while suspecting my
  own state restore and my lip-sync handling first.

# Part 2. Where I'm at

- **The plugin:** rollback inside the game's own online matches. It detects a modded opponent over a Steam P2P side channel (channel
  `0x52424F4E`, picked so it shouldn't clash with other mods' side channels; Fotten's rollback notes mention HorseMod's `0x484F`). Against an
  unmodded opponent it leaves the four fix bytes (1.2) stock, so the game's own code runs there unchanged, and its battle-step hooks stay
  transparent.
- **Two PCs:** since the two copies first turned rollback on together, four Player Match sessions with a second tester, 14 battles in all,
  about 55 to 650 ms round trip. Both copies turned on at the same frame in every one. One battle desynced, the clash in
  1.6, before the fix. (Two still earlier sessions never turned on, which is how I found the hitbox-hash trap in Part 3.)
- **Recent changes:** rollback starts at round 1; the round-start / KO / replay stretches run at the game's own input delay instead of a
  fixed 2 frames; a time sync that slows whichever PC runs ahead; the input delay is set per battle from the measured ping (2, 3 or 4
  frames) with up to 7 frames of rollback on top; an on-screen status indicator; and effect calls a rollback hid on two listener slots are
  re-sent from the confirmed pass (Part 3, 3.6).
- **Still open:** other listener slots still miss calls during rollbacks, and any slot can still get a call from a misprediction. At about
  330 ms the other player saw a Reversal Edge startup spark stay on a weapon for a whole round (I think it's one of those slots, hypothesis); the
  game still waits on the other PC on about 1 frame in 10 at that ping; audio stutters there.

# Part 3. Older notes (re-checked)

These are from an earlier draft, re-verified against the current build. Some of it is already in Fotten's SC6ModdingDocs; I've tried to
say so and point at the part that might be new.

## 3.1 Testing rollback without a second player (verified)

1. Record a real match: inputs plus a per-frame gameplay checksum.
2. Replay it in the real game on one PC while a fixed-schedule network emulator delays one side's inputs, so the plugin has to predict,
   roll back and resimulate.
3. Compare the checksum every frame against a zero-delay replay.

On an 8,698-frame recording: 100 ms, 456 rollbacks, matched on every frame; 150 ms with every input 6 frames late (455 of 456 rollbacks 6
deep), matched on every frame; lip-sync on at 100 ms, matched on every frame. The same rig (with no emulated delay, on the longer recording
in 1.5), plus the fill hook, is how I checked the bug in Part 1.

## 3.2 The step reads leftover register values (verified)

- `FUN_1402edb90` fills per-bone weapon-hold flags in a loop that can exit early. From bone 25 on the flags are never written but are still
  read, and those stack bytes hold a spilled copy of the **caller's `xmm9`**, so WEAPON3 / WEAPON4 attachment decisions depend on who called
  the step: the game's own call, or a resimulation from a hook.
- Without a fixed call context (the registers and stack below the call), about 19% of frames mismatched (392 of 2,084) at 7-frame-deep
  rollbacks, in a pattern that followed the walk cycle. With a fixed context, 0 of 7,086 in Training and 0 of 5,580 over three Versus rounds.
- **Fix:** give every step pass, live and resim, on both PCs, the same call context: the same vector and integer registers, a pre-cleared
  stack region below the call, and a fixed MXCSR / x87 setting. (Fotten's rollback notes already pin MXCSR / x87 on both sides; the part
  that might be new is the `xmm9` spill.)
- **The game's own values (verified):** the native netcode's step-entry context was constant over 47,577 ticks in 5 online battles:
  `xmm7 = xmm9 = 0x47a8c000` (86400.0f), `xmm8 = 0x7fffffff`; `xmm10`-`xmm12`, `xmm14`, `xmm15` and every upper half 0; `xmm6 = xmm13` the
  frame delta (`0x3c888888`, 1/60, on over 99.9% of ticks); x87 control word `0x027f`, MXCSR control `0x1f80`. Two different PCs produced
  identical values. **Hypothesis:** using those values rather than zeros keeps native-netcode frames and rollback frames in agreement,
  which matters if rollback switches on mid-match; the stack-contents part of that is untested.

## 3.3 The camera is gameplay state (camera save: verified; the CE/SC link: hypothesis)

- At the start of every step, `FUN_140312510` (called from the step at `0x1402dbf41`) picks the dominant camera layer from its layer list,
  takes the sign of a cross product with the camera eye (`FUN_14038f600`), and sets the screen-side flag `PLAYER+0x124`. The input mirror
  `PLAYER+0x16e4` follows from it (the same flag the raw-bit test in 1.1 depends on). It's a threshold, so a wrong dominant layer flips it.
- The camera-function block is `0x41e0` bytes at `[0x14470ee90+8]`. This matches the timer-node writer/reader in Fotten's notes (`0x140324900` /
  `0x1403249E0`): the `0x2f0` node, the `0x41e0` backing block, the trailing globals. The part that might be new: `H_CameraFuncThrow` keeps
  its timer in the head of its slot but its state machine in the tail, so restoring only the slot heads (which I did first) ran the throw
  camera shot early. Restoring the whole `0x41e0` block plus the dword `0x1440f4a84` fixed it, in game, for throws and Critical Edges.
- SC6 Astral's README mentions CE / SC flipping the camera 180 degrees and swapping sides. In my build a partial camera restore gave a
  similar-looking symptom, so it might be worth a look. Since the side flag also drives the input mirror, a wrong side there could change
  inputs too, not just the view (hypothesis).

## 3.4 Picking a voice line consumes gameplay RNG (mechanism verified; that it alone caused my drift is a hypothesis)

- Each player has a voice table (`vtb`), a heap block allocated while the battle loads (10,620 and 10,884 bytes for Ivy and Cervantes in my
  test). The step's voice pick writes a round-robin cursor into it (`0x14038f204`), and the last chosen variant (`[PLAYER+0x25c]`) decides
  how many draws from `LuxMoveVM_GetRandU32` (`0x14034F130`) the pick's retry loop consumes (`0x14038f216`).
- So if these tables aren't restored on rollback, the battle RNG can drift. In game, a 100 ms test without them drifted from frame 1582,
  with two of the game's RNG states differing first; with them restored (and the blocks in 3.5), it matched on all 8,698 frames.

## 3.5 Heap blocks the step writes that a battle-start snapshot misses (verified)

- One test settled it: snapshotting every heap block my plugin had tracked (1,521 blocks, 331,355 bytes) removed the drift completely, so
  the gap was coverage, not timing. The step-written blocks I've had to cover:
  - the wind emitter (it draws RNG when it fires a gust), the RingOut / Parallel wind objects, and gusts already spawned (stage-dependent);
  - the 29 key-record vectors in the battle WorkMemory;
  - the voice tables (3.4);
  - a `std::map` off `PLAYER+0x3590`, and the `std::list` at `PLAYER+0x2b500` (the step pushes onto it and clears it). I also run a check
    that refuses rollback if any other `std::list` / `std::map` in the battle globals / WorkMemory changes and isn't covered.
- **How I found them:** watch which heap blocks the step writes during a live pass, then check each is in the snapshot. That's how the voice
  tables turned up.

## 3.6 Listener notifications: the engine writes fight state around them (verified)

- Inside notifications, UE re-enters the battle core (IBattleCore `+0x008` / `+0x010` / `+0x0e0` / `+0x100` / `+0x110` / `+0x118`) and
  writes core memory. For example, the `+0x100` call at `0x140308433` returns a float in its packet, which the core stores to `PLAYER+0x2c`
  at `0x14030844b`.
- **What worked:** answer queries the same way on every pass, and undo UE's writes into core state after each forwarded call. Keep only
  presentation fields, such as lip-sync.
- **The state class (slots `+0x058` / `+0x0c0`):** `+0x0c0` fires at battle start, in the round-start / replay stretches, and mid-round for
  the KO slow motion; in the KO call UE frees roughly 1,900 to 3,250 blocks. I mute that class in predicted passes and deliver it from the confirmed
  pass, followed by a rollback to that frame. That matched on every frame.
- **Effects (slots `+0x120` and `+0x128`):** `+0x120` is a per-player value channel (player, index 0-7, float); `+0x128` is a positioned
  one-shot. Both hand UE a copy of their payload by value, so a call a rollback hid can be made again later from the confirmed pass. I do
  that now, and in my two-PC logs (three battles, both PCs) those two slots no longer miss a call; a call from a misprediction can still
  reach them, and on `+0x120` the confirmed pass sets the value again. What UE draws from them isn't decoded (hypothesis: the red Critical
  Edge / Reversal Edge glows).
- This may overlap with the "presentation object lifetime and thread affinity" item in Fotten's rollback notes. The by-value
  payloads might help a confirmed-commit step; happy to share which listener calls free memory or re-enter the core, by slot.

## 3.7 A trap if you checksum hitbox data (cause verified statically)

- The KHit lists live in a static pool in the image (`0x14485ebd0` / `0x144866bd0` + idx × 0x4000).
- A **KHitSphere node is only 0x80 bytes**, while Area and FixArea nodes are 0xa0. Every node starts with an absolute vtable, and each list
  is followed by an array of absolute node addresses.
- I hashed 0x1c bytes at `+0x7c` of every node. For a sphere that runs past the node into the next node's absolute vtable, or into that
  pointer array, and the game loads at a different address every launch, so two PCs practically never matched.
- **Fix:** hash only the fields written once at load, per class: Sphere center `+0x40..+0x48`, radius `+0x74`, `+0x78`, bone `+0x7c`; Area
  points `+0x30..+0x48`, bones `+0x90` / `+0x94`; FixArea points `+0x30` / `+0x40` / `+0x50`, bone `+0x90`; every class `+0x08`, `+0x10`,
  `+0x16`, `+0x17`. Never the world buffers, the `+0x14` per-state flag, or addresses. Keep list order.

## 3.8 Finding the opponent's SteamID (verified static + live)

- `session+0xd0` is not a SteamID in any match. The opponent's address object sits inline at `session+0xc8`, and its id is a CommSys peer
  handle drawn from CRT `rand()`.
- The real SteamID: the CommSystem static `0x1443ff350` has a vector at `[+0x260, +0x268)` of `{peer, ctrl}` entries; the peer whose address
  object at `+0x2708` holds that handle at `+0x2710` has a `FInternetAddrSteam` (vt `0x143ba78c0`) at `+0x26f8`; the SteamID is at `+0x20`
  there. I accept it only if it's an individual SteamID, isn't my own, and `GetP2PSessionState` shows a session with it that's active or
  connecting.

## 3.9 Faces under rollback (fix verified in game; why it works is a hypothesis)

- From live passes, keep only the lip-sync state (the controller, voice state, face weights and layers), never the world-space face bone
  rows. An early build of mine kept the world rows and faces visibly stretched off the head for a few frames after a rollback; keeping only
  the lip-sync state fixed it. **Hypothesis:** the face rig rebuilds those rows every step from the restored head.

## 3.10 Smaller things (verified)

- The step's 0x18-byte third input block: offline it holds P1's input for the next frame; online it's the local player's own undelayed pad,
  and its analog tail is local-only, so keep it out of cross-PC hashes. Predicting it brought my rollbacks down to 892 in one test; my model
  of the old predictor gives about 8,935, so roughly 90% fewer (that comparison is modeled, not measured).
- To check that both players run the same gameplay content without listing mods, compare the move-record multimap at `PLAYER+0x455d0` (size
  at `+0x455d8`) and its keys. A content mod changed Ivy's tree from 1421 to 556 entries between launches; cosmetic mods didn't.

Happy to compare notes or go deeper on any of this, especially Part 1: open an issue on this repository.

— energy.jp
