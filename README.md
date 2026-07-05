# SuperLAME

A multithreaded MP3 encoder built on LAME, using Robert Kausch's **SuperFast**
technique to run many `libmp3lame` instances in parallel and reassemble a single
bit-reservoir-correct MP3 — roughly **8× faster than stock single-threaded LAME**
at the same quality.

It is also a small but fairly complete MP3 **codec**: encode, decode, wide input
format support, high-quality resampling, and ID3 tagging.

## What's in the box

- **Encoder:** LAME 3.101 beta 3 (SVN r6531) with maikmerten's "q4" quality fix
  (LAME bug #516).
- **SuperFast MT:** parallel-instance encoding + bit-reservoir repacker
  (ported from fre:ac/BoCA, GPL), with a self-healing fallback that guarantees
  valid output.
- **CPU dispatch:** one fat binary containing four `libmp3lame` builds —
  `znver5`, `znver4` (AVX-512), `znver3` (AVX2/FMA) and generic `x86-64` (SSE2) —
  chosen at runtime by CPUID. (`znver4`/`znver5` are built but **unverified** —
  no Zen 4/5 host was available to test them; `znver3` and SSE2 are tested.)
  **Intel CPUs are supported:** the engines emit standard AVX-512 / AVX2+FMA+BMI2
  / SSE2 instructions (Intel's too), so an Intel chip runs the matching engine —
  AVX-512 Intel → the AVX-512 engine, mainstream Intel → the AVX2 engine, older →
  SSE2. The Zen 5-tuned engine is AMD-only by design and never selected on Intel.
  The engines are AMD-*tuned*, so on Intel they run correctly but a few percent
  below Intel-specific tuning. (Reasoned from the ISA + dispatch logic; not yet
  benchmarked on a physical Intel box — Intel reports welcome.)
- **Decoder:** MP3 → WAV via built-in libmpg123 (`--decode`).
- **Input:** WAV (PCM 8/16/24/32-bit + 32/64-bit float), AIFF (16/24-bit), and
  FLAC (up to 24-bit, **multithreaded decode**), mono/stereo, any sample rate;
  `-` = stdin/stdout. 24-bit, float and FLAC keep full precision through LAME's
  float pipeline (no early truncation).
- **Resampling:** non-MP3 rates (e.g. 96/88.2 kHz hi-res) resampled to the
  nearest legal rate with r8brain (high quality, linear phase), parallelized
  across cores. `--resample` forces a rate; low bitrates auto-downsample (LAME
  parity).
- **Tags:** ID3v1 + ID3v2 (UTF-8): `--tt/--ta/--tl/--ty/--tc/--tn/--tg`, plus
  album art (`--ti cover.jpg`, JPEG/PNG). FLAC inputs carry their own Vorbis
  comments and embedded cover art through automatically (CLI flags win).
- **Robustness:** graceful out-of-memory refusal, hardened WAV/AIFF/MP3/FLAC
  parsers (input-fuzzed + tested against the IETF FLAC conformance corpus),
  files up to ~4 GB.

## Usage

```
SuperLAME-1.0.2 [options] <infile> [outfile.mp3]

  -b n            CBR bitrate (kbps)
  --abr n         ABR (average) bitrate
  -V n            VBR quality 0..9 (0=best)
  --preset p      medium / standard / extreme / insane, or 8..320 (ABR)
  --resample f    output sample rate (kHz or Hz; default automatic)
  -q n            encoder quality (clamped to >=4 for CBR/ABR: bug #516)
  -m mode         a=auto s=stereo m=mono j=joint
  -t n            worker threads (0 = all cores)
  --decode        decode an MP3 to WAV
  --tt/--ta/...   ID3 tags   |   --ti cover.jpg   album art (JPEG/PNG)
  -v / --quiet    verbose / silent
  --version --about --features --longhelp --license
```

Run `SuperLAME-1.0.2 --longhelp` for the full listing.

## Building

Requires clang/LLVM (for the `-march=znverN` intrinsics and r8brain SIMD — MSVC
won't build the SIMD paths). **dr_flac** is vendored (`dr_flac/`); the other
three sources are fetched (see [docs/BUILDING.md](docs/BUILDING.md)):

- LAME trunk **r6531**
- **mpg123** ≥ 1.26 (built via its CMake port)
- **r8brain-free-src** (header-only)

Then:

```sh
bash build/build_dispatch.sh   # compile the four prefixed libmp3lame engines
bash build/build_final.sh      # compile the frontend + link the final binary
```

See `docs/BUILDING.md` for the exact fetch/patch steps.

## Notes & limits (honest)

- **Speed:** ~8× stock LAME in MT, but scaling **plateaus at ~2 workers** — the
  bit-reservoir repacker is a single serial stage that can't be parallelized, so
  more cores don't help *one* file. For many-core batch encoding, run one process
  per file.
- **Memory:** the whole input, its float expansion, and the output MP3 are held
  in RAM, so practical input is ~1–2 GB (up to ~4 GB for plain 16-bit). Larger
  inputs are refused with a clear message rather than crashing.
- Single-thread output is bit-identical to stock LAME; multi-thread differs only
  by inaudible (<-45 dB) reservoir reconciliation at chunk seams.
- Any output that needs a lower sample rate (high `-V` levels, low CBR/ABR, or
  `--resample`) is resampled up-front with r8brain — LAME's own per-frame
  resampler never runs inside a worker, which keeps MT length exactly equal to
  single-thread.
- **FLAC limits:** the bundled dr_flac decoder handles up to 24-bit; 32-bit-per-
  sample FLAC and a few unusual streams (mid-stream sample-rate/channel changes,
  extreme Rice partition orders) are refused or partially decoded with a clear
  message rather than producing garbage. Standard FLAC (any blocksize, predictor
  order, hi-res up to 384 kHz, mono/stereo) is fully supported.

## Testing

`tests/` contains the validation harnesses:
- `regression.ps1` — validity, ST-vs-MT equivalence, and znver3-vs-SSE2 parity.
- `fuzz_input.ps1` — malformed WAV/AIFF/MP3/FLAC input fuzzing.
- `flac_conformance.ps1` — the IETF CELLAR flac-test-files corpus.
- `odaq.ps1` — objective ST-vs-MT parity over the ODAQ reference corpus.
- `sqam.ps1` / `scaling.ps1` — EBU SQAM quality + core-scaling benchmarks.

See `docs/` for measured results.

## Support

Provided **as-is, best-effort** — no warranty and no support guarantee. Bug
reports and PRs are welcome; please read [CONTRIBUTING.md](CONTRIBUTING.md) first
(it lists known/intentional limits worth checking before filing). If you have a
**Zen 4/5 or an Intel** CPU, reports are especially useful: the AVX-512 engines
are built but unverified, and Intel is supported-by-design but not yet
benchmarked on real hardware.

## License

**GPL v2 or later.** This combines
[LAME](https://lame.sourceforge.io/) (LGPL),
[mpg123](https://www.mpg123.de/) (LGPL),
[r8brain-free-src](https://github.com/avaneev/r8brain-free-src) (MIT),
[dr_flac](https://github.com/mackron/dr_libs) (public domain / MIT-0), and code
derived from [fre:ac / BoCA SuperFast](https://github.com/enzo1982/BoCA)
(GPL v2+); the combined work is GPL. See [LICENSE](LICENSE) and
[THIRD-PARTY.md](THIRD-PARTY.md) for full details and per-component license
texts (in [`licenses/`](licenses/)).
