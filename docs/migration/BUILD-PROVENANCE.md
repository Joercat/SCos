# Build provenance: every image says which commit it is

2026-09-22 — native x64 UEFI, still unnumbered.

## Why this exists

A USB stick can carry an image that matches no commit, and for a long time nothing in the running system
could say so. That is not a hypothetical: a physical panel was reported as "the newer features are not
implemented at all" from a stick built inside a sandbox out of a workspace state that was never
committed. The report contained `Tables: 1019 id rules bind a driver; 1298 further ids name a chip...`
while every committed build of this repository generates **1296** naming rows
(`python3 tools/research/gen_gpu_tables.py --registry-only --check` prints the number, and
`tools/tests/test_buildinfo.py` asserts it). The image being judged and the source being discussed were
two different builds, and neither side could prove it — the argument could only be settled by reflashing.

So "which build is this" became a feature of the OS rather than a property of the release notes.

## The mechanism

```
git state ──► tools/buildinfo.py ──► build/scosbuild.h ──► kernel/desktop/buildinfo.c ──► every surface
                                            │
                                            └────────────► tools/makedisk.py ──► \SCOS\BUILD.TXT on the ESP
                                                                              └► build/build.json (record)
```

* `tools/buildinfo.py` reads `git rev-parse --short=12 HEAD`, the branch, whether `git status --porcelain`
  is empty, and the current UTC time, and writes `build/scosbuild.h`. The build time is only refreshed
  when the tree identity moves, so an unchanged repository does not look like a source edit.
* `kernel/desktop/buildinfo.c` is the kernel's only reader of that header. Its Makefile rule is
  `FORCE`-bound, so it is recompiled on every `make`: a commit can never lag behind the running kernel.
  Because it is the only reader, the relink is the whole cost, and the image bytes stay identical when
  the stamp did not change (verified: two consecutive `make` runs produce one sha256).
* `scos.h` includes the header through `__has_include`, with `unstamped` fallbacks, so the host-side
  harnesses in `tools/tests` that compile driver files with their own flags keep working without a `build/`
  directory. A kernel compiled without the stamp still boots — it just says `unstamped`.
* `tools/makedisk.py` generates the record from the packed files and **refuses to assemble an image whose
  kernel does not contain the commit and date the record claims** (`refusing to pack this image: the build
  stamp says commit X built Y, but the kernel binary does not contain that stamp`). The record therefore
  cannot be written from what the build hoped for; it is written from the bytes, or the build fails.

## Where to read it

| Where | What it says |
| --- | --- |
| terminal `version` | `SCos unnumbered x64 development - build x64-dev from <commit> <tree>, <date>`, plus `[WARNING: built with uncommitted changes]` |
| terminal `graphics` | a `Build: <commit> <tree>, <date>` line, ending `matches the committed source` or `UNCOMMITTED tree - not evidence about any commit` |
| About panel | the stamp under the title, so a screenshot carries it |
| TTY (`F1`-`F6`, `version`) | the same stamp, readable when the window manager never came up |
| serial log | `build: <commit> <tree>, <date> branch <branch>`, logged before the desktop exists |
| `\SCOS\BUILD.TXT` on the ESP | commit, branch, tree state, build time and the sha256 of the packed kernel and of every driver module — readable from any OS without booting |
| `dist/scos.img.build.json` | the same values as JSON, committed beside the image |

`TREE` is `modified` whenever the build ran on top of uncommitted changes. Treat such an image as an
experiment in both directions: a feature it lacks, or has, says nothing about any commit.

## Shipping, so the delivered image can be stamped `clean`

The stamp names the commit the *sources* came from, so the artifacts of a commit cannot be part of it.
A delivery is therefore two commits, and `make dist` exists to keep the three artifacts together:

```sh
make && make dist            # dist/scos.img, dist/scos.img.build.json, dist/scos.img.sha256
git add -f dist/scos.img && git add -A && git commit    # code; tree now clean
make                       # same code, clean tree: the stamp flips to <that commit> clean
make dist && git add -f dist/scos.img && git commit -m "dist: build record for <commit>"
```

Because of that pattern the record legitimately names a commit *behind* HEAD, which is also what a
forgotten `make dist` looks like from the outside.  The two are told apart mechanically:
`tools/check_delivery.py`, run as the last step of `make dist`, reads the record, then reads the packed
bytes back out of the image's own FAT32 ESP and hashes them - `KERNEL.ELF` and every `.MOD` - so a record
refreshed without repacking (or the reverse) fails.  It also refuses an uncommitted tree state, a commit
the repository does not contain, and any commit that touched `kernel/`, `boot/`, `drivers/`,
`tools/makedisk.py`, the table generator or the Makefile since the record was made - while docs, tests and
host-side tools may move freely without invalidating an image.  `tools/tests/test_delivery.py` breaks each
of those states on a scratch copy to prove the check fires.  A stick, rather than a repository, is dated by
`tools/verify-stick.py`, which reads the same record from the device without mounting or booting it.

The second commit changes no source, so its kernel still carries the first commit's id — which is the
commit that contains the code being judged. `dist/scos.img.sha256` is written as
`<hash>  dist/scos.img` by `sha256sum` from the repository root, the format `sha256sum -c` expects.

## Limits, stated plainly

Provenance answers "which build is this", not "does this build work". It does not make a claim about GPU
support, about an app, or about a feature's completeness — those need the suites and, for physical
hardware, the panel itself. What it removes is the possibility of two people arguing about a feature while
holding different binaries.

`tools/tests/test_buildinfo.py` asserts the whole chain: the header names a commit that exists in this
repository; the JSON record matches the bytes it was generated from (kernel size, kernel sha256, each
module's size and sha256); the packed image really carries `BUILD.TXT` and its text; `makedisk` refuses a
doctored header and a missing one; and a booted guest's `scos_build_stamp()`, `graphics_report()` and boot
log all equal the header.
