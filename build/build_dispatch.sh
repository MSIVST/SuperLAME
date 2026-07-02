#!/usr/bin/env bash
# Build the four prefixed libmp3lame engines for the CPUID-dispatch fat binary:
#   znver5 __zv5_ / znver4 __zv4_ (AVX-512) / znver3 __l3v_ (AVX2) / x86-64 __lsse_ (SSE2)
# A CPUID check at startup selects which engine the workers use.
#
# Paths are derived from $ROOT (the directory that holds lame-r6531/, mpg123-*/,
# r8brain/, and the build/ output dir). Override by exporting ROOT, e.g.:
#   ROOT=/path/to/deps bash build/build_dispatch.sh
set -u
# Default ROOT to the repo's parent (deps sit alongside the checkout); override
# by exporting ROOT to wherever lame-r6531/, mpg123-*/, r8brain/ and build/ live.
ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
SRC="$ROOT/lame-r6531"
MPG123INC="$ROOT/mpg123-1.33.6/src/include"
B="$ROOT/build"
D="$B/dispatch"; mkdir -p "$D"
CRT="-D_DLL -D_MT -Xclang --dependent-lib=msvcrt"
SOURCES="VbrTag bitstream encoder fft gain_analysis id3tag lame newmdct presets psymodel quantize quantize_pvt reservoir set_get tables takehiro util vbrquantize version mpglib_interface"

build_engine () {
  local name="$1" march="$2" extra="$3" prefix="$4"
  local OUT="$D/$name"; mkdir -p "$OUT/obj"
  cat "$B/phase4-znver3/config.h" > "$OUT/config.h"
  [ -n "$extra" ] && echo "$extra" >> "$OUT/config.h"
  local INC="-I$OUT -I$SRC -I$SRC/include -I$SRC/libmp3lame -I$SRC/mpglib -I$MPG123INC"
  local CF="-O3 -march=$march $CRT -DHAVE_CONFIG_H -DSTDC_HEADERS -Wno-implicit-function-declaration -Wno-deprecated-non-prototype -Wno-pointer-sign $INC"
  echo "  [$name] compiling @ -march=$march"
  for s in $SOURCES; do
    clang $CF -c "$SRC/libmp3lame/$s.c" -o "$OUT/obj/$s.o" 2>"$OUT/obj/$s.err" || { echo "  FAIL $name/$s"; head -5 "$OUT/obj/$s.err"; return 1; }
  done
  # vector intrinsics file only when HAVE_XMMINTRIN_H is set
  if echo "$extra" | grep -q HAVE_XMMINTRIN_H; then
    clang $CF -c "$SRC/libmp3lame/vector/xmm_quantize_sub.c" -o "$OUT/obj/xmm_quantize_sub.o" 2>/dev/null || true
  fi
  # Build ONE global rename map of every symbol DEFINED anywhere in this engine
  # (LAME's own funcs + data), then apply that SAME map to EVERY object. This
  # renames both definitions and internal cross-references consistently, while
  # leaving undefined CRT/mpg123 imports untouched (they're not in the map).
  : > "$OUT/rename.map"
  for o in "$OUT"/obj/*.o; do
    llvm-nm --defined-only "$o" 2>/dev/null | awk '{print $NF}'
  done | sort -u | awk -v p="$prefix" 'NF{print $0" "p$0}' > "$OUT/rename.map"
  echo "  [$name] rename map: $(wc -l < "$OUT/rename.map") symbols"
  for o in "$OUT"/obj/*.o; do
    llvm-objcopy --redefine-syms="$OUT/rename.map" "$o"
  done
  llvm-ar rcs "$OUT/libmp3lame.a" "$OUT"/obj/*.o
  echo "  [$name] lib: $(ls -la $OUT/libmp3lame.a|awk '{print $5}') bytes (prefix $prefix)"
}

echo "=== Building four engines (znver5 / znver4 / znver3 / sse2) ==="
# All AVX2+ engines get the SSE-intrinsics quantize path (HAVE_XMMINTRIN_H);
# the generic x86-64 engine also has SSE2 available on any 64-bit CPU.
SSEDEF="#define HAVE_XMMINTRIN_H 1
#define MIN_ARCH_SSE 1"
build_engine "znver5" "znver5" "$SSEDEF" "__zv5_"  || exit 1
build_engine "znver4" "znver4" "$SSEDEF" "__zv4_"  || exit 1
build_engine "znver3" "znver3" "$SSEDEF" "__l3v_"  || exit 1
build_engine "sse2"   "x86-64" ""        "__lsse_" || exit 1
echo "=== engines built ==="
