#!/usr/bin/env bash
# Phase 4 final: link the frontend + dispatch shim against BOTH prefixed engines
# (znver3 + sse2) into one binary with runtime CPUID selection.
set -u
MT="/c/.Claude_LAMEsf/superlame-mt"
LAMEINC="/c/.Claude_LAMEsf/lame-r6531/include"
MPG123LIB="/c/.Claude_LAMEsf/build/mpg123-cmake/src/libmpg123/mpg123.lib"
ZV5="/c/.Claude_LAMEsf/build/dispatch/znver5/libmp3lame.a"
ZV4="/c/.Claude_LAMEsf/build/dispatch/znver4/libmp3lame.a"
L3V="/c/.Claude_LAMEsf/build/dispatch/znver3/libmp3lame.a"
LSSE="/c/.Claude_LAMEsf/build/dispatch/sse2/libmp3lame.a"
OUT="/c/.Claude_LAMEsf/build/final"; mkdir -p "$OUT"

MPG123INC="/c/.Claude_LAMEsf/mpg123-1.33.6/src/include"
CRT="-D_DLL -D_MT -Xclang --dependent-lib=msvcrt"
# Frontend itself built at znver3 (it's not hot; the engines carry the DSP).
R8BRAIN="/c/.Claude_LAMEsf/r8brain"
CXXFLAGS="-std=c++17 -O2 -march=x86-64 $CRT -I$MT -I$LAMEINC -I$MPG123INC -I$R8BRAIN -Wall -Wno-unused-parameter"

echo "=== Compiling frontend + dispatch ==="
fail=0
for s in mp3frame repacker worker main lame_dispatch; do
  if ! clang++ $CXXFLAGS -c "$MT/$s.cpp" -o "$OUT/$s.o" 2>"$OUT/$s.err"; then
    echo "FAILED: $s.cpp"; head -25 "$OUT/$s.err"; fail=1
  fi
done
[ $fail -eq 1 ] && { echo "compile failed"; exit 1; }

# dr_flac implementation: one C TU (~530KB header). Portable (runtime SIMD),
# so a single generic-arch object is fine.
CFLAGS_FLAC="-O2 -march=x86-64 $CRT -Wall -Wno-unused-function -Wno-unused-parameter"
if ! clang $CFLAGS_FLAC -c "$MT/dr_flac_impl.c" -o "$OUT/dr_flac_impl.o" 2>"$OUT/dr_flac_impl.err"; then
  echo "FAILED: dr_flac_impl.c"; head -25 "$OUT/dr_flac_impl.err"; exit 1
fi

# Windows resource: application icon + version info. llvm-rc emits a COFF .res;
# clang links it directly. Non-fatal if llvm-rc is missing (icon just omitted).
RESOBJ=""
if command -v llvm-rc >/dev/null 2>&1; then
  if llvm-rc -fo "$OUT/superlame.res" "$MT/res/superlame.rc" >"$OUT/rc.log" 2>&1; then
    RESOBJ="$OUT/superlame.res"
    echo "  [res] icon + version info compiled"
  else
    echo "  [res] WARNING: llvm-rc failed; building without icon"; cat "$OUT/rc.log" | head -5
  fi
else
  echo "  [res] llvm-rc not found; building without icon"
fi

EXENAME="SuperLAME-1.0.exe"
echo "=== Linking $EXENAME (both engines) ==="
SYSLIBS="-lshlwapi -lshell32 -Xlinker /nodefaultlib:libucrt.lib -Xlinker /nodefaultlib:libcmt.lib"
clang++ $CXXFLAGS "$OUT"/mp3frame.o "$OUT"/repacker.o "$OUT"/worker.o "$OUT"/main.o "$OUT"/lame_dispatch.o "$OUT"/dr_flac_impl.o $RESOBJ \
  "$ZV5" "$ZV4" "$L3V" "$LSSE" "$MPG123LIB" $SYSLIBS -o "$OUT/$EXENAME" 2>"$OUT/link.err" \
  && echo "LINK OK: $OUT/$EXENAME" \
  || { echo "LINK FAILED:"; grep -iE 'error|unresolved|multiply' "$OUT/link.err" | head -25; exit 1; }
# Keep a stable superlame-mt.exe name too (test harnesses reference it).
cp -f "$OUT/$EXENAME" "$OUT/superlame-mt.exe" 2>/dev/null || true
