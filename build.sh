#!/usr/bin/env bash
# Build sc6rb.asi (the SoulCalibur VI rollback plugin) with llvm-mingw.
#
#   ./build.sh                  -> build/sc6rb.asi
#
# Needs the llvm-mingw toolchain (clang targeting x86_64-w64-mingw32, UCRT): https://github.com/mstorsjo/llvm-mingw
# The released binaries are built with llvm-mingw 20260922 (ucrt, x86_64). Run from Git Bash or MSYS2 on Windows.
# If x86_64-w64-mingw32-clang is not on PATH, point SC6RB_CC at it:
#   SC6RB_CC=/c/llvm-mingw/bin/x86_64-w64-mingw32-clang ./build.sh
# SC6RB_OUT changes the output folder (default: build/).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${SC6RB_CC:-x86_64-w64-mingw32-clang}"
MH="$HERE/third_party/minhook"
OUT="${SC6RB_OUT:-$HERE/build}"
mkdir -p "$OUT"

CFLAGS=(-O2 -Wall -Wextra -Wformat=2 -Wno-unused-parameter -Wno-missing-field-initializers -fno-strict-aliasing -mno-red-zone)
# The hook bodies (nobs_body.c, rbo_body.c) run inside the game's own call stack: no compiler-invented memcpy / memset, no vector
# registers, no stack protector, so they leave the caller's register and stack state untouched.
BODY_FLAGS=(-fno-builtin -mgeneral-regs-only -fno-stack-protector -Werror=cast-qual -Werror=incompatible-pointer-types-discards-qualifiers
            -Werror=int-conversion)

S="$HERE/src"
PLUGIN_SRCS=("$S/main.c" "$S/config.c" "$S/log.c" "$S/mem.c" "$S/scan.c" "$S/writeset.c" "$S/modes.c" "$S/watch.c" "$S/l1.c" "$S/l1_stub.S"
             "$S/c1a_run.c" "$S/rb.c" "$S/netproto.c" "$S/net.c" "$S/netsess.c" "$S/entry.c" "$S/sha256.c" "$S/lipsync.c" "$S/nobs.c"
             "$S/nobs_install.c" "$S/nobs_thunks.S" "$S/rbo.c" "$S/rbo_install.c" "$S/rbo_fetch.c" "$S/rbo_core.c" "$S/rbo_thunks.S"
             "$S/rbo_l3s.S" "$S/p2p.c" "$S/rbomsg.c" "$S/overlay.c" "$S/cue.c" "$S/vm.c" "$S/vm_thunks.S" "$S/argcfix.c")
CORE_SRCS=("$S/arena.c" "$S/snap.c" "$S/defer.c" "$S/fwd.c" "$S/fwd_thunks.S" "$S/l4.c" "$S/pres.c" "$S/c1a.c" "$S/c1a_setup.c" "$S/emu.c")
MH_SRCS=("$MH/src/buffer.c" "$MH/src/hook.c" "$MH/src/trampoline.c" "$MH/src/hde/hde64.c")

"$CC" "${CFLAGS[@]}" "${BODY_FLAGS[@]}" -c "$S/nobs_body.c" -o "$OUT/nobs_body.o"
"$CC" "${CFLAGS[@]}" "${BODY_FLAGS[@]}" -c "$S/rbo_body.c" -o "$OUT/rbo_body.o"
"$CC" "${CFLAGS[@]}" -shared -o "$OUT/sc6rb.asi" "${PLUGIN_SRCS[@]}" "$OUT/nobs_body.o" "$OUT/rbo_body.o" "${CORE_SRCS[@]}" "${MH_SRCS[@]}" \
  -I"$MH/include" -static -s -lws2_32
echo "built $OUT/sc6rb.asi ($(stat -c %s "$OUT/sc6rb.asi") bytes)"
