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

## WavPack (considered, not integrated)

WavPack (`.wv`) input was evaluated. The reference library (libwavpack,
BSD-3-Clause, github.com/dbry/WavPack) has built-in multithreaded decoding and
would fit cleanly, but it is a compiled dependency (a fifth static archive)
rather than a drop-in header, and `.wv` is rare enough that the cost is not yet
justified. FLAC already covers lossless hi-res input. This is a clean future
addition if demand arises.
