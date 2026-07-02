# Build Plan: SuperFast-style, quality-patched, libmpg123-backed LAME for znver3

> **Status: FINALIZED** — all decisions locked. Ready to execute Phase 0.

## Goal

Produce a Windows binary that is:

1. **LAME trunk r6531** ("3.101 beta 3", 2021-03-09 — the de-facto current LAME) as the encoder core (`libmp3lame`).
2. With **libmpg123 (1.33.6)** as the internal MPEG-audio decoder (replacing bundled `mpglib`).
3. With the **maikmerten "q4" quality fix — Variant B** applied (LAME bug #516), **plus a verbose-mode notice** when the clamp engages.
4. Wrapped in a **standalone multithreaded "SuperFast" frontend** reimplementing Kausch's chunk-split + bit-reservoir repack around our `libmp3lame` (no fre:ac/BoCA dependency).
5. Compiled with **clang `-march=znver3`** plus a **generic-x86-64 (SSE2) fallback**, selected at runtime by CPUID dispatch.

## Locked decisions (with rationale)

| Decision | Choice | Why |
|---|---|---|
| **Encoder base** | **LAME trunk r6531**, not the 3.100 tarball | r6531 already integrates libmpg123 (2020-07-11) + clang flag handling (2021-03-09); the maikmerten diffs are authored *against* r6531 so they apply with `patch -p0`. It's the base every current community/winLAME/fre:ac build uses. Upstream is in permanent hibernation — r6531 is effectively the final state. |
| **Quality fix** | **Variant B** (`cbr-abr-quality-settings-clamp`, 2024-07-25) **+ verbose notice** | Most recent maikmerten patch; matches the community-standardized "q4 patch" naming/intent and the direction fre:ac itself is moving. Verbose notice added so the silent `-q`→4 remap is discoverable. |
| **SuperFast delivery** | **Standalone MT frontend** | SuperFast is a frontend technique over *unmodified* libmp3lame, not a LAME patch. We port the algorithm and drop the fre:ac/BoCA/smooth runtime. |
| **Platform / toolchain** | **Windows, clang/LLVM** | Required for `-march=znver3` and the optional tmkk SIMD intrinsics (neither works under MSVC). |

## Variant B — exact change + the added notice

Insert into the `default: /* cbr/abr */` arm of `lame_init_params` (in r6531; in the local 3.100 tree this is `libmp3lame/lame.c:1050–1051`):

```c
if (gfp->quality < 0)
    gfp->quality = LAME_DEFAULT_QUALITY;

/* maikmerten bug #516: VBR/ABR psymodel + noise_shaping_amp at -q<4
 * degrades CBR/ABR quality. Clamp to 4. */
if (gfp->quality < 4) {
    if (gfp->quality >= 0 && cfg->msfix /* verbose proxy */) { /* see note */ }
    gfp->quality = 4;
}
```

**Verbose notice (our addition, not in the diff):** emit a one-line message when the clamp actually changes the requested value, gated on verbose output — e.g. via the existing `MSGF`/`ieee754_*` logging path used at `lame.c:1562`. Wording: `CBR/ABR: -q clamped to 4 (bug #516 quality fix)`. Only printed when `requested_q < 4` and `--verbose` is on, so default/`-q 4+` runs stay quiet.

**Behavioral effect (CBR/ABR only; VBR untouched):** `-q 0..3` become equivalent to `-q 4`; default (`LAME_DEFAULT_QUALITY = 3`) effectively becomes 4; `-q 4..9` unchanged. No CLI flags added/removed — `-q` is still parsed normally, just clamped internally. `--verbose` will show `quality: 4` and `amplification: 0`, confirming the fix is live.

## Source inventory (staged in the build root; see docs/BUILDING.md)

| Path | Role |
|------|------|
| `lame-3.100/` | reference only; **actual base = r6531 checkout/snapshot** |
| `mpg123-1.33.6/` | decoder to integrate |
| `superfast-1.0-pre3/components/lame/` | reference algorithm (`repacker.cpp`, `worker.cpp`, `framecrc.cpp`, `dllinterface.cpp`) |
| `maikmerten-2024-07-25-cbr-abr-quality-settings-clamp.diff` | **the patch we apply (Variant B)** |
| `maikmerten-2024-07-10-cbr-and-abr-no-noise-shaping-amp.diff` | Variant A — kept for reference / optional A/B-by-ear |
| `maikmerten.md` | bug #516 context |
| `ffmpeg.exe` / `ffprobe.exe` | validation/decoding for regression checks |

---

## Phases

### Phase 0 — Toolchain & base checkout
- Install clang/LLVM + CMake/Ninja on Windows; confirm `clang --print-supported-cpus` lists `znver3`.
- Obtain **LAME trunk r6531** (SVN checkout `-r 6531`, or equivalent snapshot). Keep local `lame-3.100/` as reference.
- Build **stock r6531** unmodified → reference `lame.exe` + known-good MP3 output for later regression.
- Build `mpg123-1.33.6` as a static `libmpg123` with clang.

### Phase 1 — libmpg123 as LAME's decoder
- Configure r6531 with `--with-mpg123` (or the CMake/MSVC-style equivalent under clang), pointing at our `libmpg123`. (Largely pre-wired in r6531 — this is configure + verify.)
- Confirm `HAVE_MPG123` path compiles and `mpglib` is bypassed.
- Regression: `lame --decode` a known MP3, compare PCM against the stock reference.

### Phase 2 — Variant B quality patch + verbose notice
- Apply `maikmerten-2024-07-25-cbr-abr-quality-settings-clamp.diff` to `libmp3lame/lame.c` (`patch -p0` against r6531 — clean apply expected).
- Add the verbose-mode notice (above): print `CBR/ABR: -q clamped to 4 (bug #516 quality fix)` only when requested `-q < 4` **and** verbose is enabled.
- Verify: `lame --verbose -q 0 -b 320` reports `quality: 4`, `amplification: 0`, and prints the notice; `-q 5` does not clamp and prints nothing; VBR (`-V2 -q 0`) is unaffected.

### Phase 3 — Standalone SuperFast MT frontend
- Port from `superfast-1.0-pre3/components/lame/`:
  - `worker.*` → thread pool, one `lame_global_flags` per worker, round-robin overlapping chunks.
  - `repacker.*` → bitstream unpack → reservoir-aware repack → dummy-data re-encode fallback for un-fittable frames. **(Highest-risk component — preserve logic exactly.)**
  - `framecrc.*` → frame integrity.
  - Replace `dllinterface.*` (runtime DLL loader) + BoCA/smooth glue with **direct static linking** to our patched `libmp3lame` and a plain CLI arg parser (pass-through of `-q`, `-b`, `-V`, `--abr`, etc.).
- Output must be bit-reservoir-correct and decode cleanly (validate with our libmpg123 + bundled ffmpeg).
- Validate: equivalent output vs single-threaded encode; measure speedup across core counts.

### Phase 4 — znver3 build + legacy fallback
- Build the hot DSP objects twice: `-march=znver3` and `-march=x86-64` (SSE2 baseline).
- Runtime CPUID dispatch (AVX2/FMA/BMI2 → Zen3 path; else fallback). Ship one binary.
- Verify the fallback path actually runs (dispatch override / older CPU / emulation).
- **Optional stretch:** fold tmkk SIMD-LAME intrinsics into FFT/MDCT/quant/Huffman for real Zen-3 throughput — clang-only.

### Phase 5 — Package & validate
- Final binary + version string identifying lineage (e.g. `LAME r6531 / libmpg123 1.33.6 / q4-patch / SuperFast-MT / znver3`).
- Regression suite: encode a corpus at CBR/ABR/VBR → decode → compare; confirm gapless/LAME-tag correctness; benchmark vs stock single-thread.

## Risks / watch-items
- **Reservoir repack correctness** — the hard part. Mishandled seams → invalid MP3s. `repacker.cpp` is the authority; keep its dummy-data fallback intact.
- **r6531 build under clang on Windows** — autotools/`--with-mpg123` flow may need a CMake or hand-rolled build shim for clang-on-Windows; budget time in Phase 0/1.
- **MSVC fallback forfeits SIMD** — chose clang precisely to avoid this; don't switch toolchains mid-project.
- **Per-worker patch consistency** — Variant B lives in `lame_init_params`, which every SuperFast worker calls, so the clamp + notice apply uniformly across instances (notice may print once per worker — dedupe to a single emission in the frontend).
- **Licensing (redistribution)** — LAME (LGPL) + mpg123 (LGPL) + SuperFast-derived frontend (fre:ac → GPL): combined work likely GPL. Note before shipping.
