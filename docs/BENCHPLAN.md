# BENCHPLAN — a `--bench` mode for SuperLAME

**Status:** IMPLEMENTED 2026-07-12 (unreleased) — recommended shape B+C+items 1&2, plus
speedup candidate 1 (sleep-spin → condition variable, measured 1.8–1.9x at t2–t16 on the
5800X3D). Regression 35/35; MT/ST output byte-identical to v1.0.3. This file remains the
design record; deviations noted inline.
**Scope target:** `superlame-mt/{main.cpp,worker.h,worker.cpp}`, mirrored to `SuperLAME/src/`.

## Motivation

Timing SuperLAME through an ffmpeg pipe (`ffmpeg ... | superlame ... > NUL`) produces
garbage speed factors: ffmpeg reports `(audio duration) / (its own wall-clock)`, but it
exits as soon as it finishes filling the pipe, long before SuperLAME finishes the
in-RAM decode → resample → multi-thread encode → repack → write. The two processes
barely overlap in the part that matters, so ffmpeg's multiplier is noise (and
non-monotonic vs thread count).

SuperLAME already measures the real thing but doesn't expose it well enough for
benchmarking. A `--bench` mode should surface the accurate, in-process timing, run fully
in RAM, optionally write nothing at all, and — for cross-encoder testing against LAME and
Helix — also report **CPU time** so a fair per-core comparison is possible without faking
a multithreaded speedup.

## What the code already provides (the baseline)

- The timer `t0..t1` (`main.cpp:1231` / `1262`) wraps **only encode + repack**. Input
  read, decode, resample, tag render, and the file write are all *outside* it — so a
  "pure encode time" already exists; `--bench` mainly structures and surfaces it.
- `t0` starts *after* the read/decode/resample block (which ends ~line 1178). Those
  stages are currently **untimed** — instrumenting them is the main new value, because
  it explains the "why is `-t 8` slower than default" question (fixed serial floor vs
  parallel encode).
- Output dispatch (1268–1283) is a single clean site to add a null sink.
- `quiet` / `verbose` are existing global bools gating the stderr summary; `--bench`
  slots in as a third reporting mode.
- A FLAC decode timer already exists (985–1004) and can be captured rather than re-added.
- The encode is already isolated in a `try` block (1233–1241), which makes a repeat loop
  straightforward.
- The summary block (1286–1300) currently reports **wall-clock only** (`steady_clock`,
  audio ÷ wall). It has no CPU-time source — that is the one number LAME reports and
  SuperLAME structurally lacks. Adding it is item 1 below.

## Prior art: how LAME and Helix self-report (and why we differ)

Read from `reference/lame-4.0/frontend/timestatus.c` + `lametime.c`:

- **LAME** prints a live dual-clock line — **CPU time** (`clock()/CLOCKS_PER_SEC`,
  `GetCPUTime`) and **REAL/wall** (`ftime`, `GetRealTime`) — updated per status refresh,
  quantized to whole `mm:ss`. Its headline "x" speed (`ts_calc_times`, line 87) is
  `audio_seconds / elapsed`, and the displayed one passes **`proc_time`** — i.e. audio ÷
  **CPU-time**, a *per-core throughput* number. It coincides with wall-clock speed only
  because LAME is single-threaded. There is **no** final "total: X.XX s" line;
  `timestatus_finish` just prints a newline.
- **Helix** (`hmp3.exe`, no source in our tree — from observed behavior): reports a single
  **wall-clock** total at the end (this is why the benchmark's Helix row was trustworthy).
  Also single-threaded, so wall ≈ CPU.
- **SuperLAME** measures **wall-clock** around encode+repack (`main.cpp:1294`, audio ÷
  `secs`). This is the honest multithreaded figure. Copying LAME's audio-÷-CPU formula
  would make a 16-thread encode report ~16× its real speed (a 704x wall encode would print
  "~11000x") — a fabricated number. **So we match LAME's output *shape*, never its
  *metric*.**

**Guardrail (applies everywhere below):** never print a bare "Nx" that is secretly
per-core. Always label `play/wall` (honest throughput) vs `play/cpu` (LAME-comparable
per-core). Both present, both labeled.

## Options (increasing scope)

### Option A — Minimal: machine-readable timing, no output written
`--bench`:
1. Forces the output sink to null (skips `fwrite`/`WriteToFILE` at 1277–1279 entirely —
   not even a NUL memcpy), making `outPath` optional.
2. Prints one parseable line to stderr and nothing else, e.g.:
   ```
   bench: encode=3.40s read=0.62s total=4.02s audio=2394.0s speed=704x threads=16 engine=znver4 bytes=95761920
   ```
**Cost:** ~15 lines. One new bool, one guard at the write site, one `fprintf`, plus making
`outPath` optional at the arg-parse check (line 941).
**Use:** the direct "all in RAM, point to null, test speed" ask, plus a stable grep-able
format across encoders and thread counts.

### Option B — Add stage timers (diagnostic)
Everything in A, plus instrument the currently-untimed stages:
- `t_read` around ReadWav (~1210)
- `t_decode` — capture the existing FLAC timer (985–1004)
- `t_resample` around `ResampleFloat` (1172)
- `t_encode` = existing `t0..t1`
- (optionally split `t_repack` out of the self-heal path, though it lives inside t0..t1)

Output becomes a small table:
```
bench  read 0.62  decode 0.00  resample 0.00  encode 3.40  total 4.02  (16 threads, znver4)
```
**Cost:** ~30–40 lines, a few `steady_clock` pairs. Low risk (pure instrumentation).
**Use:** actually answers "thread scaling stalls past 8 threads" by separating the fixed
serial floor (read + resample) from the parallel encode. High value for benchmarking.

### Option C — Repeat/warmup loop (`--bench=N`)
Everything in B, plus run the **encode** N times on the already-in-RAM PCM and report
min/median/mean. Iterations 2..N are pure encode with zero I/O — ideal for stable numbers
on short clips where startup noise dominates.
```
--bench=5  ->  encode: min 3.38  median 3.41  mean 3.44  max 3.55  (5 runs, warmup discarded)
```
**Cost:** ~50–60 lines. Wrap the encode `try` block (1233–1241) in a loop over a fresh
`MemDriver` each pass; keep the last for optional output.
**Caveat:** the self-heal path mutates `driver` — loop over a temporary; only the final
iteration's driver survives.
**Use:** min-of-N is the honest "fastest achievable" and defeats the scheduler jitter that
corrupted the ffmpeg-based numbers. The rigorous choice for published benchmarks.

## LAME/Helix comparability additions (on top of B+C)

These make SuperLAME's output testable *against* LAME/Helix without deception. Both are
small and both compose with `--bench=N` (each iteration captures a `(wall, cpu)` pair).

### Item 1 — capture CPU time alongside wall time (the substantive win)
Around the same `t0..t1` span, read process CPU time: Windows `GetProcessTimes()`
(kernel+user), portable fallback `std::clock()` (what LAME uses). Report both, plus the
derived parallel-efficiency figure:
```
encode: wall 3.40s  cpu 51.2s  play/wall 704x  play/cpu 46.4x  par-eff 15.1x/16t (94%)
```
`cpu` is directly comparable to LAME's CPU-time; `par-eff` (cpu ÷ wall vs thread count) is
the real, numeric answer to "why doesn't `-t 16` help past 8." ~15 lines (one clock pair +
report fields). This is the highest-value addition — the one metric SuperLAME lacks.

### Item 2 — LAME-compatible labeled line under `--bench`
Emit a stable, self-describing line any LAME-parsing harness can also parse:
```
bench: frames=91234 wall=3.40 cpu=51.2 play/wall=704x play/cpu=46.4x kbps=320 threads=16 engine=znver4
```
`play/cpu` is the apples-to-apples-with-LAME field; `play/wall` is the honest throughput.
Pure formatting over the item-1 numbers. ~10 lines.

### Item 3 (optional, YAGNI) — legacy format presets
`--bench=lame` → `mm:ss / mm:ss` column shape; `--bench=helix` → `frames N / time X.XX /
percent 100`. Only if a real harness is keyed to their exact text. Otherwise the single
`bench:` line (item 2) is better — self-describing and a superset of both. Skip unless
needed.

## Recommended shape: B + C + items 1 & 2

- `--bench`     → null sink, stage table, one instrumented run, dual-clock line (the
  "in-RAM, no file, just speed" ask, answered directly)
- `--bench=N`   → same, but repeat the encode N times → min/median/mean (see rules below)
- Every line carries **both** clocks (`play/wall` + `play/cpu`) so it is comparable to LAME
  and honest about threading.
- machine-readable single-line mode via `--bench` under `--quiet` (or a companion flag)

### `--bench=N` aggregation rules (N ≥ 2)
- **Rank by wall-time; carry CPU along.** Report the full min/median/mean spread for
  **wall**, and report `cpu` + `play/cpu` + `par-eff` for the **best (min-wall) run only** —
  never independently-minned columns (that would describe a run that never happened, the
  same fabrication trap as LAME's per-core "x").
  ```
  bench: runs=5  wall[min 3.38 med 3.41 mean 3.44]  best-run cpu=51.1s play/wall=708x play/cpu=46.9x  par-eff=15.1x/16t (94%)
  ```
- **Discard warmup** (run 1) from the aggregate — cold-cache defeat.
- **Exclude self-heal-fallback iterations** from the aggregate but **print them labeled**
  (`bench: run 3 hit ST fallback, excluded`). A fallback run includes a second
  single-threaded re-encode and is not representative of MT steady state; but it is a real
  event worth surfacing (consistent with the existing always-report-on-stderr policy at
  line 1248).

### `--bench=1` semantics (and bare `--bench`)
- **Bare `--bench` ≡ `--bench=1`.** Repetition is strictly opt-in via `=N≥2`; the default
  is one run so the fast path stays fast and the number after `=` is literally "how many
  encodes."
- **Single-run output, no aggregate wrapper.** min=med=mean would be identical — print the
  item-1/2 line directly, not the `wall[min med mean]` block.
- **It is a cold-start measurement — no warmup discard.** Semantically distinct from
  `=N≥2` (warm steady-state). Do **not** silently warm-up-then-measure-once; that would
  make `=1` two encodes and mismatch the label.
- **Sole-run self-heal fallback is printed-and-labeled, not excluded** (nothing to fall
  back to): print the line tagged `(ST fallback — not representative of MT)`, exit 0.

### Semantics to lock in regardless of scope
- **`--bench` makes `outPath` optional** and defaults to no write. If a real `outPath` *is*
  given, honor it (write once, after timing) — bench-and-keep.
- **`--bench` implies its own reporting**, overrides `--quiet` (a silent benchmark is
  useless), and is distinct from `--verbose`.
- **Report engine + thread count + CPU** on the line (from `SelectLameEngine().name` /
  `CpuDescription()` / `numThreads`) so a pasted result is self-describing — fixes the
  "which engine/CPU produced this number" ambiguity.
- **Keep the self-heal active.** A benchmark that silently benched a single-threaded
  fallback would lie; if it triggers, print `bench: WARNING repacker fell back to ST` (and
  apply the `=N` / `=1` fallback rules above).
- **Exclude tag render** from timing (negligible, not the point); skip it entirely when
  not writing.
- **Always report both clocks, both labeled** (`play/wall`, `play/cpu`) — never a bare
  per-core "x". This is the honesty guardrail that lets the output be compared to LAME
  without misleading anyone about SuperLAME's threading.

### What NOT to do
- No separate `--null-output` flag — fold it into `--bench` (YAGNI; nobody wants a null
  sink except for benchmarking).
- Don't time the input read when reading from a **pipe** — that's the upstream producer's
  speed (ffmpeg), not SuperLAME's, and pollutes the number. Time the read only for file
  inputs; for stdin, label read time `n/a (pipe)`.
- Don't thread `--bench` into the `--decode` path; it's an encoder benchmark.

## Footprint estimate

New global bool + int (`benchMode`, `benchRuns`); arg parse for `--bench[=N]` near
900–940; make `outPath` optional at 941; 3–4 `steady_clock` pairs in the read/resample
region; **one CPU-time pair (`GetProcessTimes`/`std::clock`) around `t0..t1` (item 1)**;
loop wrapper at 1233 with rank-on-wall/carry-CPU aggregation and fallback exclusion; a
reporting block replacing/extending 1286–1300 that prints the dual-clock labeled line
(item 2); a null-sink guard at 1275. Roughly **80–100 lines** for full B+C+items 1&2, all
in `main.cpp`, mirrored to `SuperLAME/src/main.cpp`. Plus a `--longhelp` entry and
regression tests: `--bench`/`--bench=1` exits 0, writes no file, prints a `bench:` line
with both `play/wall` and `play/cpu`; `--bench=3` prints the aggregate `wall[min med mean]`
form. **No engine rebuild** — frontend-only.

## Recommendation

Build **B + C + items 1 & 2**. Rationale:
- **B** (stage split) answers "why doesn't `-t 16` help past 8" — the fixed serial floor.
- **C** (min-of-N) gives publishable rigor and defeats the scheduler jitter that corrupted
  the ffmpeg numbers.
- **Item 1** (CPU time) adds the one metric SuperLAME lacks and LAME has — the basis for
  any legitimate cross-encoder comparison, plus a real parallel-efficiency figure.
- **Item 2** (labeled dual-clock line) makes the output parseable by a LAME-oriented
  harness while staying honest about threading.
Skip **item 3** (legacy format presets) unless a concrete harness demands the exact
LAME/Helix text. Option A alone leaves the thread-scaling question unanswered and has no
CPU-time basis for comparison.

---

# Speedup candidates (the reason `--bench` matters)

The 8→16 thread flattening is the target. **Discipline: measure before optimizing.** The
`--bench` CPU-time + stage split exists precisely to tell us *which* of these is the wall.
The deciding metric is `par-eff = cpu/wall`: if it climbs toward the thread count, the
parallel encode still scales and the ceiling is elsewhere; if it plateaus (e.g. ~8× no
matter the `-t`), an irreducibly serial spine is the wall.

Candidates are ordered by **confidence**, not by size. Only the first is safe to do
without the benchmark; everything below it is gated on measurement.

## Architectural ceiling (BoCA/SuperFast) — the floor the candidates approach but cannot lower

The scaling limit is **inherent to chunked-parallel MP3**, not a SuperLAME or BoCA bug.
Confirmed from the upstream source (`reference/superfast-1.0-pre3/components/lame/`):

- **The MP3 bit reservoir is a serial dependency chain.** MP3 frames are not independent: a
  frame borrows unused bits from earlier frames via `main_data_begin`, so frame N's audio
  can physically live in frames N-1, N-2… The repacker (`repacker.cpp`) recomputes
  `reservoir` / `main_data_offset` / padding / bitrate index frame-by-frame to make the
  reassembled stream reservoir-legal. That state is a **running accumulator** (`reservoir=0`
  init at repacker.cpp:173, then mutated every frame at :357–:439) — it **cannot be
  parallelized**. So no matter the core count, reassembly is a single-threaded walk over
  every frame in the file, cost ∝ file length. This is the Amdahl term.
- **Overlap is wasted encode work that grows with worker count.** Chunk boundaries break the
  reservoir and the psychoacoustic lookahead, so workers encode **overlapping** chunks
  (`overlap` frames) that are then discarded. More workers → more chunks → more seams → more
  overlap frames thrown away. Past some N, added workers add more overlap than they remove
  wall-time.
- **Seam re-encodes are data-dependent extra passes.** When a chunk's reservoir accounting
  doesn't fit at a seam, the worker must `ReEncode` — re-run frames with "dummy frames to
  pressure reservoir" until consistent (upstream worker.cpp:267–296; ours mirrors it). Extra
  work, partly on the serial path, unpredictable.

**Proof it is MP3-specific, not general to SuperFast:** the sibling components in the same
tree — **Opus and Speex — have `worker.cpp` but no `repacker.cpp`.** Their codecs emit
self-contained packets with no cross-frame reservoir, so their SuperFast drivers are
embarrassingly parallel and scale nearly linearly. **Only LAME/MP3 carries a repacker.** The
ceiling is precisely the price of MP3's bit reservoir — the same feature that gives MP3 its
quality-per-bit is what serializes reassembly. (This is also why stock LAME never went
multithreaded, and why SuperFast-class MP3 encoders show strong scaling to ~4–8 threads then
flatten — documented, expected behavior of the technique.)

**Consequence for the candidates below:** they can help SuperLAME *approach* the floor of
"N-core encode + 1-core serial reassembly + overlap tax," but none can *lower* it. Only an
architectural change (candidate 3: overlap the serial repack with encoding) moves it, and
even that overlaps rather than removes the serial cost. What we still don't know is *how
large* that serial fraction is on real hardware — that is exactly what `par-eff = cpu/wall`
from `--bench` measures. The architecture explains *why* a ceiling exists; the benchmark
tells us *where it sits*.

## Why scaling flattens (hypothesis to verify, not proven)

The encode is **not** "N workers, then a serial repack at the end." A single dispatch
thread (`EncodeFrames`, main.cpp:491) walks the buffer, hands `blockSize=128`-frame chunks
to workers round-robin, and **interleaves the reservoir repack inline** on that same thread
(`ProcessResults` → `ProcessPackets`, main.cpp:498; re-encode loop, main.cpp:532). The
bit-reservoir is running state across the whole file, so this repack is serial by
construction and its cost tracks **file length, not thread count**. Architecturally that is
the Amdahl ceiling — but we have **not** isolated that it is the actual 8→16 limiter (the
benchmark numbers so far came through the corrupted ffmpeg pipe, and a ~4 s encode is
dominated by fixed overhead). Treat the serial spine as the prime *suspect*, confirmed only
when `par-eff` plateaus in a clean `--bench` run.

## 1. Sleep-spin → condition variable  (DONE 2026-07-12)

**Measured result (5800X3D, 44-min album, CBR 320, min-of-3 vs --bench=4):** t2 24.25→13.78s,
t4 12.80→7.13s, t8 9.77→5.07s, t16 7.19→3.85s — **1.8–1.9x at every MT thread count**, output
byte-identical. The tax was larger than the 1 ms analysis below predicted because
`sleep_for(1ms)` on Windows rounds up to the ~15.6 ms scheduler tick without a raised timer
resolution. Post-fix `par-eff` (t16 ≈ 61%, encode-CPU rising 25.6s@t2 → 37.7s@t16) now shows
the remaining ceiling is the predicted serial-repack + overlap tax. Original analysis:

Every wait is a **1 ms polling loop**, not an event wait:
- dispatcher waiting on a worker: `while (!IsReady()) sleep_for(1ms)` (main.cpp:494, 520)
- worker waiting for work: `while (!quit && !process) SleepMs(1)` (worker.cpp:81)
- `ReEncode` waiting for completion: `while (process) SleepMs(1)` (worker.cpp:180, 187)

At 320 kbps a 128-frame chunk encodes in well under 1 ms, so each handoff can waste **up to
1 ms of wake latency larger than the work itself**. This tax is invisible at low thread
counts (chunks take >1 ms, the poll is "free") and grows with thread count as handoffs get
shorter and more frequent — i.e. it bites exactly in the 8→16 regime. A `condition_variable`
drops wake latency from ~1 ms to microseconds and is **strictly ≥ a poll** — never worse.
The `std::atomic<bool> process` signals already exist (worker.h:50).

**Confidence:** high — the code proves the waste (fixed 1 ms granularity vs sub-ms tasks)
independent of any measurement. This is the one item safe to implement before benchmarking;
it may *by itself* restore 8→16 scaling, which is why it must land first (it can change
which bottleneck dominates).
**Cost/risk:** ~30–40 lines across `worker.{h,cpp}` + `main.cpp`. Real surgery on the sync
protocol — must be wired carefully around the existing `recursive_mutex` + Lock/Encode
re-entry (worker.h:29), not bolted over it. The `ReEncode` waits (worker.cpp:180,187) are
the same poll and get fixed by the same change (free rider).

## 2. Variable / larger chunks  (SPECULATIVE — gated on measurement)

Intuition: bigger chunks → fewer handoffs and fewer serial `ProcessResults`/`ReEncode`
passes. Plausible, but **do not do before measuring**, for two reasons:
1. If item 1 (sleep-spin) was the real limiter, chunk size does nothing — you'd add risk
   for no gain. Fix the spin and re-measure first.
2. `blockSize=128` (main.cpp:395) interacts with `overlap = 4*1152/frameSize`
   (main.cpp:437) and the reservoir re-encode loop (main.cpp:532). Bigger chunks = fewer
   handoffs (good) but **more work re-done** on a reservoir-accounting miss, and coarser
   load-balancing (a straggler holds up more of the file). Smaller = the opposite. The
   optimum is real but **unknowable without the stage timers**.

**Gate:** (a) land item 1, (b) `--bench` with CPU-time + stage split, (c) *only if*
`par-eff` shows the serial repack is still the wall, tune `blockSize` and re-measure. Blind
tuning violates measure-first.

## 3. Pipeline the repack off the dispatch thread  (SPECULATIVE — big change)

If the benchmark shows the single-threaded serial `ProcessResults` (main.cpp:498) is the
wall even after items 1–2, the deeper fix is moving the reservoir repack off the dispatch
thread so encode-dispatch and repack overlap. Significant architectural change; pure
speculation until measured. Listed for completeness, not planned.

## 4. Input read / buffer allocation  (measure, probably not worth it)

Fixed cost (402 MB read + `MemDriver` growth), but for a ~4 s encode it may be a large
*fraction* of wall time. The stage timers will say. Cheap to check via `--bench` read/total
split; premature to optimize until they show it matters.

## Ordering (why this sequence, not caution for its own sake)

1. **Condition variable** (item 1) — provable, strictly-better, targets the many-short-
   handoffs regime 8→16 lives in.
2. **Build `--bench`** (items 1&2 + stage timers) — measure whether 8→16 improved.
3. **Only then** chunk sizing (item 2) — with `par-eff` numbers, not intuition.

The CV fix may change which bottleneck dominates, so measuring chunk-size effects before it
would measure the wrong system.
