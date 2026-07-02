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
- **CPU dispatch:** one binary containing a `znver3` (AVX2/FMA) build and a
  generic `x86-64` (SSE2) build, chosen at runtime by CPUID.
- **Decoder:** MP3 → WAV via built-in libmpg123 (`--decode`).
- **Input:** WAV (PCM 8/16/24/32-bit + 32/64-bit float) and AIFF (16/24-bit),
  mono/stereo, any sample rate; `-` = stdin/stdout. 24-bit and float keep full
  precision through LAME's float pipeline (no early truncation).
- **Resampling:** non-MP3 rates (e.g. 96/88.2 kHz hi-res) resampled to the
  nearest legal rate with r8brain (high quality, linear phase), parallelized
  across cores. `--resample` forces a rate; low bitrates auto-downsample (LAME
  parity).
- **Tags:** ID3v1 + ID3v2 (UTF-8): `--tt/--ta/--tl/--ty/--tc/--tn/--tg`.
- **Robustness:** graceful out-of-memory refusal, hardened WAV/AIFF/MP3 parsers
  (input-fuzzed), files up to ~4 GB.

## Usage

```
superlame-mt [options] <infile> [outfile.mp3]

  -b n            CBR bitrate (kbps)
  --abr n         ABR (average) bitrate
  -V n            VBR quality 0..9 (0=best)
  --preset p      medium / standard / extreme / insane, or 8..320 (ABR)
  --resample f    output sample rate (kHz or Hz; default automatic)
  -q n            encoder quality (clamped to >=4 for CBR/ABR: bug #516)
  -m mode         a=auto s=stereo m=mono j=joint
  -t n            worker threads (0 = all cores)
  --decode        decode an MP3 to WAV
  --tt/--ta/...   ID3 tags
  -v / --quiet    verbose / silent
  --version --about --features --longhelp --license
```

Run `superlame-mt --longhelp` for the full listing.

## Building

Requires clang/LLVM (for `-march=znver3` and the r8brain intrinsics — MSVC won't
build the SIMD paths) plus the three external sources (not vendored here — see
[docs/BUILDING.md](docs/BUILDING.md)):

- LAME trunk **r6531**
- **mpg123** ≥ 1.26 (built via its CMake port)
- **r8brain-free-src** (header-only)

Then:

```sh
bash build/build_dispatch.sh   # compile the two prefixed libmp3lame engines
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

## Testing

`tests/` contains the validation harnesses: `sqam.ps1` (EBU SQAM reference
corpus), `fuzz_input.ps1` (malformed-input fuzzing of the parsers), and
`scaling.ps1` (core-scaling benchmark). See `docs/` for measured results.

## License

**GPL v2 or later.** This combines LAME (LGPL), mpg123 (LGPL), r8brain (MIT), and
code derived from fre:ac/BoCA SuperFast (GPL v2+); the combined work is GPL. See
[LICENSE](LICENSE) and [THIRD-PARTY.md](THIRD-PARTY.md).
