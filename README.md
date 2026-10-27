# SC6 Rollback (sc6rb)

Rollback netcode for **SOULCALIBUR VI** on Steam (PC), as a native plugin. **Experimental.**

- It works inside the game's own online matches. When both players have the plugin, each battle switches to rollback with a short input
  delay (2 to 4 frames, chosen from the ping). Against a player without it, the game's own netcode runs, untouched.
- It also carries a four-byte fix for a bug in SC6 itself that can make a Reversal Edge clash come out differently on two PCs. See
  [The stock-game bug](#the-stock-game-bug) and [docs/NOTES.md](docs/NOTES.md).

Version 0.34.1. Built for the current Steam build of the game (`SoulcaliburVI.exe` sha256
`F8904E4B04BCA3B47BC52A683F6190365D2EB89EE8F44F8072759E9C5E04A553`).

## Status

- Tested between two PCs over Steam: 14 battles in Player Matches, at about 55 to 650 ms round trip. Both copies turned rollback on at the
  same frame in every battle. One battle desynced, in a Reversal Edge clash, before the stock-game fix below.
- With 0.34.0 (the same gameplay code as 0.34.1): 3 battles at about 110 and 330 ms, every cross-PC check passed, and all 14 Reversal Edge
  clashes in them came out the same on both PCs.
- Known issues:
  - Some visual effects are still missed or left behind by a rollback. For example, a Reversal Edge's white startup spark sometimes doesn't
    show, and once stayed on a weapon for a whole round.
  - At about 330 ms the game still waits on the other PC on roughly 1 frame in 10, and more at higher pings (the badge turns yellow,
    "at limit").
  - Audio can stutter at high ping.
- Not tested: Linux / Proton, and running together with other mods that hook the game (UE4SS-based mods such as HorseMod). Don't run it
  together with another rollback mod such as SC6 Astral.

## Install

You need [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) (x64) by ThirteenAG, and this plugin from the
[Releases](../../releases) page. **Both players need it.**

1. Close the game.
2. Open the game folder: in Steam, right-click SOULCALIBUR VI, then Manage, then Browse local files. Go to `SoulcaliburVI\Binaries\Win64`.
3. Put the ASI Loader's x64 DLL there and name it `dsound.dll`. Skip this if another mod already put an ASI loader there.
4. Make a `plugins` folder there, and copy `sc6rb.asi` and `sc6rb.ini` from the release zip into it.
5. Start the game. At the main menu the bottom center should say `sc6rb 0.34.1 loaded`. **F2** hides and shows it.

The files end up as:

```
SoulcaliburVI\Binaries\Win64\dsound.dll
SoulcaliburVI\Binaries\Win64\plugins\sc6rb.asi
SoulcaliburVI\Binaries\Win64\plugins\sc6rb.ini
```

## Playing

Both players install it, then play an online match (a Player Match is the best place to try it). The badge at the bottom center says what
it's doing:

| Badge | Meaning |
|---|---|
| `sc6rb 0.34.1 loaded` (white) | Loaded, at the menus. |
| `PAIRING...` (yellow) | The two copies found each other and are agreeing on when to start. |
| `ROLLBACK ON  delay 2  ping 110 ms  rollbacks 123` (green) | Rollback is on for this battle. |
| `ROLLBACK ON - at limit (waiting on the other PC) ...` (yellow) | The ping is beyond what rollback can cover, so the game sometimes waits, as with the stock netcode. |
| `STOCK NETCODE (opponent not modded)` (grey) | The other player doesn't have the plugin (or has an incompatible version). The game's own netcode runs. |
| `STOCK NETCODE (<reason>)` (grey) | The two copies didn't agree (different settings, different move data, or one has the fix off). The game's own netcode runs. |
| `FALLBACK: <reason> - stock netcode for this battle` (orange) | Rollback stopped for the rest of this battle; the game's own netcode takes over. |
| `DESYNC - save your logs and report it` (red) | The two PCs' game states differ. Finish the battle, keep the logs and open an issue. |
| `ROLLBACK PLUGIN OFF: <reason>` (red) | The plugin couldn't install on this game build; nothing is changed. |

**Ranked:** the plugin can't tell Ranked from Casual or Player Match. With the shipped `sc6rb.ini`, rollback turns on in **any** online
match where both players have the plugin, Ranked included. To keep it off everywhere, set `rbonline_unranked_ack=0`: then nothing is probed
and every match runs the game's own netcode.

Both players need the same `rbonline_*` settings. Mods that change move data on one side make the two copies refuse to pair (the battle
runs the game's own netcode); cosmetic mods are fine.

## Logs and reporting problems

Logs go to `plugins\sc6rb_logs\`: one `.log` per launch, a record of each battle, and a network trace (`.nobs`, up to 256 MB per launch;
`netobs_trace=0` in `sc6rb.ini` turns it off). If something goes wrong (red DESYNC, orange FALLBACK, a crash, a freeze), quit the game,
keep that folder and open an issue saying what happened, roughly when, and the ping shown. The logs and traces contain no SteamIDs or IP
addresses (checked on real ones from both test PCs), but the logs do contain file paths (the game folder, the log folder), so look them
over before sharing if your Windows user name is private.

## Uninstall

Close the game and delete `plugins\sc6rb.asi`, `plugins\sc6rb.ini` and `plugins\sc6rb_logs\`. Delete `dsound.dll` too if no other mod
uses it.

## The stock-game bug

SC6's move-script VM has a predicate function (`EvaluateIfOpcode`, `0x1403732F0`) whose sub-ops `0x25` and `0x91` ("newly pressed
direction", for the player and the opponent) read one argument past the end of their argument list. That extra value is a leftover from
an earlier call on the stack, and it differs per launch. On a launch whose leftover value fails the test, a Reversal Edge direction choice
silently doesn't register (shown with a forced value; not yet caught on a natural launch, see the notes). Two PCs hold different
leftovers, so one PC could register a choice and the other not.

The plugin flips four jump bytes in memory so these two sub-ops stop where their correct siblings (`0x24`, `0x90`) do. It does this only
in a battle where both players have the plugin and both advertise the fix, and puts the original bytes back at the battle's end. It never
writes the exe on disk.

[docs/NOTES.md](docs/NOTES.md) has the details, the evidence (what is shown and what is still a hypothesis), and how to check it yourself.
`python tools/argc_static.py "<path to SoulcaliburVI.exe>"` checks the four sites in your exe (needs `pip install pefile capstone`).

## How it works, briefly

- **Passive by default.** Hooks around the battle step are register- and stack-transparent, so the game runs exactly as without the plugin.
  The plugin writes no game data (its hooks are its only change) and sends nothing on the game's own network channel. It probes for a modded opponent on its own
  Steam P2P channel (`0x52424F4E`), which the unmodded game never reads.
- **Pairing.** When both answer, a handshake compares the protocol number, the settings, the gameplay content (move data and hitbox
  layout) and the fix, and both agree on the delay from their measured pings.
- **Rollback.** From the battle's first round, inputs are predicted, and the battle state (the game's heap blocks, statics and RNG) is
  snapshotted every frame and restored to resimulate when a prediction was wrong. Round starts, KOs and replays run at the game's own input
  delay. Every 60 frames the two PCs compare a hash chained over the gameplay state of the fight frames in between (round starts, KOs
  and replays are not in it); a mismatch shows DESYNC.
- **Presentation.** Calls from the battle core into Unreal (effects, sounds, lip-sync, the KO slow motion) are handled per type: some are
  muted in predicted frames and delivered from the confirmed frame, some are delivered late, and Unreal's writes back into battle state are
  undone.

[docs/NOTES.md](docs/NOTES.md) has more on the game internals this needed.

## Building

- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (clang targeting `x86_64-w64-mingw32`, UCRT). Releases are built with llvm-mingw
  20260922 (ucrt, x86_64).
- Git Bash or MSYS2 on Windows. MinHook 1.3.4 is included in `third_party/minhook`.

```
SC6RB_CC=/c/llvm-mingw/bin/x86_64-w64-mingw32-clang ./build.sh
```

This writes `build/sc6rb.asi`. The linker stamps the build time into the PE header, so compare builds with
`python tools/asi_hash.py build/sc6rb.asi`: its `normalized` hash zeroes that stamp. With the same toolchain, the normalized hash matches the
one published with each release.

## Repository layout

```
src/                 the plugin
third_party/minhook/ MinHook 1.3.4 (BSD 2-Clause)
tools/argc_static.py static check of the stock-game bug and the fix's four sites
tools/asi_hash.py    sha256 of a build, with the PE timestamp zeroed
sc6rb.ini            the shipped settings
docs/NOTES.md        reverse-engineering notes
build.sh             the build
```

The source supports more modes than `rbonline` (the online mode above): `record`, `replay` and `rbreplay` record a real match and replay
it on one PC with emulated network delay, which is how most of this was tested without a second player. Comments in the source cite design
notes by name (for example `OPTA-M2-SPEC.md §23` or `design_v0295/...`), the test harness (`test/`, the `SC6RB_HARNESS` code paths) and
other static-check scripts in `tools/`; those are not published.

## License

MIT, see [LICENSE](LICENSE). MinHook is BSD 2-Clause, see [THIRD_PARTY.md](THIRD_PARTY.md).

Not affiliated with or endorsed by Bandai Namco. SOULCALIBUR is a trademark of Bandai Namco Entertainment Inc.

## Thanks

- Fotten's [SC6ModdingDocs](https://github.com/FottenSC/SC6ModdingDocs); the notes use its function names.
- The other SC6 rollback projects: SC6 Astral and HorseMod.
- MinHook (Tsuda Kageyu) and Ultimate ASI Loader (ThirteenAG).
