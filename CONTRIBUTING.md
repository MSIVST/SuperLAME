# Contributing & support

SuperLAME is a personal project shared in the hope it's useful. It is provided
**as-is, with no warranty** (see LICENSE) and maintained on a **best-effort,
spare-time basis** — there is no support commitment or response-time guarantee.

## Issues

Issues are welcome, but please help me help you:

- **Bug reports** — include the exact command line, the input file's format
  (sample rate / bit depth / channels — `ffprobe` output is ideal), your CPU
  (so I know which engine tier ran), and the full console output. A small input
  that reproduces the problem is worth a thousand words.
- **Known limits first** — please skim the README "Notes & limits" and
  THIRD-PARTY.md before filing. A few things are known/intentional:
  - Scaling **plateaus at ~2 workers** per file (the repacker is serial) — this
    is by design, not a bug. For many-core throughput, run one process per file.
  - **znver4 / znver5 engines are UNVERIFIED** — they were built but never run
    on Zen 4/5 hardware. If you have such a CPU, reports (works / crashes /
    faster) are especially valuable.
  - FLAC input is **up to 24-bit**; 32-bit-per-sample and a few unusual streams
    are refused with a message (dr_flac limitation), not a crash.
- **Feature requests** — fine to open, but no promises. WavPack input, a
  minimum-phase resample option, and raw-PCM input are already noted as possible
  future work.

## Pull requests

PRs are welcome but may be slow to review. Please:

- Keep changes focused and match the surrounding code style.
- Note that the project is **GPL v2 or later** — contributions are accepted under
  the same terms.
- If you touch the encoder path, run `tests/regression.ps1` (and ideally
  `tests/flac_conformance.ps1`) and mention the result.

## Security / privacy

If you find something sensitive (a crash on crafted input that looks
exploitable, etc.), feel free to open an issue — this is a local CLI tool with a
small attack surface, so normal issues are fine; there's no separate embargoed
channel.
