# Third-party software

## MinHook 1.3.4 (bundled, BSD 2-Clause)

`third_party/minhook/` is MinHook 1.3.4 by Tsuda Kageyu (https://github.com/TsudaKageyu/minhook), unmodified. It is compiled into
`sc6rb.asi`. Its license, including the Hacker Disassembler Engine 64 notice, is in `third_party/minhook/LICENSE.txt`; the release zip carries the
same file as `LICENSE-MinHook.txt`.

## Ultimate ASI Loader (not bundled, MIT)

The plugin is loaded by Ultimate ASI Loader by ThirteenAG (https://github.com/ThirteenAG/Ultimate-ASI-Loader), installed as
`Binaries\Win64\dsound.dll`. It is not part of this repository or its releases; download it from its own releases page.

## SOULCALIBUR VI

This project is not affiliated with or endorsed by Bandai Namco. SOULCALIBUR is a trademark of Bandai Namco Entertainment Inc. The plugin
contains no game files. It carries short byte signatures of the game's code (the longest is 137 bytes), which it compares with the running
game before it patches anything, and it changes the game only in memory, never on disk.
