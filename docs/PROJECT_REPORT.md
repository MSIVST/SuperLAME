# SuperLAME — Project Report & Hand-off

*A thorough description of what this project is, how it works internally, and
everything a new maintainer needs to build, test, release, and extend it.*

Version at time of writing: **1.0.2** · Repo: <https://github.com/MSIVST/SuperLAME>

---

## 1. What SuperLAME is (in one paragraph)

SuperLAME is a **multithreaded MP3 encoder built on LAME**. Its headline feature
is Robert Kausch's **"SuperFast"** technique: instead of encoding an MP3 serially
(stock LAME), it runs *many* `libmp3lame` instances in parallel on overlapping
chunks of the audio, then **reassembles a single, bit-reservoir-correct MP3** from
the pieces — roughly **8× faster than stock LAME at the same quality**. Around
that core it is a reasonably complete MP3 **codec**: it encodes, decodes
(`--decode` via libmpg123), reads a wide range of input formats (WAV/AIFF/FLAC),
resamples with a high-quality converter (r8brain), and writes ID3 tags including
album art. It ships as **one fat Windows binary** that contains four CPU-tuned
builds of the encoder and picks the right one at runtime via CPUID.

**Why it exists:** to get LAME-quality MP3s encoded much faster on modern
many-core CPUs, in a single self-contained tool, with modern format support that
stock LAME's frontend lacks.

---

## 2. The core idea — how "SuperFast" parallel encoding works

MP3 (Layer III) frames are **not independent**: each frame can borrow bits from a
shared *bit reservoir* filled by previous frames (`main_data_begin` points
backwards into earlier frames' byte space). That coupling is why you can't just
encode chunks in parallel and concatenate them — the reservoir chaining would be
broken at every seam.

SuperFast solves this in two stages:

1. **Parallel encode with overlap.** The input is split into ~128-frame chunks.
   Each chunk is handed to a worker thread running its own independent
   `libmp3lame` instance. Consecutive chunks **overlap** by a few frames so each
   worker's encoder "warms up" its reservoir state on frames the previous worker
   already owns.

2. **Serial repack (the crown jewel).** A single `SuperRepacker` on the main
   thread takes the workers' output frames in order, **unpacks** each frame's main
   data, and **re-chains the bit reservoir** across the seams — writing a
   continuous, spec-valid MP3 where every `main_data_begin` offset is correct.
   This stage is inherently serial (it stitches one continuous reservoir), which
   is why **scaling plateaus at ~2 workers** for a single file (see §7).

The result is byte-for-byte *not* identical to stock LAME's serial output, but the
difference is confined to inaudible (<−45 dB) reservoir reconciliation at chunk
seams — verified repeatedly (regression + ODAQ, §6). **Single-thread mode is
bit-identical to stock LAME.**

> This algorithm is ported from **fre:ac / BoCA** (`enzo1982/BoCA`), (C) Robert
> Kausch, GPL v2+. `repacker.cpp` is the highest-risk, highest-value file in the
> project. It was ported from the 2018 `superfast-1.0-pre3` snapshot and then
> **updated with ~5 years of BoCA fixes** (notably the 2019 "bit reservoir bigger
> than a frame" fix) after a real desync was found on dense material — see
> `docs/CHANGELOG`-style history in the memory notes and BUILD_PLAN.md.

---

## 3. Component stack

| Layer | Component | Version | License | Role |
|-------|-----------|---------|---------|------|
| Encoder core | **LAME** trunk | r6531 ("3.101 beta 3") | LGPL v2 | `libmp3lame` — the actual MP3 encoder |
| Quality patch | maikmerten "q4" | 2024-07-25 | (LAME bug #516) | clamps `-q≥4` for CBR/ABR |
| Tag patch | SuperLAME id3v2.4 synchsafe | this project | GPL (combined) | fixes APIC cover-art frame sizes |
| Parallel engine | **fre:ac/BoCA "SuperFast"** | 2018 + BoCA fixes | GPL v2+ | the repacker + worker orchestration |
| Decoder | **libmpg123** | 1.33.6 | LGPL v2.1 | MP3→WAV (`--decode`) |
| FLAC input | **dr_flac** | v0.13.4 | public domain / MIT-0 | FLAC decode (vendored) |
| Resampler | **r8brain-free-src** | latest | MIT | high-quality sample-rate conversion |

Because the SuperFast repacker is **GPL v2+**, the whole combined work is
**GPL v2 or later**. All other licenses (LGPL/MIT/public-domain) are
GPL-compatible. This is the reason binary distribution has GPL obligations
(source availability) — handled in `RELEASING.md` / `THIRD-PARTY.md`.

---

## 4. How the code is organized

The frontend lives in `src/` (mirrored from the build tree `superlame-mt/`). It's
~3,000 lines of C++17. Each third-party dependency sits behind a thin adapter, so
the main logic barely knows they exist.

| File | Lines | Responsibility |
|------|-------|----------------|
| `main.cpp` | 1233 | CLI parse, input dispatch, the `SuperEncoder` orchestrator, resample decision, output, self-heal, tag wiring |
| `repacker.cpp` / `.h` | 357/47 | **`SuperRepacker`** — bit-reservoir repack (the core) |
| `worker.cpp` / `.h` | 195/83 | **`SuperWorker`** — one thread + one prefixed `libmp3lame` instance |
| `lame_dispatch.cpp` / `.h` | 196/62 | 4-engine function-pointer table + CPUID selection |
| `flac_decode.h` | 240 | FLAC input via dr_flac (MT range-parallel decode, tag + art import) |
| `resample.h` | 144 | r8brain wrapper (`ResampleFloat`, parallelized per-channel/per-range) |
| `mp3frame.cpp` / `.h` | 30/148 | MP3 frame geometry helpers + CRC used by the repacker |
| `decode.h` | 81 | `--decode` path via libmpg123 feed API |
| `tags.h` | 74 | ID3v1/v2.4 rendering via LAME's tag serializer |
| `iodriver.h` | 72 | `MemDriver` — in-RAM seekable byte sink the repacker writes to |
| `audiodata.h` | 21 | `WavData` struct shared by WAV/AIFF/FLAC readers |
| `dr_flac_impl.c` | 5 | the single TU that compiles dr_flac's implementation |
| `res/superlame.rc` | — | Windows icon + VERSIONINFO |

### End-to-end data flow (a single encode)

```
argv (UTF-8, via GetCommandLineW on Windows)
  │
  ├─ ReadWav(path)              main.cpp — sniffs magic:
  │     ├─ "RIFF"/"WAVE"  →  PCM 8/16/24/32-bit + 32/64-float
  │     ├─ "FORM"/"AIFF"  →  ReadAiff()  (16/24-bit)
  │     └─ "fLaC"         →  DecodeFlac() (flac_decode.h, MT, imports tags+art)
  │
  ├─ (rate decision)  ProbeLameOutRate(cfg, inRate)   ← asks LAME what output
  │     if the chosen rate != input rate:              rate it would pick
  │        ResampleFloat(...)  (resample.h, r8brain)   THEN resample up-front
  │
  ├─ SuperEncoder enc(cfg, &driver, numThreads)
  │     enc.WriteData(all samples)    → EncodeFrames() dispatches overlapping
  │     enc.Finish()                    chunks to N SuperWorkers, then the
  │                                      SuperRepacker stitches the reservoir
  │
  ├─ FrameChainValid(driver.Bytes())?  ← self-heal: if the MT stream's frame
  │     if broken → re-encode single-threaded (always correct), use that
  │
  ├─ RenderTags(...)  → id3v2 (prepend) + id3v1 (append)
  └─ write outfile.mp3   (id3v2 · MP3 stream · id3v1)
```

`--decode` is a separate short path (`decode.h`): libmpg123 feed API → 16-bit WAV.

### The 4-engine CPU dispatch (why LAME is compiled four times)

`libmp3lame` is compiled **four times** with different `-march` flags, and each
build's symbols are **prefixed** so all four can coexist in one binary:

| Engine | `-march` | Prefix | Selected when |
|--------|----------|--------|---------------|
| `kZnver5` | `znver5` | `__zv5_` | AMD Zen 5+ with AVX-512 (**unverified** — no hw) |
| `kZnver4` | `znver4` | `__zv4_` | AVX-512 present (**unverified**) |
| `kZnver3` | `znver3` | `__l3v_` | AVX2+FMA+BMI2 (the tested path) |
| `kSse2` | `x86-64` | `__lsse_` | everything else (generic SSE2 fallback) |

The prefixing is done at link time with `llvm-objcopy --redefine-syms` using one
global rename map per engine (renames both definitions **and** internal
cross-references). The frontend never calls `lame_*` directly — it goes through a
`LameEngine` **function-pointer table** (`lame_dispatch.h`). `Choose()` runs
CPUID once at startup and binds the table to one engine. `SUPERLAME_ENGINE=sse2|
znver3|znver4|znver5` env var forces a specific engine (with a runnable-check that
warns and falls back if the CPU lacks the features).

**Intel support:** the engines emit standard AVX-512/AVX2/SSE2 (Intel's too), so
Intel CPUs run the matching engine correctly. They're AMD-*tuned*, so Intel runs a
few % below Intel-specific tuning, but functionally fine. The `znver5` engine is
AMD-only by design and never selected on Intel.

---

## 5. Building

### Toolchain (required)
- **clang/LLVM 21** targeting `x86_64-*-windows-msvc` (MSVC cannot build the
  `-march=znverN` intrinsics or r8brain SIMD). Also needs `llvm-ar`,
  `llvm-objcopy`, `llvm-nm`; `llvm-rc` (optional, for the icon).
- **CMake** + **NASM** (for the mpg123 static lib).

### External sources (fetched, not vendored — see `docs/BUILDING.md`)
Placed alongside the repo (or point `ROOT` at them):
- **LAME r6531** — apply the 3 patches in `patches/` to `libmp3lame/`.
- **mpg123 ≥ 1.26** — built via its CMake port with `-ffile-prefix-map` so no
  local build path leaks into the binary.
- **r8brain-free-src** — header-only, no build step.
- **dr_flac** — *vendored* in `dr_flac/dr_flac.h`; nothing to fetch.

### Build commands
```sh
bash build/build_dispatch.sh   # compiles libmp3lame 4× (one per CPU tier),
                               # symbol-prefixing each engine
bash build/build_final.sh      # compiles the frontend (+ dr_flac) and links ONE
                               # binary with all four engines + mpg123 + r8brain
```
Output: `build/final/SuperLAME-1.0.2.exe` (a stable `superlame-mt.exe` copy is
also written for the test scripts to reference).

**Build-tree dependency to know:** `build_dispatch.sh` reads
`build/phase4-znver3/config.h` as the LAME build config. Keep that file.

---

## 6. Testing

All harnesses are in `tests/` (PowerShell). They take `$env:SUPERLAME_ROOT` (or
default to the repo root) and invoke `build/final/superlame-mt.exe`.

| Script | What it checks |
|--------|----------------|
| `regression.ps1` | **28 cases**: validity (every mode×signal MT-encodes + decodes clean), ST-vs-MT equivalence (≤−40 dB), znver3-vs-SSE2 parity, Xing tag presence, sample-count preservation |
| `odaq.ps1` | Objective ST-vs-MT parity over the ODAQ reference corpus (132 encodes; worst diff must be ≤−40 dB) |
| `flac_conformance.ps1` | IETF CELLAR flac-test-files corpus — 0 crashes; documents known dr_flac limits |
| `fuzz_input.ps1` | Malformed WAV/AIFF/MP3/FLAC input fuzzing (truncation, bit-flip, random+magic) |
| `sqam.ps1` / `scaling.ps1` | EBU SQAM quality + core-scaling benchmarks |

**Current status (v1.0.2):** regression **35/35** (28 original + tiny-input MT,
resample-seam and rate-decision regressions added in 1.0.2), ODAQ **PASS**
(worst −59 dB), FLAC conformance **0 crashes**, fuzz clean.

**Benchmark gotcha:** files < ~300 MB become fully cache-resident and give flat,
misleading scaling timings. Use a large first-touch input (e.g. a 440 MB album).

---

## 7. Known limits & intentional behavior (be honest with users)

- **Scaling plateaus at ~2 workers per file.** The serial repacker is the ceiling.
  For many-core batch work, run **one process per file**, not one big file across
  cores.
- **All-in-RAM.** The whole input, its float expansion, and the output MP3 are
  held in memory. Practical input ~1–2 GB (up to ~4 GB for plain 16-bit). Larger
  is refused with a clear message (RAM preflight), not a crash. True streaming /
  RF64/W64 / 64-bit sample counts would be needed for 8 GB+.
- **`znver4`/`znver5` engines are UNVERIFIED** — built but never executed on real
  Zen 4/5 hardware. Auto-selector only picks them on an AVX-512 CPU; forcing one
  on a CPU that lacks the ISA warns + falls back.
- **FLAC limits (dr_flac):** up to 24-bit only; 32-bit-per-sample FLAC and a few
  unusual streams (mid-stream rate/channel changes, extreme Rice partition orders)
  are refused or partially decoded with a clear message. Standard FLAC (any
  blocksize/predictor order, hi-res to 384 kHz, mono/stereo) fully supported.
- **Input formats:** WAV/AIFF/FLAC only. `.tak`, `.wv` (WavPack, evaluated and
  deliberately skipped), etc. are refused cleanly. (Minor cosmetic: the refusal
  message says "input is not a WAV or AIFF file" and omits FLAC.)
- Single-thread output is bit-identical to stock LAME; multi-thread differs only
  by inaudible seam reconciliation.

---

## 8. Notable bugs fixed (institutional memory)

- **Repacker desync on dense material** (pre-1.0): the 2018 SuperFast snapshot
  predated the BoCA "bit reservoir bigger than a frame" fix (2019). Ported the
  full set of BoCA repacker fixes. Validated against 2 independent real triggers
  (GYBE VBR + SQAM #41 CBR); short synthetic fuzz did NOT reproduce it — needs
  real dense/transient audio.
- **MT dropped ~6.6% of audio when downsampling** (fixed 1.0.1): the r8brain
  pre-resample was skipped for VBR, letting LAME's internal per-frame resampler
  run inside each worker, which breaks the SuperFast "1 input frame = 1 output
  frame" invariant. Fix: probe LAME for its chosen output rate (VBR too),
  pre-resample with r8brain, and pin each worker's `out_samplerate == in`.
- **ID3v2.4 APIC used raw (non-synchsafe) frame size** (fixed 1.0.1): large cover
  art produced a size with high bits set → strict parsers reject the tag and
  desync. Fix: synchsafe frame sizes in v2.4 mode
  (`patches/superlame-id3v24-synchsafe-frame-size.diff`).
- **MT encode of sub-overlap inputs crashed** (fixed 1.0.2): a 1..overlap-1
  frame input made `EncodeFrames`' flush accounting go negative → memmove heap
  corruption. Such inputs now take the degenerate single-worker path.
- **Chunked resampler misaligned seams at non-integer ratios** (fixed 1.0.2):
  each parallel chunk landed sub-sample time-shifted (up to −13 dBFS error at
  44.1→16 kHz; −26 dBFS at 44.1↔48). Integer-family ratios were exact, hiding
  it from the hi-res corpora. Fix: snap each chunk's input start to
  `srcRate/gcd(srcRate,dstRate)`.
- **Mono inputs got the stereo auto-downsample decision** (fixed 1.0.2): the
  LAME rate probe ran before `cfg.channels` was bound (mono CBR 96 was cut to
  32 kHz; LAME keeps mono at 44.1 kHz). See `docs/CHANGELOG.md` for the full
  1.0.2 list including robustness fixes.

---

## 9. Modularity — how easy is it to update a dependency?

| Dependency | Effort | Why |
|------------|--------|-----|
| **dr_flac** | Trivial — replace one header, rebuild | Vendored single header; stable public API |
| **r8brain** | Trivial — replace the folder, rebuild | Header-only; one class used (`CDSPResampler24`) |
| **mpg123** | Easy — rebuild its static lib, repoint `MPG123LIB`/`MPG123INC` | Separate lib, unprefixed symbols, own CMake build. Remember `-ffile-prefix-map` + dynamic-CRT flags. |
| **LAME** | Hard — re-apply 3 patches, rebuild ×4 | Patched + quad symbol-prefix dispatch. **But** LAME is in permanent hibernation; r6531 is effectively final, so there's nothing to update to. |

Adding a LAME API to the dispatch (like `get_out_samplerate` in 1.0.1) means
editing 3 spots in `lame_dispatch`: the struct (`.h`), the `ENGINE_EXTERNS` macro,
and the `ENGINE_INITS` macro — order must match the struct.

---

## 10. Releasing

Follow `RELEASING.md`. In short:
1. Commit + push the exact source; **tag** it (`git tag vX.Y.Z && git push --tags`).
2. Build path-clean (verify `strings SuperLAME-*.exe | grep -iE 'Users|AppData|:\\'`
   is empty).
3. Assemble the ZIP: `SuperLAME-X.Y.Z.exe` + `LICENSE` + `licenses/` +
   `THIRD-PARTY.md` + `README.md` + `SOURCE.txt` (exact repo URL + tag/commit) +
   `CHANGELOG.md`. Add a SHA-256.
4. `gh release create vX.Y.Z <win64.zip> <source.zip> --title ... --notes-file ...`
5. **GPL note:** if the repo is private and you hand someone the binary, you still
   owe them the source — the source ZIP inside the release satisfies this.

Version lives in 3 places (bump together): `SUPERLAME_VERSION` in `main.cpp`,
`FILEVERSION`/`PRODUCTVERSION`/strings in `res/superlame.rc`, and `EXENAME` in
`build/build_final.sh`.

---

## 11. Hand-off specifics

### Repository
- **Public** at <https://github.com/MSIVST/SuperLAME> (owner: `MSIVST`).
- Issues **on**, with a support policy in `CONTRIBUTING.md` (best-effort, no
  warranty). Bug-report + config issue templates in `.github/ISSUE_TEMPLATE/`.
- Latest release: **v1.0.2** (binary + source zips attached).
- `.gitattributes` enforces LF for sources, CRLF for `.ps1`.

### What's in the repo vs. what's fetched
- **In repo:** all frontend `src/`, build scripts, patches, tests, docs, vendored
  `dr_flac/`, license texts.
- **NOT in repo (fetch per `docs/BUILDING.md`):** LAME r6531, mpg123, r8brain, and
  all media/test corpora. `.gitignore` excludes third-party trees, media, and
  build outputs.

### Local build environment (the original dev box)
- Host: **AMD Ryzen 7 5800X3D** (Zen 3, family 0x19 model 0x21, 16 logical cores,
  **no AVX-512**) → auto-selects the `znver3` engine; that's the tested path.
- clang **21.1.6**, target `x86_64-unknown-windows-msvc`. Windows 11.
- Working root: `C:\.Claude_LAMEsf\` holds the fetched deps
  (`lame-r6531/`, `mpg123-1.33.6/`, `r8brain/`, `dr_flac/`), the build tree
  (`build/`), and the repo checkout (`SuperLAME/`).
- `build/` was cleaned to ~1.2 GB; keepers are `dispatch/` (4 engine `.a` libs),
  `final/` (binary), `mpg123-cmake/` (built `mpg123.lib`), `phase4-znver3/`
  (`config.h`), `release/` (the v1.0.1 package). Everything else there is
  regenerable scratch.
- `gh` CLI is authenticated as **MSIVST**.

### To reproduce a full build from a clean machine
1. Install clang/LLVM 21, CMake, NASM.
2. Fetch LAME r6531, mpg123, r8brain next to a checkout of this repo (see
   `docs/BUILDING.md`); apply the `patches/` to LAME.
3. Build mpg123's static lib (CMake port, with the path-map + dynamic-CRT flags).
4. `bash build/build_dispatch.sh && bash build/build_final.sh`.
5. `pwsh tests/regression.ps1` — expect 28/28.

### If you can't verify znver4/znver5
That's expected — flag it honestly (as the README does). On a real Zen 4/5 box,
run the suite and check that `--version` reports the AVX-512 engine and output
matches the znver3 engine within inaudible margins (see the engine-parity test in
`regression.ps1`, section C — extend it to the AVX-512 engines when hardware is
available).

### Good next steps (all optional, none started)
- Add an "Updating a dependency" section to `docs/BUILDING.md` (mpg123 path-map
  flag is the one non-obvious step).
- Extend engine-parity tests to znver4/znver5 once AVX-512 hardware exists.
- Fix the cosmetic "not a WAV or AIFF" message to mention FLAC.
- Consider Intel-tuned tiers (`x86-64-v3`/`v4`) if Intel perf matters — AMD tiers
  would stay unchanged, no regression. Discussed, not requested.
- Streaming / RF64 / W64 for >4 GB inputs (large lift; not needed for typical use).

---

*Report generated for hand-off. For the deep technical rationale behind base
version, quality patch, and architecture choices, see `docs/BUILD_PLAN.md`; for
per-release fixes see `docs/CHANGELOG.md`.*
