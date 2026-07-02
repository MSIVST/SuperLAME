# Third-party components

SuperLAME combines several projects. Their sources are **not** vendored in this
repo; fetch them per docs/BUILDING.md. Their licenses and notices apply.

| Component | Version | License | Role |
|-----------|---------|---------|------|
| LAME | trunk r6531 ("3.101 beta 3") | LGPL v2 | MP3 encoder core (libmp3lame) |
| mpg123 / libmpg123 | >= 1.26 (1.33.6 used) | LGPL v2.1 | MP3 decoder |
| r8brain-free-src | latest | MIT | high-quality sample-rate conversion |
| fre:ac / BoCA "SuperFast" | 2018 pre3 + BoCA fixes | GPL v2+ | parallel bit-reservoir repacker (ported) |
| maikmerten q4 patch | 2024-07-25 | (LAME bug #516) | CBR/ABR quality fix, in patches/ |

Because the SuperFast-derived repacker is GPL v2+, the **combined work is GPL
v2 or later**. If you distribute a built binary, you must make the complete
corresponding source available under the GPL.

- LAME: https://lame.sourceforge.io/
- mpg123: https://www.mpg123.de/
- r8brain-free-src: https://github.com/avaneev/r8brain-free-src
- fre:ac / SuperFast: https://www.freac.org/ , https://github.com/enzo1982/BoCA
- LAME bug #516: https://sourceforge.net/p/lame/bugs/516/
