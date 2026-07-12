# FFmpeg interop notes

**Status:** verified 2026-07-12 against the post-1.0.3 tree (CV + `--bench` build) on the
full pipe test matrix. This file is the staging ground for a future README "Using FFmpeg"
section — keep entries short and user-facing.

## When you don't need FFmpeg

SuperLAME reads WAV (PCM 8/16/24/32-bit, 32/64-bit float, EXTENSIBLE), AIFF and FLAC
natively, at any sample rate, from a file or stdin. For those sources, skip FFmpeg
entirely — it's simpler and avoids every quirk below:

```
superlame -V2 -t0 input.flac output.mp3
```

Use FFmpeg only for sources SuperLAME can't read: lossy inputs, exotic containers, or
the audio track of a video file.

## The quirks (each verified)

### 1. `-f wav -` silently downconverts hi-res to 16-bit  ← the big one

FFmpeg's WAV muxer defaults to `pcm_s16le`. Piping a 24-bit or float source with plain
`-f wav -` truncates it to 16-bit **with no warning** — the encode succeeds, the file
plays, and the extra resolution is silently gone (SuperLAME's full-precision float path
never engages). Verified: the piped-24-bit MP3 hashed identical to a 16-bit encode.

Fix — pin the codec to match the source:

```
ffmpeg -i in24.wav  -c:a pcm_s24le -f wav - | superlame -b 320 -t0 - out.mp3
ffmpeg -i in-f32.wav -c:a pcm_f32le -f wav - | superlame -b 320 -t0 - out.mp3
```

With the codec pinned, piped output is byte-identical to reading the file directly
(verified for 16/24-bit, float32, mono, and 96 kHz hi-res through the resample path).

### 2. FFmpeg's `speed=` statistic is meaningless for the pipe

FFmpeg reports `(audio duration) / (its own wall-clock)` and exits as soon as the pipe
is filled — long before SuperLAME finishes its in-RAM encode. Measured on the same run:
FFmpeg claimed `speed=2.7e+03x`; SuperLAME's real throughput was 643x. Never quote
FFmpeg's number for a SuperLAME pipe; use SuperLAME's own stderr line or `--bench`
(whose read stage is labeled `n/a (pipe)` because it would measure FFmpeg, not us).

### 3. Streaming WAV headers lie about sizes — handled, no action needed

FFmpeg can't backfill RIFF/data chunk sizes on a non-seekable pipe, so piped WAV
headers carry bogus sizes. SuperLAME ignores header sizes on stdin and reads to EOF,
clamping lying chunk sizes. Piped and file input produce byte-identical MP3s.

### 4. FLAC: native beats ffmpeg-piped

Feed `.flac` files to SuperLAME directly: the built-in decoder is multithreaded and
feeds the full-precision float path. An `ffmpeg -i x.flac -f wav -` pipe decodes to
16-bit (quirk 1) and yields a different (16-bit-path) MP3. Both are valid and
duration-exact, but native is the better and faster route.

### 5. Windows PowerShell 5.1 corrupts binary pipes

Legacy PowerShell text-mangles binary `>` and `|`. Use PowerShell 7+, cmd.exe, or a
real output filename. (Environmental — not fixable in SuperLAME.)

### 6. The reverse direction just works

SuperLAME's stdout can feed FFmpeg for muxing/transcoding, and the Xing/Info tag is
complete even on stdout (stock LAME can't do that on an unseekable pipe):

```
superlame -V2 -t0 in.wav - | ffmpeg -i - -c:a copy out.mka
```

Verified: null-decode 0 errors, FLAC transcode OK, ffprobe sees exact duration.

## Recommended command patterns

| Task | Command |
|---|---|
| WAV/AIFF/FLAC source | `superlame -V2 -t0 in.flac out.mp3` (no FFmpeg) |
| Lossy/video source | `ffmpeg -i in.mkv -vn -f wav - \| superlame -V2 -t0 - out.mp3` |
| Hi-res via pipe | add `-c:a pcm_s24le` (or `pcm_f32le`) before `-f wav -` |
| Benchmark | `superlame --bench=3 -b 320 -t0 in.wav` — ignore FFmpeg's `speed=` |
| SuperLAME → FFmpeg | `superlame -V2 in.wav - \| ffmpeg -i - ...` |
