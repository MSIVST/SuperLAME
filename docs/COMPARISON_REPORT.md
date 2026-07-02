# superlame-mt vs stock LAME — comparison report

**Test corpus:** 4 full albums in `ALBUMS_WAV` (16-bit / 44.1 kHz stereo, ~3.3 h total).
**Host:** AMD Ryzen 7 5800X3D (8C / 16T, Zen 3).
**Date:** 2026-06-29.

- **superlame-mt** — this build: LAME r6531 + libmpg123 + q4 patch + SuperFast multithreading (znver3 engine), all 16 threads (`-t0`).
- **stock LAME** — the *same* patched r6531 encoder core, single-threaded (`build/phase2-build/lame.exe`).

Both share an identical encoder core, so this is a clean apples-to-apples test: it
isolates exactly what SuperFast adds — **speed at equal quality** — with nothing else changing.

---

## Results

| Album | Setting | Audio | superlame-mt | stock LAME | Speedup | Size Δ | decode errs (mt / lame) | audio diff vs LAME |
|---|---|---|---|---|---|---|---|---|
| American Head Charge — The Feeding | VBR -V2 | 41:46 | 3.48 s (720× rt) | 21.08 s (119× rt) | **6.05×** | −0.01% | 0 / 0 | −73.8 dB |
| American Head Charge — The Feeding | CBR 320 | 41:46 | 11.86 s (211× rt) | 23.97 s (105× rt) | **2.02×** | −0.09% | 0 / 0 | −45.6 dB |
| Elliot Goldenthal — Aliens 3 | VBR -V2 | 50:04 | 4.19 s (718× rt) | 24.46 s (123× rt) | **5.84×** | −0.19% | 0 / 0 | −87.2 dB |
| Elliot Goldenthal — Aliens 3 | CBR 320 | 50:04 | 6.58 s (457× rt) | 30.25 s (99× rt) | **4.60×** | −0.09% | 0 / 0 | −68.6 dB |
| Godspeed You Black Emperor! — f#a#∞ | VBR -V2 | 63:29 | 4.56 s (836× rt) | 28.13 s (135× rt) | **6.17×** | −0.12% | 0 / 0 ✓ | (now clean — see §3) |
| Godspeed You Black Emperor! — f#a#∞ | CBR 320 | 63:29 | 7.53 s (506× rt) | 35.59 s (107× rt) | **4.73×** | −0.09% | 0 / 0 | −63.8 dB |
| Static-X — Wisconsin Death Trip | VBR -V2 | 43:56 | 3.67 s (719× rt) | 21.71 s (121× rt) | **5.92×** | −0.02% | 0 / 0 | −79.4 dB |
| Static-X — Wisconsin Death Trip | CBR 320 | 43:56 | 17.19 s (153× rt) | 25.60 s (103× rt) | **1.49×** | −0.09% | 0 / 0 | −48.3 dB |

*"rt" = realtime factor (audio seconds encoded per wall-clock second). "audio diff" = RMS of the
sample-by-sample difference between the two decoded outputs; the music sits around −8 to −12 dB,
so e.g. −73 dB means the difference is ~60 dB below the music — inaudible.*

---

## Findings

### 1. Speed — the headline

For **VBR -V2** (the recommended setting), superlame-mt is **5.8×–6.2× faster** than
single-threaded LAME on every album — a 50-minute album encodes in ~4 seconds (700–840×
realtime). This is the SuperFast multithreading scaling close to the core count.

**CBR is slower than VBR** in the multithreaded path (1.5×–4.7×). This is expected and inherent:
CBR forces every frame to a fixed size, so the bit-reservoir repacker does far more work
reconciling chunk boundaries (more re-encode retries). VBR frames size themselves naturally and
stitch together cheaply. **For the biggest speedup, use VBR.** The two slowest CBR cases (Static-X
1.49×, AHC 2.02×) are dense, loud, transient-heavy metal — the hardest content for reservoir packing.

### 2. Quality — equivalent

Output sizes match stock LAME to within **0.2%** in every case (same encoder, same bit allocation).
For the 7 clean rows, the decoded-audio difference vs stock LAME is **−45 to −87 dB** — 35–75 dB
below the music itself. Inaudible; this is the expected sub-frame reservoir-reconciliation noise of
parallel encoding, **not** quality loss. (Single-threaded superlame-mt output is bit-identical to
stock LAME, verified separately.)

### 3. ⚠ One real defect — Godspeed You! Black Emperor, VBR, high thread count

The GYBE VBR -V2 row shows **2 malformed frames** (ffmpeg: "Header missing") and a correspondingly
inflated −24.8 dB difference. To be clear: that −24.8 dB is **not** a 24 dB quality drop — the 2 bad
frames desync the decoder against the reference, which is what raises the number.

Investigated and characterized:

- **Deterministic, not a race:** identical 2 errors and identical file size on every run, at every
  thread count ≥ 4.
- **Multithread-only:** clean at `-t1` (repacker bypassed) and `-t2`; appears at `-t4` and above.
- **Content-specific:** the other 3 albums are perfectly clean at full `-t0`. Stock LAME on the same
  GYBE file is clean. GYBE's *f#a#∞* has extreme silence→loud dynamics — the worst case for the bit
  reservoir.
- **VBR-only here:** GYBE at **CBR 320 is clean** (0 errors).

**Root cause (found and FIXED at source).** The bug was a 72-byte main-data desync in the repacker's
`IncreaseReservoir`, triggered when a frame's main data is larger than one frame (GYBE's last good
frame carried 1292 bytes of main data in a 1044-byte frame). The port was based on the 2018
`superfast-1.0-pre3` snapshot — which **predates five years of bug fixes** that fre:ac/BoCA applied
afterward. Diffing against the current BoCA source (`enzo1982/BoCA`) revealed the relevant commits:

- *2019-12-26* "Handle bit reservoir sizes bigger than a frame when resizing frames" — **the fix:**
  clamp the trailing write to `min(frameSpace, prevReservoir)` so oversized main data can't overrun
  the next frame header.
- *2019-12-22* "Make frame synchronization in IncreaseReservoir more robust" — validate candidate
  frames against a reference (mode/header/samplerate) and require a real preceding frame, so the
  resizer can't latch onto a false sync inside main data.
- Plus the recursion-depth limit, the larger read window, read-before-first-frame guard, and keeping
  LAME's frame padding.

These were ported in. Verified:

- **GYBE -V2 -t0 now encodes clean directly in multithreaded mode** (`145801 frames, 0 decode
  errors`) at **full speed (~5 s)** — no fallback needed.
- All 4 albums × VBR/CBR at `-t0`: every one `direct` and clean.
- Full 28-test regression passes; ST and MT output are now bit-identical on the complex signal.

**Validation of the fix (the strong evidence).** To prove the fix and not just trust it, the change
was reverted to produce a known-buggy binary, then both binaries were run against:

- **The EBU SQAM corpus** — 70 standard codec-stress reference tracks (transients, solo instruments,
  extreme dynamics), × 5 modes = 350 encodes each. The buggy binary desynced on **track 41 at CBR
  128** — a *second, independent* reproducer, in a *different* mode than GYBE. The fixed binary
  processed all 350 cleanly (0 desyncs). Confirmed directly: track 41 / CBR 128 on the fixed binary
  scans clean (2529 frames, 0 decode errors).
- **GYBE** — the buggy binary desyncs, the fixed binary is clean.

So the fix is confirmed against two independent real-world triggers across both VBR and CBR, and the
combined reference corpus (4 albums + 70 SQAM tracks × 5 modes) shows **0 desyncs on the fixed
build**. (Short synthetic fuzz signals did *not* reproduce the bug on either binary — the trigger
needs real, dense, transient-rich material, which is why the SQAM corpus was the decisive test.)

A self-healing fallback (verify the frame chain; re-encode single-threaded if ever broken) is kept
as a defensive safety net, but no longer triggers on any tested input.

### 4. A separate bug found and fixed during this test

The first comparison run had GYBE "encoding" in 14 ms — because the `∞` in the filename was mangled
by Windows' ANSI argument handling, so the file never opened. Fixed (UTF-16 command line +
`_wfopen`); the `∞` filename now encodes correctly. The numbers above are from the fixed build.

---

## Bottom line

For ordinary material, superlame-mt delivers **stock-LAME quality at ~6× the speed** in VBR (the
recommended mode), with output sizes within 0.2% and inaudible differences. CBR works but with a
smaller speedup. The repacker edge case found on GYBE was traced to missing post-2018 fre:ac fixes
and **fixed at the source** — every album now encodes clean and at full multithreaded speed, with a
self-healing fallback retained purely as a belt-and-braces safety net.
