# Changelog

## v1.1.0 — 1.8–1.9x faster multithreading, --bench mode, FLAC hardening

Frontend-only changes; the four engine libraries are untouched. Output is
byte-identical to v1.0.3 for every configuration. Regression suite: 42/42
(35 existing + 7 new --bench tests); SQAM 350 encodes and ODAQ 132 encodes
clean; input fuzzer 920/920 (up from 919/920 — see fix 3).

### 1. Worker handoffs no longer sleep-poll (1.8–1.9x faster MT encodes)

**Symptom:** multithreaded encodes were roughly half the speed they should
have been at every thread count.

**Cause:** every dispatcher<->worker handoff waited in a 1 ms sleep-poll
loop. On Windows, `sleep_for(1ms)` rounds up to the scheduler tick (~15.6 ms
by default), so each of the thousands of chunk handoffs could stall far
longer than the chunk's own encode time.

**Fix:** all six polling loops replaced with condition-variable event waits
(`worker.{h,cpp}`, dispatcher wait sites in `main.cpp`). Wake latency drops
from milliseconds to microseconds. Measured on a 5800X3D (44-minute album,
CBR 320): t2 24.3s -> 13.8s, t4 12.8s -> 7.1s, t8 9.8s -> 5.1s,
t16 7.2s -> 3.9s. Output verified byte-identical to v1.0.3.

### 2. New `--bench[=N]` benchmark mode

Encodes fully in RAM and writes nothing (an output file is optional —
giving one keeps the result, byte-identical to a normal encode). Reports a
stage split (read / resample / encode) and a parseable line carrying both
clocks, always labeled: `play/wall` (honest multithreaded throughput),
`play/cpu` (per-core figure comparable to stock LAME's "x"), and `par-eff`
(cpu/wall parallel efficiency). `--bench=N` repeats the encode on the
in-RAM audio and reports wall min/median/mean (run 1 is warmup, discarded;
CPU figures come from the fastest run). Pipe input labels read time
`n/a (pipe)` — that stage would measure the upstream producer. Design
record: `docs/BENCHPLAN.md`.

### 3. FLAC allocation bomb (fuzzer-found hang)

**Symptom:** a bit-flipped FLAC (fuzzer case) hung the process; present in
all previous releases.

**Cause:** the decoder sized its output buffer from STREAMINFO's
`totalPCMFrameCount` without sanity-checking it. A corrupt ~20 KB file
claiming 3.7 billion frames demanded a ~30 GB zeroed allocation, which
thrashed the pagefile instead of failing (the RAM preflight estimates from
file size, so it could not catch this).

**Fix:** real FLAC cannot store more than ~420 samples per byte even for
constant silence, so a header claiming more than 512x the file size is
treated as an unknown length and decoding takes the streaming path, which
sizes buffers by what actually decodes. Legit files are unaffected
(verified against ODAQ, SQAM and a maximum-density silence file).

### 4. Docs and tests

`docs/FFMPEG-NOTES.md` collects verified FFmpeg interop quirks (README
candidates), the biggest being that `-f wav -` silently downconverts hi-res
sources to 16-bit unless `-c:a pcm_s24le`/`pcm_f32le` is pinned. The
regression suite gains a --bench section (I); `tests/regression.ps1` is now
42 checks.

## v1.0.3 — stdout/piping fixes and honest CPU reporting

A piping audit prompted by user reports. Frontend-only changes; the four engine
libraries are untouched. Regression suite: 35/35.

### 1. Encoding to stdout gave no status output

**Symptom:** `superlame -b 320 in.wav - > out.mp3` ran completely silent — no
banner, no progress, no summary. Stock LAME shows its console output in this
case.

**Cause:** all run-time status text was written to *stdout*, so when the MP3
stream itself owned stdout the frontend had to force `--quiet` to keep text
bytes out of the file.

**Fix:** run-time status output (banner, config, progress line, summaries,
decode report) now goes to **stderr**, exactly like stock LAME, and the forced
quiet is gone. Requested help/info commands (`--help`, `--version`, `--about`,
`--features`, `--license`, `--longhelp`) stay on stdout so they can be piped
into a pager. The missing-argument error path prints usage to stderr (also the
LAME convention).

**Insight:** because SuperLAME assembles the whole MP3 in memory before
writing, the Xing/Info tag on a stdout stream is *complete* (frame count, TOC,
CRC). Stock LAME cannot seek stdout, so its piped output carries an
unfinalized tag. Piped SuperLAME output is byte-identical to file output.

### 2. `--decode` from a piped stdin silently truncated the output

**Symptom:** `type in.mp3 | superlame --decode - out.wav` produced a
fragment (~one frame, 0.03 s of a 3 s file) and **exited 0**. The
redirect form (`< in.mp3`, seekable) was always correct.

**Cause:** the MP3 reader sized its buffer by seeking to end-of-file.
On a Windows pipe that seek can *appear to succeed* with a bogus small
size — worse than failing, because the short read then looks like a
complete file. (The encode-side reader already special-cased stdin;
the decode-side reader predated it and never got the guard.)

**Fix:** stdin is now always read to EOF, never sized by seek — same
pattern for audio input, `--decode` input, and `--ti` album art (a piped
cover now works and produces a byte-identical MP3; art and audio both on
stdin fails with a clear error). Piped decode output is byte-identical
to the seekable cases.

### 3. Engine banner didn't identify the CPU

**Symptom:** an AVX-512 machine reported the engine as
`znver4 (AVX-512, unverified)` — a static label that neither names the
actual CPU nor explains the choice; `--longhelp` implied *any* AVX-512
CPU maps to znver4.

**Fix:** the probe now exposes a human CPU description — vendor, Zen
generation and usable ISA (`AMD Zen 4 (family 0x19, AVX-512)`,
`Intel (AVX-512)`, …). `--version` prints it as a `cpu :` line next to
`active :`; verbose encodes print `Engine: … CPU: …`. The engine display
names drop the "unverified" clutter, and the `--longhelp` table now states
the real mapping (Zen 5 → znver5, Zen 4 + other AVX-512 CPUs → znver4,
Zen 1-3 / any AVX2+FMA+BMI2 → znver3, else SSE2). Within AMD family 0x19,
usable AVX-512 distinguishes Zen 4 from Zen 3, so the description always
matches what the dispatcher can actually use.

Note: engine *selection* was already correct in all released versions
(an identified Zen 5 picks znver5); this release fixes what is *reported*.

### 4. AVX-512 usability is now verified through XCR0 (`xgetbv`)

**Latent bug:** the CPUID probe trusted the AVX/AVX-512 feature bits with
only an OSXSAVE check, assuming the OS saves YMM/ZMM register state. Under
a hypervisor or OS that exposes the CPU bits without enabling the state,
the AVX-512 engines could be selected and fault on first use.

**Fix:** the probe reads XCR0 via `xgetbv` and requires the XMM+YMM state
bits (0x06) for the AVX2 tier and additionally opmask+ZMM (0xE0) for the
AVX-512 tiers.

### Docs (same pass)

- `--longhelp` gained a `STDIN / STDOUT ("-")` section; README's input
  bullet expanded: status output is on stderr, output is assembled in RAM
  (complete Xing tag on stdout; no streaming), and **legacy Windows
  PowerShell 5.x corrupts binary `>` redirection** — use PowerShell 7+,
  cmd.exe, or a filename there.

**Verified:** regression suite 35/35; full pipe matrix (WAV/AIFF/FLAC piped
in, MP3 to stdout, decode from pipe/redirect/file, two-process
encode-to-decode chain, piped 96 kHz hi-res through the parallel resampler at
`-t1`/`-t16`, piped album art) — every pipe path byte-identical to its file
equivalent; `--quiet` behavior unchanged.

## v1.0.2 — crash, resampler-seam and rate-decision fixes (source audit 2026-07)

A source audit found three user-visible bugs (all reproduced, all fixed) plus
several robustness gaps. Frontend-only changes; the four engine libraries are
untouched. Regression suite extended from 28 to 35 cases (all pass).

### 1. Multithreaded encode of very short inputs crashed (heap corruption)

**Symptom:** `-t ≥ 2` on an input holding 1..3 full MPEG-1 frames (1152–4607
samples @ 44.1/48/32 kHz; up to 7×576 at MPEG-2 rates) segfaulted or aborted.
Single-thread was fine.

**Cause:** `EncodeFrames`' flush accounting `framesProcessed += framesToProcess
- overlap` goes negative when the whole input is shorter than the chunk
overlap; the buffer-compaction memmove then runs with a wrapped `size_t`.
The existing degenerate-input guard only covered `framesToProcess <= 0`
(inherited from upstream BoCA, partially guarded in the port).

**Fix:** flush inputs with `framesToProcess < overlap` now take the existing
single-worker degenerate path. Output for such inputs decodes to PCM
bit-identical with the single-thread encode.

### 2. Chunked resampler misaligned seams at non-integer rate ratios

**Symptom:** any resample whose ratio isn't a clean integer family conversion
(`--resample 48` from 44.1 kHz, `--resample 44.1` from 48 kHz, low-bitrate
auto-downsamples like 44.1→32/24/16/12/8 kHz) produced audio with every
parallel chunk time-shifted by a sub-sample offset — up to **−26 dBFS** error
vs the correct output (−12.7 dBFS at 44.1→16 kHz), with a waveform
discontinuity at every chunk seam. Integer-family conversions (88.2→44.1,
96→48, DXD 8:1 — everything in the test corpora) were exact, which is how it
went unnoticed.

**Cause:** each thread's output-placement `padOutStart = i0*dstRate/srcRate`
truncates unless `i0*dstRate` is divisible by `srcRate`.

**Fix:** each chunk's padded input start is snapped to the exact-alignment
grid (`i0 -= i0 % (srcRate/gcd(srcRate,dstRate))`). Measured worst error after
the fix: **−297 dBFS** (double-precision floor) on all previously-broken
ratios; family ratios unchanged.

### 3. Mono inputs got the stereo auto-downsample decision

**Symptom:** a mono file at e.g. CBR 96 was resampled to 32 kHz although LAME
keeps mono at 44.1 kHz at that bitrate (mono has twice the per-channel budget).
Needless bandwidth loss — and the pointless 44.1→32 conversion also hit bug 2.

**Cause:** `ProbeLameOutRate` ran before `cfg.channels` was bound to the real
input, so the probe always asked LAME with the default 2 channels.

**Fix:** the channel count is bound before the rate decision. Verified: mono
CBR 96 stays 44.1 kHz, stereo CBR 96 still downsamples to 32 kHz (matching
stock LAME in both cases).

### Robustness/minor (same audit)

- `~SuperWorker` now stops and joins its thread; previously an exception
  unwinding the encoder (e.g. out-of-memory) hit `std::terminate` via deleting
  a joinable `std::thread`, aborting before the error message printed. The
  single-thread self-heal re-encode is now also covered by the OOM handler.
- `SuperRepacker::UpdateInfoTag` no longer writes past the Xing frame vector
  for small-frame layouts (MPEG-1 stereo CBR ≤ 56 kbps); fields that don't fit
  are dropped deterministically (emitted bytes unchanged).
- `--decode` now reports write failures (disk full) instead of exiting 0, and
  refuses >4 GB WAV output instead of writing a wrapped RIFF header; the
  encoder path checks the final flush/close too.
- Inputs with more than 2^31 samples are refused with a clear message instead
  of aborting in `vector::resize`.
- Verbose banner shows the true effective `qval=4` for default CBR/ABR (the
  q4 clamp applies to LAME's default quality as well).
- Fixed a non-Windows compile break (cover-art `open_utf8` call missing its
  `#ifdef _WIN32` wide-mode guard).

**Verified:** regression suite **35/35** (28 original + 4 tiny-input MT cases,
1 non-integer-ratio resample seam check, 2 channel-aware rate-decision checks);
byte-identical output on unaffected paths (normal-length MT encodes, ST
encodes, integer-family resamples).

## v1.0.1 — MP3 correctness fixes (multithreaded downsampling + cover art)

Two independent bugs were found via a FLAC → MP3 encode that played "off-time"
and skipped in some players. Both are fixed; the frontend and the four engines
were rebuilt.

### 1. Multithreaded encoding dropped ~6.6% of audio whenever LAME downsampled

**Symptom:** an MT (`-t >1`) encode was noticeably shorter than the source
(e.g. a 5:44 track came out 5:21) — but only when the output sample rate differed
from the input. That happens on the high VBR levels (`-V7/-V8/-V9` auto-drop to
32/24/22.05 kHz) and at low CBR/ABR bitrates. Single-thread was always correct.

**Cause:** the SuperFast overlap accounting assumes **1 input frame == 1 output
frame** across chunk seams. SuperLAME already resamples non-MP3 rates with
r8brain *before* chunking so this holds — but the pre-resample step was skipped
for VBR (`targetKbps == 0` short-circuited the rate logic), letting LAME's
**internal** per-frame resampler run *inside each worker*. Once input and output
frame counts diverge, each chunk silently loses `overlap` output frames
(~`overlap/blockSize` ≈ 6.25%), accumulating to seconds of missing audio.

**Fix:**
- The frontend now **probes LAME** (`ProbeLameOutRate`) for the exact output rate
  it would choose for the given CBR/ABR/VBR config, and pre-resamples the whole
  stream to that rate with r8brain — for VBR as well as CBR/ABR. This inherits
  LAME's real downsample decision instead of approximating it.
- Each worker now sets `out_samplerate == in_samplerate`, so LAME's internal
  resampler can **never** run inside a worker (a hard guarantee of the 1:1
  invariant even if a future rate mismatch slips through).
- Added `lame_set_out_samplerate` / `lame_get_out_samplerate` to the engine
  dispatch table.

**Verified:** ST vs MT decode to identical length and byte-identical audio at
`-V7/-V8/-V9`; `-V2` (no resample) unchanged; regression suite 28/28.

### 2. ID3v2.4 cover art (APIC) used a non-synchsafe frame size

**Symptom:** files with embedded cover art loaded wrong in strict players; the
whole ID3 tag could be rejected and the decoder desynced into the JPEG bytes.

**Cause:** SuperLAME emits ID3v2.4 tags, but stock LAME writes per-frame size
fields as raw 32-bit big-endian values (valid only in ID3v2.3). ID3v2.4 requires
28-bit **synchsafe** sizes. Text frames (< 128 bytes) were unaffected, but a
large APIC (e.g. a 736 KB JPEG) produced a size with high bits set — which
libmpg123 flags as `non-syncsafe size of APIC frame`.

**Fix:** frame-size fields (comment, text, WXXX, APIC) are now written synchsafe
when the tag is v2.4, raw when v2.3. See
`patches/superlame-id3v24-synchsafe-frame-size.diff`.

**Verified:** APIC size now `00 2D 7F 72` (753650, no high bits); libmpg123
decodes the tagged file cleanly at full length.
