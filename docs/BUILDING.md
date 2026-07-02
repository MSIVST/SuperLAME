# Building SuperLAME

## Toolchain
- **clang/LLVM** (required — MSVC cannot build the `-march=znver3` intrinsics or
  r8brain's SIMD). Tested with clang 21 targeting `x86_64-*-windows-msvc`.
- CMake (for the mpg123 build), NASM (for mpg123 asm), llvm-ar / llvm-objcopy.

## Fetch the external sources
Place these next to this repo (the build scripts expect them under
`C:/.Claude_LAMEsf/` — adjust paths in build/*.sh to your layout):

1. **LAME r6531** — SVN snapshot of trunk at revision 6531. Apply the patch in
   `patches/` (maikmerten q4 fix) to `libmp3lame/lame.c`, plus the small
   verbose-notice + `q_clamped_from` field described in docs/BUILD_PLAN.md.
2. **mpg123** (>= 1.26) — build the static `libmpg123` via its `ports/cmake`
   with clang (dynamic UCRT: `-D_DLL -D_MT --dependent-lib=msvcrt`).
3. **r8brain-free-src** — clone; header-only, no build step.

## Build
```sh
bash build/build_dispatch.sh   # compiles libmp3lame TWICE (znver3 + x86-64),
                               # symbol-prefixes each engine (__l3v_ / __lsse_)
bash build/build_final.sh      # compiles the frontend + links one binary with
                               # both engines, mpg123, and r8brain
```
Output: `build/final/superlame-mt.exe`.

## CRT / linking notes
- Everything uses the dynamic UCRT; static `libucrt.lib`/`libcmt.lib` are
  excluded via `/nodefaultlib` to avoid CRT collisions with mpg123.
- Extra system libs: `-lshlwapi -lshell32` (mpg123 path APIs, CommandLineToArgvW).

See docs/BUILD_PLAN.md for the full rationale and history.
