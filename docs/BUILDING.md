# Building SuperLAME

## Toolchain
- **clang/LLVM** (required — MSVC cannot build the `-march=znverN` intrinsics or
  r8brain's SIMD). Tested with clang 21 targeting `x86_64-*-windows-msvc`.
- CMake (for the mpg123 build), NASM (for mpg123 asm), llvm-ar / llvm-objcopy.
- **llvm-rc** (optional) — embeds the app icon + version info. If absent, the
  build still succeeds, just without the icon.

## Vendored (no fetch needed)
- **dr_flac** (`dr_flac/dr_flac.h`) — single-header FLAC decoder, already in the
  repo. Compiled once via `src/dr_flac_impl.c`.

## Fetch the external sources
Place these next to this repo (the build scripts expect them under
a deps directory alongside this repo, or set `ROOT` when invoking build/*.sh):

1. **LAME r6531** — SVN snapshot of trunk at revision 6531. Apply the patch in
   `patches/` (maikmerten q4 fix) to `libmp3lame/lame.c`, plus the small
   verbose-notice + `q_clamped_from` field described in docs/BUILD_PLAN.md.
2. **mpg123** (>= 1.26) — build the static `libmpg123` via its `ports/cmake`
   with clang (dynamic UCRT: `-D_DLL -D_MT --dependent-lib=msvcrt`). Add
   `-ffile-prefix-map=<your mpg123 dir>=mpg123/` to `CMAKE_C_FLAGS_RELEASE` so
   libmpg123's `__FILE__`-based error strings do not bake your local build path
   into the final binary. (LAME already uses relative `__FILE__`, so only
   mpg123 needs this.)
3. **r8brain-free-src** — clone; header-only, no build step.

## Build
```sh
bash build/build_dispatch.sh   # compiles libmp3lame FOUR times, one per CPU tier,
                               # symbol-prefixing each engine:
                               #   znver5 __zv5_ / znver4 __zv4_ / znver3 __l3v_ / x86-64 __lsse_
bash build/build_final.sh      # compiles the frontend (+ dr_flac) and links ONE
                               # binary with all four engines, mpg123, r8brain,
                               # and (if llvm-rc present) the icon resource
```
Output: `build/final/SuperLAME-1.0.exe` (a `superlame-mt.exe` copy is also
written for the test scripts).

The four engines are selected at runtime by CPUID (see `src/lame_dispatch.cpp`).
NOTE: znver4/znver5 are built but **unverified** — they contain AVX-512 code that
no Zen 4/5 host was available to execute during development; znver3 and the SSE2
fallback are tested. On a non-AVX-512 CPU the auto-selector never picks them, and
forcing one via `SUPERLAME_ENGINE=znver5` warns and falls back rather than
crashing.

## CRT / linking notes
- Everything uses the dynamic UCRT; static `libucrt.lib`/`libcmt.lib` are
  excluded via `/nodefaultlib` to avoid CRT collisions with mpg123.
- Extra system libs: `-lshlwapi -lshell32` (mpg123 path APIs, CommandLineToArgvW).

## Tests
- `tests/regression.ps1` — 28-case validity / ST-vs-MT / engine-parity suite.
- `tests/fuzz_input.ps1` — malformed WAV/AIFF/MP3/FLAC input fuzzing.
- `tests/flac_conformance.ps1` — IETF CELLAR flac-test-files corpus.
- `tests/odaq.ps1` — objective ST-vs-MT parity over the ODAQ reference corpus.
- `tests/sqam.ps1`, `tests/scaling.ps1` — SQAM quality + core-scaling benchmarks.

See docs/BUILD_PLAN.md for the full rationale and history.
