# Third-party components

SuperLAME combines several projects. Most are **not** vendored here — fetch them
per docs/BUILDING.md. The one exception is **dr_flac**, a single public-domain
header that is vendored in `dr_flac/`. All licenses and notices apply.

| Component | Version | License | Vendored? | Role |
|-----------|---------|---------|-----------|------|
| LAME | trunk r6531 ("3.101 beta 3") | LGPL v2 | no | MP3 encoder core (libmp3lame) |
| mpg123 / libmpg123 | >= 1.26 (1.33.6 used) | LGPL v2.1 | no | MP3 decoder (--decode) |
| r8brain-free-src | latest | MIT | no | high-quality sample-rate conversion |
| dr_flac | v0.13.4 | public domain / MIT-0 | **yes** (`dr_flac/`) | FLAC input decoder |
| fre:ac / BoCA "SuperFast" | 2018 pre3 + BoCA fixes | GPL v2+ | no (ported) | parallel bit-reservoir repacker |
| maikmerten q4 patch | 2024-07-25 | (LAME bug #516) | in `patches/` | CBR/ABR quality fix |

Because the SuperFast-derived repacker is GPL v2+, the **combined work is GPL
v2 or later**. If you distribute a built binary, you must make the complete
corresponding source available under the GPL. (dr_flac's public-domain/MIT-0
terms and r8brain's MIT terms are GPL-compatible.)

- LAME: https://lame.sourceforge.io/
- mpg123: https://www.mpg123.de/
- r8brain-free-src: https://github.com/avaneev/r8brain-free-src
- dr_flac (dr_libs): https://github.com/mackron/dr_libs
- fre:ac / SuperFast: https://www.freac.org/ , https://github.com/enzo1982/BoCA
- LAME bug #516: https://sourceforge.net/p/lame/bugs/516/

## Resampling quality (why r8brain, and which settings)

SuperLAME resamples non-MP3 input rates (e.g. 96/88.2 kHz hi-res) with
**r8brain-free-src**, specifically the `CDSPResampler24` class at its default
high-quality settings:

- **~207 dB stop-band attenuation** (`ReqAtten = 206.91`) — roughly 34-bit-clean,
  far below anything MP3 quantization could preserve.
- **2% transition band** (`ReqTransBand = 2.0`) — flat response to just under
  Nyquist, then a clean roll-off.
- **Linear phase** (`ReqPhase = 0`) — all frequencies delayed equally, so
  transient shape and stereo imaging are preserved (no phase smear).

**Is this "the best"?** For an MP3 encoder, effectively yes — and deliberately
so. By the objective numbers this tier matches reference-grade converters (SoX
"very high", libsoxr, SSRC) and comfortably beats LAME's own internal Blackman-
sinc resampler (which is why we resample *before* the encoder rather than letting
LAME do it per-frame). The resampling is transparent by a ~180 dB margin *before*
MP3 quantization is even applied, so the encoder — not the resampler — is the
quality bottleneck. No realistic resampler upgrade would produce an audible
difference in the final MP3.

**Trade-offs we chose, and the knobs if you disagree:**

- *Linear vs. minimum phase* — linear-phase filters are symmetric and produce
  faint **pre-ringing** before sharp transients (inaudible at 207 dB, and masked
  by MP3 anyway). Mastering purists sometimes prefer minimum phase (no pre-ring,
  some phase shift). r8brain supports it via `ReqPhase`; we use linear because
  transient integrity and zero phase smear are the right defaults for feeding a
  lossy codec.
- *24-bit vs. 16-bit tier* — `CDSPResampler24` is chosen for headroom.
  `CDSPResampler16` (~16-bit-clean) is still inaudible for MP3 and cheaper; it's
  the knob to turn if resampling speed on long hi-res files ever matters.

In short: the current path is intentionally past the point of diminishing returns
for lossy output. If SuperLAME ever grew a **lossless** path (WAV→WAV / FLAC→FLAC
downsampling), it would be worth benchmarking r8brain-24 against SoX-VHQ/libsoxr
for the last fraction of a dB and exposing a minimum-phase option — for MP3 that
effort is below audibility.

## WavPack (considered, not integrated)

WavPack (`.wv`) input was evaluated. The reference library (libwavpack,
BSD-3-Clause, github.com/dbry/WavPack) has built-in multithreaded decoding and
would fit cleanly, but it is a compiled dependency (a fifth static archive)
rather than a drop-in header, and `.wv` is rare enough that the cost is not yet
justified. FLAC already covers lossless hi-res input. This is a clean future
addition if demand arises.
