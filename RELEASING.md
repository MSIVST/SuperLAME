# Releasing a SuperLAME binary

SuperLAME is GPL v2+. Distributing a binary is allowed (including commercially),
provided the GPL/LGPL/MIT conditions below are met. This checklist makes a
release compliant. See THIRD-PARTY.md for the licensing rationale.

## Before you build

- [ ] Commit and push the exact source you're releasing. The binary's
      "corresponding source" must be the *actual* revision that built it.
- [ ] Tag the release commit, e.g. `git tag v1.0 && git push --tags`, so the
      source snapshot is unambiguous.

## Build (path-clean)

- [ ] Build libmpg123 with `-ffile-prefix-map` (see docs/BUILDING.md) so no local
      build path is baked into the binary's `__FILE__` strings.
- [ ] `bash build/build_dispatch.sh && bash build/build_final.sh`
- [ ] Sanity-check the binary carries no local paths:
      `strings SuperLAME-1.0.exe | grep -iE 'Users|AppData|:\\\\'` should be empty.

## Assemble the release archive

The ZIP must contain, alongside `SuperLAME-1.0.exe`:

- [ ] `LICENSE`                 — GPL v2 (governs the combined work)
- [ ] `licenses/`               — every component's license text + README
- [ ] `THIRD-PARTY.md`          — component table, attribution, patent note
- [ ] `README.md`               — usage
- [ ] `SOURCE.txt`              — one line: the exact source URL + tag/commit,
                                  satisfying the GPL "corresponding source" duty

Example `SOURCE.txt`:

    Complete corresponding source for this binary (GPL v2+):
    https://github.com/MSIVST/SuperLAME  at tag v1.0 (commit <sha>)

## Source availability (GPL section 3)

Pick ONE:

- [ ] **Public repo** — make https://github.com/MSIVST/SuperLAME public (or a
      public release branch/tag). Simplest; the SOURCE.txt link then works for
      anyone. OR
- [ ] **Ship the source** — include a source tarball in the release. OR
- [ ] **Written offer** — include a written offer valid ≥3 years to provide the
      source on request (GPLv2 §3b). Only needed if the repo stays private.

> If the repo is PRIVATE and you give someone the binary, you are still obligated
> to give *them* the source. A private repo they cannot see does not satisfy the
> GPL on its own — use a source tarball or written offer in that case.

## Patents

- [ ] No action needed. MP3 patents expired (~2017); no royalty applies. (See
      THIRD-PARTY.md.)

## Optional but good practice

- [ ] Provide a SHA-256 checksum of the binary.
- [ ] Note the build toolchain (clang version) and target in the release notes.
- [ ] State clearly it is provided "with NO WARRANTY" (GPL §11–12).
