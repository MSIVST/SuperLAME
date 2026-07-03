# Changelog

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
