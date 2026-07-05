# Changelog

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
