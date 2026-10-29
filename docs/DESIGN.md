# How sc6rb works

This is the design of the plugin, written so it can be used without asking: how SoulCalibur VI's own online netcode works, how the
plugin stays invisible to a player without it, how two copies pair, what changes once rollback is on, what a snapshot holds, how the
battle core's calls into Unreal are handled, how desyncs are caught, and what is still unsolved. [NOTES.md](NOTES.md) has the
individual findings (the stock-game bug first); this file is the frame they fit into.

**Conventions.** Same as NOTES.md: **verified** = shown in the game's code or in the plugin's own logs; **hypothesis** = not shown.
Addresses assume the image base `0x140000000` (the exe is ASLR-enabled; rebase at runtime), for `SoulcaliburVI.exe` sha256
`F8904E4B04BCA3B47BC52A683F6190365D2EB89EE8F44F8072759E9C5E04A553`. "The step" is the per-frame battle update (`LuxBattle_PerFrameTick`,
`0x1402DBC60`, in Fotten's [SC6ModdingDocs](https://github.com/FottenSC/SC6ModdingDocs)). The source files named below are in `src/`.

## 1. SC6's own online netcode (verified unless marked)

Rollback runs inside the game's own online battle, so it has to fit the game's input synchronization exactly.

### 1.1 The input-sync object

An object with vtable `0x143283578`, 0x44d0 bytes, at `mgr+0x478`, where `mgr` is the battle manager (its Tick, `0x1403fbf30`, drives
everything in this section; the battle core's interface, vtable `0x14327b610`, is at `mgr+0x1450`). Every address and offset the plugin
uses is named in `src/common.h`, `src/nobs.h` and `src/rbo.h`. The fields the plugin reads or writes:

| Field | Meaning |
|---|---|
| `+0x3a0` | GameRound |
| `+0x3a4` / `+0x3a8` / `+0x3ac` | GameTime / PauseTime / UpdateTime |
| `+0x440c` / `+0x4410` | the current InputLag / SendTime |
| `+0x3c0 + player * 0x2000` | the input ring of each player: 512 entries of 16 bytes, indexed by `frame & 0x1ff`, tagged with (round, frame) |
| `+0x43e4` / `+0x43e8` | InputLagMin / InputLagMax (the game's own range is 2..15; stock online ran 4 to 5) |
| `+0x4400` | RemotePlayerFlags (nonzero in a battle against a remote player; spectators fail it) |
| `+0x4404` | bOnline |
| `+0x4405` | bOnlineSync: 0 to 1 at tick 2 of the battle, 1 to 0 at the final GAMESET |
| `+0x4408` | the round trip time, a float in ms |
| `+0x4414` | SyncTime (how far the remote input is confirmed) |
| `+0x4424` | the sync state; `+0x4425` the result IsPaused reads |

An online battle the plugin may arm in has: that vtable, `+0x4404 = 1`, `+0x4400 != 0`, `+0x4405 = 1` and state 2.

Stock moves the current InputLag by 1 per tick toward `clamp(ceil(RTT_ms * 0.03) + 2, InputLagMin, InputLagMax)` (`FUN_1403f8390`),
which gave 4 to 5 frames at 50 to 80 ms.

### 1.2 One tick

`UpdateTime` (`0x1403fdb80`) runs, in order: RTT, InputLag, ReceiveInputs, capture the pads, the sender, StreamAppend, the sync state
machine (`0x1403fdfa0`, called through vtable `+0x698` at `0x1403fdc61`), flush. IsPaused (`0x1403e9ee0`) runs after it and is the only reader of `+0x4425`: it returns
`PauseTime != 0 || result == 1`, then zeroes the result. That makes every stall decision **one tick late** (the frame IsPaused lets
through was decided on the previous tick).

### 1.3 States and results

- **States:** 0 and 1 are the per-round barrier (state 1 sends control code 0 every tick and has no timeout); 2 is running; 5 is the
  post-match ending, which each PC steps **alone, with the remote input 0** (SyncTime stops, GameTime keeps going).
- **Results:** 1 = barrier (immediate pause); 2 = stall (`GameTime >= SyncTime - 1`); 3 = a one-tick local catch-up; 4 = 120 ticks
  since SyncTime moved, which is only a pause with no consumer; 5 = stepping alone.
- The pause menu never stops the online simulation.

### 1.4 The wire

- One channel, **4** (sent through the session's vtbl `+0x20`). Header bytes `[player << 4 | round & 0xF,
  (UpdateTime & 0xF) << 4 | type]`, little-endian, no byte swap.
- Only two packet types exist: **type 0** (3 bytes, control codes; code 0 is the round barrier) and **type 1** (inputs: u32 start frame,
  u32 ack, then u16 input words; 10 to 28 bytes, so 0 to 9 input words). Each input frame is sent about **9 times** (a resend window).
- Measured on live battles at 60 to 80 ms round trip: no input frame was ever missing; the first copy arrived 1 to 5 ticks before it was
  needed; the remote input's lead averaged 1.4 to 1.6 frames, max 4 to 9.
- **The stock receive parser trusts its indexes.** In a type 1 packet a player nibble of 2 or more writes past the object; a type 0
  code 1 with a matching round calls `SetOnlineState(4)`, which is irreversible and otherwise unreachable. Packets of type 2 or more are
  dropped after reading their 2 header bytes. **Never send a stock peer a type 0 or 1 packet** (the plugin sends nothing on channel 4).
- Hypothesis: whether channel 4 is reliable at the transport level was not traced; in practice nothing was lost.

### 1.5 The step word (FetchInput, `0x1403fcd10`)

Called per player per sub-step. For player `p` on frame `f`:

```
raw   = the ring entry of p for (GameRound, f), or 0 if the tag doesn't match
held  = raw
press = (prev ^ raw) & raw                      ; prev = the previous fetch's raw (mgr+0x1498[p]); it carries across rounds
word  = held | press << 32, then both halves & ~[mgr+0x12f8], and & 0xffffc3f0 when FUN_1403f2220(mgr, p)
```

In the real game `mgr+0x1498`, `+0x14a8` and `+0x14b8` hold pointers to small per-player arrays: the previous raw value (u32), the current
step word and the previous step word (u64). The phase is `mgr+0x1480` (by its use: 1 a round's start, 2 the fight, 3 the game set, 5 the
win; the names are a hypothesis). `FUN_1403f2220` is true only in phase 3 (`mgr+0x1480 == 3`), with a local option at `LuxOptionSaveObject+0x125` set, for a player
that isn't local: so in phase 3 each PC masks only the remote player's `0x3c0f` bits. This rule reproduced every step word of a human
opponent on 95,154 live frames. Handing input back to the game with a wrong `prev` causes phantom or lost presses (a model put the
phantom-press rate at 43% of frames), so any hand-back must repair `0x1498 / 0x14a8 / 0x14b8` for the remote player.

### 1.6 The third input block

`mgr+0x14c8` (0x18 bytes) is filled once per tick before the step: the **local player's own undelayed pad** plus an analog tail. Online each
PC holds its own player's pad there, so the two PCs' blocks differ whenever the players' inputs differ (the analog tail carried data in 3
of 5 live battles). Keep it out of every cross-PC comparison;
capture it on the live pass and reuse it in re-simulations.

### 1.7 Rounds

- The per-round reset (`0x1403f7d20`) sets GameRound, zeroes GameTime / PauseTime / UpdateTime / SendTime / SyncTime, invalidates
  both rings and enters state 0, so every round starts with a barrier. The intro runs as round 9; the fight rounds are numbered from 0.
- Every irreversible match-flow step runs one path: the core fires listener `+0x058`, UE's handler pushes the mode byte, and a function
  after each step acts on each change of `mgr+0x1480` (leaving 2: the hangup check; entering 1: the round reset; entering 5:
  `SetOnlineState(5)`). Stock sends nothing at a round's end; the only per-round wire event is the next barrier.
- Hypothesis: the match type (Ranked, Casual, Player Match) is not readable without calling game code; the plugin doesn't try.

## 2. Staying invisible to a player without the plugin (verified)

**Why it matters.** The battle core reads uninitialized data that comes from its caller's registers and stack (NOTES.md 3.2: a spill
of the step caller's `xmm9` decides weapon hold flags). `Tick` (`0x1403fbf30`) saves only `xmm6`, and `UpdateBattle` contains no
vector, MXCSR or x87-control instruction before its call to the step. So any hook in the step's call path that leaves a register
different from what the game would have had can change the battle, against a player who doesn't have the plugin.

**Where the hooks are** (`rbo_install.c`): 9 vtable slots are rewritten (the manager's Tick; the battle core interface's init, shutdown
and step; the input-sync object's SendControl, sync state machine, IsPaused, SetOnlineState and round reset), and 2 rel32 calls are
retargeted (the receive enqueue at `0x1403eed2f`; FetchInput's call at `0x1403fe6e1`, to a jump through a cell in the plugin's memory
that points at the original unless the battle is armed). Each signature is checked first; any mismatch leaves the plugin off.

**How.** Each game-thread hook (all but the FetchInput jump) is a naked pre-thunk (`nobs_thunks.S`, `rbo_thunks.S`) that:
1. takes a per-hook lock with `lock bts` on a rip-relative dword, touching no register (a contended entry jumps to the original with
   everything intact);
2. saves `rax` and the game's `rsp`, and switches to a private, fully committed 256 KiB stack, so nothing is written below the game's
   `rsp`;
3. saves every other general register and RFLAGS, and the full extended state with `XSAVE64` (mask = XCR0 minus the AMX tile state);
4. calls a short C body (pre-hook only) on the private stack;
5. `XRSTOR64`s, restores every register and `rsp`, releases the lock and jumps to the original.

Status flags are not preserved (`bts` sets CF); none of the hooked entry points reads a status flag before writing it, and DF is untouched.

**Proof.** A test harness sets every general, vector and mask register, MXCSR, the x87 control word and the bytes below `rsp` to
distinct values, calls each hooked function through the thunk and directly, and compares what the original sees: byte-identical for
every hook (16 GPRs, DF, MXCSR, x87 CW, `xmm0-15`, `ymm` and `zmm` upper halves, `zmm16-31`, `k0-7`, 4 KiB below `rsp`). Seven planted
mistakes (skipping `XRSTOR64`, pushing on the game stack, `vzeroupper`, dirtying `xmm9`, changing `rax` or `r10`, leaving DF set) are
each caught. Live, an earlier observe-only build with these same thunks compared the register context handed to the battle core on
**47,577 ticks of 5 online battles against an unmodded opponent: 0 mismatches**.

**Passive means:** zero writes into game memory beyond the hook patches made once at startup (and, after the first armed battle, the
allocator doors of section 5, which then point at the originals), zero calls into game code from the hooks, nothing on the game's channel. The only
traffic is a short probe on the plugin's own Steam P2P channel (section 3), which an unmodded game never reads.

## 3. Pairing

- **Channel:** Steam P2P (`ISteamNetworking`: `SendP2PPacket` / `ReadP2PPacket`, from the game's own `steam_api64.dll`, SDK 1.39) on
  channel `0x52424F4E`, from the plugin's own thread. HorseMod uses `0x484F` (per Fotten's notes). How the plugin finds the opponent's
  SteamID: NOTES.md 3.8.
- **Messages** (`rbomsg.h`): fixed little-endian structs with a magic, a protocol number and a CRC; anything malformed is dropped and
  counted.
  - Sequenced (resent until acknowledged, delivered once): HELLO, HELLO_ACK, ARM, ARM_ACK, CHECK, DESYNC, DRAIN. BYE is sent 3 times
    and never waited for.
  - Best effort (never resent): ACK (the transport's acknowledgements), BEACON (each PC's state, once a second while paired, armed or
    drained, so a one-sided arm is noticed) and TSYNC (section 4.4).
  - At most 10 probes go to a peer that hasn't answered.
- **HELLO compares:** the protocol number, the exe's PE timestamp and SizeOfImage, a gameplay content digest (the move-record keys and the
  authored hitbox fields of both players; never an absolute address, see NOTES.md 3.7), the step-entry register context both PCs
  measured (both must also equal the built-in values in 4.5), the settings that must match (input delay, rollback depth, CHECK interval, adaptive delay), the ranked flag and the
  EvaluateIf fix flag. A difference refuses to pair and the battle stays stock. The plugin version and the mod list are never compared,
  so cosmetic mods never block a match.
- **States:** PASSIVE, PROBE (from `+0x4405` 0 to 1), PAIRED (both HELLOs agree), ARMED, DRAINED, END.
- **Arming:** both PCs agree on ARM(round r) at least 2 seconds before round r's reset, and both switch at round r's barrier release
  (state 1 to 2), when GameTime, SendTime and SyncTime have just been zeroed, so both switch at the same frame. Normally r is the battle's
  first round. The match start is never held.
- **The input delay** is agreed at pairing from the two measured round trips:
  `d = max(rbonline_d, clamp(ceil(maxRTT_ms * 0.03) + 1 - max_rb, 2, 4))`, so with `rbonline_d = 2` and `max_rb = 7`: 2 frames up to
  266 ms, 3 up to 300 ms, 4 above.
  The rollback window is `d + max_rb` frames of one-way latency.

## 4. Once rollback is on

### 4.1 What the hooks do

| Hook | Passive | Armed |
|---|---|---|
| the battle core interface's step (`+0x68`) | transparent | runs the step through the rollback core |
| IsPaused (`0x1403e9ee0`) | transparent | lifts a result-2 stall within the rollback depth |
| FetchInput (`0x1403fcd10`) | transparent | supplies the confirmed or predicted step word (the rule in 1.5) |
| the round reset (`0x1403f7d20`) | transparent | asserts nothing is unconfirmed |

### 4.2 Inputs and prediction

- The **confirmed remote input is the game's own ring**, filled by the game's own ReceiveInputs. The plugin sends no input packets.
- The **prediction** for a remote frame not yet in the ring is the last confirmed remote raw value; the step word is then built with the
  rule in 1.5, so every reader of the step words sees the same word.
- At each step the plugin scans the ring for remote frames that became confirmed since the last step. The earliest one whose raw value
  differs from what was predicted is the rollback target: restore that frame's snapshot and re-simulate up to the present.

### 4.3 The stall lift

At IsPaused, while armed: if the result is 2 and `GameTime - (SyncTime - 1) < max_rb` (checked against the frame that will actually run,
because of the one-tick lag), write `+0x4425 = 0` so the frame runs on a prediction. Results 1, 3, 4 and 5 are never touched. When the
remote input is later than the window, the lift stops and the game waits exactly as stock does (the badge's "at limit").

### 4.4 Input delay, zones and time sync

- `InputLagMin/Max` are set to `d` on fight frames only. **Zones** run with no prediction and no stall lift, at the game's own input
  delay (restored 13 frames before each zone): the intro, each round's barrier (states 0 and 1), the game's short replays in full, the KO
  window, and state 5 (the end). Pinning the delay to 2 in the zones made them wait on most frames at a 3 to 5 frame round trip.
- **How zones are found** (`rb.c`, on confirmed frames in order, so both PCs decide the same frames with no message): a zone starts 21
  frames after the first confirmed frame whose mode byte isn't the fight mode (2), whose replay flag is set, or that made a `+0x058`
  listener call; it ends 30 frames after the first confirmed frame that is back in the fight mode with no replay and no `+0x058` call in
  the 7 frames before it. The 13-frame lead is 21 - 7 (max_rb) - 1.
- **Time sync:** both PCs report their frame advantage (TSYNC); the PC that runs ahead waits single frames, so neither keeps rolling back
  deep while the other never does.

### 4.5 The register context of every pass

Every pass the plugin runs, live and re-simulated, on both PCs, gets the context the game itself hands the step, measured on 47,577
live ticks and identical on two PCs: `xmm7 = xmm9 = 0x47a8c000` (86400.0f), `xmm8 = 0x7fffffff`, `xmm6 = xmm13 = 0x3c888888` (1/60),
every other vector register and upper half 0, x87 control word `0x027f`, MXCSR control `0x1f80`, plus a pre-zeroed 256 KiB of stack
below the call. Once a battle arms, it keeps this context to the battle's end, even after a drain.

### 4.6 Draining

On a fault, a desync or a lost peer: stop predicting and lifting stalls; let the game's own stall rule run out the unconfirmed frames and
apply the last correction; repair `0x1498 / 0x14a8 / 0x14b8` for the remote player; restore `InputLagMin/Max`; keep the context.
FetchInput stays replaced in a confirmed-only form, which equals stock on confirmed frames. A frame that ran on a prediction after the
core stopped is verified when its input arrives, and a difference is reported as a desync.

### 4.7 Cost (verified, one battle at about 330 ms)

76 seven-frame rollbacks on one PC (78 on the other). A step with a seven-frame rollback (the restore, 7 re-simulated frames and the
live frame) took 2.7 ms typically and 4.2 ms at worst on one PC, and 5.0 ms and 8.2 ms on the other, inside a 16.7 ms frame. Snapshots
are kept for the last 10 frames.

## 5. What a snapshot holds (verified)

- **Fixed regions:** the core's `.bss` block (`0x14470d000..0x14485f000`), the `.data` singletons (`0x144100a40..0x144100ee0`) and the
  `std::mt19937` tail after them (`0x144100ee0`, 0x1350 bytes), the battle WorkMemory (pointer at `0x144843f00`, 0x19a710 bytes), and the
  camera-function block whole (`0x41e0` bytes at `[0x14470ee90+8]` plus the dword `0x1440f4a84`; NOTES.md 3.3).
- **The arena.** The game's allocator entry points (GMalloc's Malloc / Realloc / Free / GetAllocationSize slots and the CRT `malloc` /
  `free` / `realloc` / `calloc` imports) are hooked. Allocations made on the step's thread while a step runs (but not inside UE's
  listener code) go into the plugin's own arena (`arena.c`), whose free lists and block headers live inside the arena itself; frees of
  blocks allocated before the battle are deferred. A snapshot copies `[base, base + used)`, which captures every block the step
  allocated and the allocator's own state; fresh blocks are zero-filled, so a re-simulation never sees bytes from an abandoned
  timeline. The hooks are installed once, at the first arm, as jump doors whose target is a cell in the plugin's memory: outside armed
  battles the cell points at the original, so the game's own code runs with its own context. While UE still holds arena blocks after
  an armed battle, a free, realloc or size query for a pointer inside the arena still goes to the plugin (only it can handle those
  blocks); every other pointer goes to the original.
- **Pins:** heap blocks allocated before the battle (at load) that the step writes. They are found by a typed walk from known roots when
  the battle arms (`rbo_q1.inc`, `rbo_q1_table.h`), plus a census of every `std::list` / `std::map` in the battle globals and WorkMemory
  that refuses to arm if one isn't covered. NOTES.md 3.5 lists the ones that mattered (the voice tables, the wind objects, the key-record
  vectors, a map and a list off the player).
- **Masks:** fields copied and restored but kept out of the hashes.
- **Presentation kept from live passes:** the lip-sync state (NOTES.md 3.9).

## 6. The battle core's calls into Unreal (verified unless marked)

The core talks to UE through a 41-slot listener interface (`scbattle::EventListenerBase`, RTTI vtable `0x143e853c8`). UE's
implementation is the object whose pointer the core keeps at `0x14470d188` (vtable `0x14337c4f8`). A rollback runs some frames more than
once, so each slot needs a rule. **The mechanism:** for every pass the plugin runs, the pointer at `0x14470d188` is swapped for a proxy
listener (`fwd.c`, `fwd_thunks.S`) whose per-slot thunks forward to UE's object or mute the call, and UE's pointer is put back after the
pass (`modes.c`, `run_step`).

| Slots | Rule |
|---|---|
| `+0x138`, `+0x140` (the two queries) | answered 1 and 0 by the plugin on every pass, live included; UE never sees them |
| `+0x058` (match flow) and `+0x0c0` (battle start, lockstep stretches, the KO slow motion) | muted in predicted passes, delivered once from the confirmed pass, then the core rolls back to that frame. Inside the KO call UE frees roughly 1,900 to 3,250 blocks |
| `+0x120` (a per-player value channel: player, index 0-7, float) and `+0x128` (a positioned one-shot) | forwarded by the live pass; a call the confirmed pass makes that no live pass made is sent once, late, from the confirmed pass. For `+0x120` it isn't sent if UE already holds a newer value for that key, and a value set only by a misprediction is set back to the confirmed value. For `+0x128` the same effect at another position isn't re-sent, and per frame at most (confirmed calls minus delivered calls) go out, so an effect never shows twice |
| `+0x008` (effect start) with its stop `+0x010`, `+0x028` (sound) with its stop `+0x030` (0.35), `+0x038` (voice line) with its stop `+0x040` (0.36); on when `rb_pres_slots` names them, as the shipped `sc6rb.ini` does since 0.36.1 (without the key, only `+0x120` / `+0x128`) | forwarded by the live pass; a start the confirmed pass made that UE never got is sent once, late, with a 0x48-byte payload copy and the confirmed r8 / r9, under the never-twice rules above plus a 3-frame near window. The stop slots are watched, not re-sent on their own: a late start is dropped if UE already got its stop. For sounds, a mirror of UE's newest-copy-only sound map decides which missed stops go out late, and the plugin calls UE's `+0x030` itself inside a live frame before a restart that would orphan one of its late copies. A late voice is dropped if UE already got a newer voice or a `+0x040` of that player. NOTES.md Part 4 |
| every other slot | forwarded by the frame's live pass (predicted or not), muted in every re-simulation, never re-sent |

**The guard around every forwarded call** (`modes.c`, the L4 guard): copy the core's `.bss` before the call, undo every write UE made
to it after the call, keep the listener pointer, check the frees made during the call, and log any re-entry into the core. UE re-enters
the core from inside some calls (IBattleCore `+0x008` / `+0x010` / `+0x0e0` / `+0x100` / `+0x110` / `+0x118`), and some calls return
data through their payload (NOTES.md 3.6). With the guard, a presentation call can't change the fight, late or not.

**Still open: the other slots miss calls.** Per battle, on both PCs, three battles (calls the confirmed frame made that no live pass
forwarded / calls forwarded from a misprediction):

| Slot | ~110 ms round trip | ~330 ms round trip |
|---|---|---|
| `+0x010` | 48 to 145 missed / 1 to 7 orphaned (of 758 to 1,382) | 48 to 80 / 11 to 17 (of 752) |
| `+0x0a0` | 38 to 117 / 1 to 6 (of 443 to 754) | 38 to 70 / 11 to 16 (of 427) |
| `+0x028` | 4 to 34 / 3 to 17 | 35 to 36 / 16 to 18 |
| `+0x0c8` | 7 to 36 / 2 to 13 | 23 to 30 / 10 to 16 |
| `+0x030` | 4 to 42 / 0 to 4 | 14 to 22 / 0 to 2 |
| `+0x120`, `+0x128` | 0 missed | 0 missed |

Smaller counts on listener `+0x008` (0 to 23 missed at ~110 ms, 10 to 18 at ~330 ms), `+0x0f8` (3 to 16 / 7 to 10) and `+0x090` (0 to 2 /
10 to 12, with 7 orphaned at ~330 ms), and a few on `+0x098`, `+0x0b0`, `+0x0b8`.

These counts are from 0.34, before 0.35 / 0.36. Since then (verified on one PC, NOTES.md Part 4): a Reversal Edge's white startup spark is
`+0x008` (effect ids `0x080f` / `0x0810`, stopped by `+0x010`), its startup sound is `+0x028`, and its voice line is `+0x038`. At 150 ms all
three were missed starts, and since 0.35 / 0.36 they're re-sent when `rb_pres_slots` names them (the shipped `sc6rb.ini` does). Hypothesis: the spark that once stayed on a weapon for
a whole round at ~330 ms was a missed `+0x010` stop; `+0x0c8` drives sounds too. Re-sending any other slot needs its payload layout checked
first, and listener `+0x008` / `+0x028` / `+0x0c8` re-enter the core, so a late call runs inside the same guard as a live one.

## 7. Catching desyncs (verified)

- Every 60 confirmed frames each PC sends **CHECK**: a hash chained over the gameplay state of every confirmed fight frame since the last
  one. Zone frames (round starts, KOs, replays; about a quarter of an armed battle) are not in the chain. A mismatch shows DESYNC on both
  PCs, writes a dump of the window's per-frame hashes and state, and drains. A desync can't be merged back: the two games may already
  differ.
- **What is hashed** (the "tier-1" gameplay state, `c1a_run.c`): 11 shared components and 7 per player.

| Component | What |
|---|---|
| `fc` | the frame counter (`0x14470d0c4`) |
| `tt`, `mrg`, `mt` | the RNGs: TT800 (`0x14485eb28`), MRG (`0x14470e2b0`), an `std::mt19937` (`0x144100ea0`) |
| `lcg` | the plugin's replacement for the CRT `rand` state (the single-PC record and replay modes redirect `rand` / `srand`; the online mode doesn't, so this is constant online) |
| `sd` | the seed copy and reseed word |
| `md` | round, mode, replay and time scales |
| `cin` | the core's step input words (`0x144855700`, 0x10 bytes) and the third block's core copy (`0x14470d100`, 0x20 bytes); the cross-PC CHECK hashes only the first |
| `ctl`, `ai` | the player controllers and the CPU AI blocks, pointer-normalized |
| `rcv` | the game's own replay check value |
| per player: `pos`, `side`, `inp`, `st`, `gau`, `mv`, `aif` | offsets from the player object (player `p` at `0x1447156f0 + p * 0x973f0`): position fields (`+0x94`, `+0xa0`, `+0xc0`, `+0x2090`), screen side and input mirror (`+0x124`, `+0x16e4`, plus `+0x23c`, `+0x324`), input words and history (`+0x2150`, `+0x2188`, the ring at `+0x2190`), the status block (`+0x1b38`), the gauges (`+0x43dfc`), the current move ids (`+0x4495a`, `+0x444f2`), and the flags the AI reads (`+0x16d8`, `+0x19a0`, `+0x4412a`). Exact lengths: `c1a_run.c`, the tier-1 function |

- **Never compared between PCs:** the third input block and its core copy, the UE input mirrors (`0x1498 / 0x14a8 / 0x14b8 / 0x14c8 /
  0x12f8 / 0x1480`), the lip-sync and face state, and anything inside a zone.

## 8. Testing without a second player

The plugin records a real match (every frame's inputs and the tier-1 hash) and replays it in the real game on one PC while an emulated
network delays one side's inputs, so the plugin has to predict, roll back and re-simulate; the hashes must equal a zero-delay replay on
every frame (NOTES.md 3.1). The same rig, with a hook that can overwrite the extra argument slot, proved the stock-game bug (NOTES.md
1.5). A separate test harness runs two copies of a fake game in one process with a fake Steam layer that adds loss, delay, jitter and
reordering; it is not published.

## 9. Still open

- The listener slots in section 6.
- **Late sound stops (0.36)** have run in a real game only 6 times (one two-PC battle at about 335 ms); the rest of their proof is a model
  of UE's sound handler and the test harness (NOTES.md 4.7). Which sound cues loop is unknown.
- **The late spark, sound and voice** (0.35 / 0.36) passed three two-PC battles with every check in agreement; more battles, pings and
  characters would show more.
- **Beyond the window:** at about 330 ms (delay 4 + 7 frames of rollback) the game still waited on the other PC on about 1 frame in 10
  (735 and 769 waits in 7,616 armed frames; 11 to 13% of frames in the 0.36 sessions at about 335 ms); more at higher ping. A larger window
  needs deeper rollback proven first.
- **Audio** stutters at high ping (hypothesis: the waits plus sound calls missed or orphaned by rollbacks).
- **The stock-game bug's natural rate** between two PCs is unmeasured (NOTES.md 1.7).
- **Ranked can't be told apart** from Casual or Player Match: no data-only detector was found (hypothesis that none exists).

## 10. Source map

| File | What |
|---|---|
| `main.c`, `config.c`, `log.c` | loading, the ini, the log |
| `nobs*.c/.S`, `nobs_body.c` | the passive layer: the transparent hooks and the network observer |
| `rbo.c`, `rbo_install.c`, `rbo_logic.inc`, `rbo_fetch.c`, `rbo_core.c`, `rbo_body.c`, `rbo_thunks.S`, `rbo_l3s.S` | the online mode: the per-battle state machine, the hooks, FetchInput, the stall lift, the delay, zones, time sync, the context switch |
| `p2p.c`, `rbomsg.c/.h`, `netproto.c` | the Steam side channel, the message formats, the CRC and byte codec |
| `rb.c`, `snap.c`, `arena.c`, `mem.c`, `defer.c`, `pres.c`, `l4.c`, `modes.c`, `fwd.c/.h`, `fwd_thunks.S` | the rollback core: snapshots, the arena and allocator routing, deferred frees, re-simulation, the listener proxy, the listener rules and the guard, late effect delivery |
| `rbo_q1.inc`, `rbo_q1_table.h`, `scan.c` | pins: the typed root walk and the container census |
| `c1a*.c` | recording, the tier-1 hash, replay |
| `emu.c` | the emulated network for single-PC tests |
| `lipsync.c` | lip-sync under rollback |
| `argcfix.c/.h` | the stock-game bug fix (NOTES.md Part 1) |
| `overlay.c`, `cue.c` | the status badge and the sound cue |
| `vm.c`, `vm_thunks.S` | move-script VM diagnostics (off by default) |
| `net.c`, `netsess.c`, `entry.c`, `netem.c`, `sha256.c`, `l1.c`, `l1_stub.S`, `watch.c`, `writeset.c` | older modes (a UDP netplay test mode) and diagnostics; not used by the online mode |
