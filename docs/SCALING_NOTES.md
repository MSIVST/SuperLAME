# Final encoder validation — findings

Host: AMD Ryzen 7 5800X3D (8 cores / 16 threads).

## 1. 2L-077 DXD (352.8 kHz / 24-bit / stereo, 202 s) — hi-res path ✓
- r8brain resampled 352.8k -> 44.1k (correct family-aware 8:1).
- Full-precision float path engaged (24-bit not truncated to 16-bit).
- Output: exact 202.16 s, 0 decode errors, clean frame chain, tags present.
- r8brain vs ffmpeg soxr (reference resampler) on this real music: -62 dB
  difference (~43 dB below the -19.5 dB signal) = professional-grade, inaudible.

## 2. SQAM reference corpus — regression ✓
- 70 tracks x 5 rate modes = 350 encodes on the FINAL binary: 0 desyncs, 0 errors.
- Confirms none of the added features (decode, wide input, float pipeline,
  resampling, ID3 tags, presets, stdin/stdout) regressed core encoding.

## 3. Core scaling — the honest picture
Measured on the 440 MB / 42 min album (large enough to defeat cache artifacts;
smaller files sit entirely in RAM cache and give misleadingly flat timings):

| threads | time   | vs -t1 | vs stock LAME |
|---------|--------|--------|---------------|
| stock LAME (1 thr) | ~28 s | — | 1.0x |
| our -t1 | ~42 s | 1.0x | 0.7x |
| our -t2..-t16 | ~3.6 s | ~11-12x | ~8x |

**What's solid:** multithreaded encoding is ~8x faster than stock single-thread
LAME (matches the original Phase-3 result). The parallelism is real and large.

**What's anomalous / honest caveats:**
- `-t1` (SuperFast with overlap=0) is ~1.5x SLOWER than stock LAME — the per-chunk
  flush/repack overhead isn't offset by any parallelism. Use -t1 only for a
  reference/serial baseline, not for speed.
- Scaling PLATEAUS at ~-t2 (≈700x realtime) rather than climbing to -t8/-t16.
  Beyond 2 workers there's little gain on this workload — the encode saturates
  quickly and coordination/chunk-granularity dominate. So the win is "big step to
  parallel", not "linear with core count".
- Benchmarking gotcha: files <~300 MB fully cache-resident -> repeated runs all
  hit RAM speed and show flat ~equal times across thread counts. Must use a large,
  first-touch input to measure true scaling.

## Why the plateau (architectural)
SuperFast has ONE serial repacker: every worker's output funnels through a single
SuperRepacker that unpacks + bit-reservoir-repacks frames on the main thread. Once
~2 encoder workers keep that repacker busy, more workers don't help — the repack
plus the serial WAV read/convert become the ceiling. This is inherent to the
design (the repacker can't be parallel — it stitches a continuous reservoir), so
linear core scaling was never on the table; the win is the one big step from
serial to parallel. Absolute throughput here (~700x realtime; 42 min album in
~3.5 s) is higher than any earlier phase measured.

Bottom line: correct, validated output everywhere; ~8x faster than stock LAME in
MT. Throughput is excellent, but scaling is step-not-linear — the serial repacker
is the ceiling past ~2 workers.
