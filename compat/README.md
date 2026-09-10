# compat/

The six original entry points, kept for compatibility. Five of them are now
`/bin/sh` wrappers around `macho9`; the sixth is still C.

> **The goal is not met yet.** The retirement plan's headline is "`macho9`
> becomes the only Mach-O rewriting binary this repo ships." This repo still
> ships two: `macho9` and `fix_macho`. Five of six is real progress and it is
> not the goal — see "Why `fix_macho` is still C" below, and do not read the
> table above as saying otherwise.

| installed name | what it is now |
|---|---|
| `patch_macho` | `patch_macho.sh` → `macho9 declassify IN OUT` |
| `change_dylib` | `change_dylib.sh` → `macho9 lc` / `dylib` / `rpath` |
| `add_version_min` | `add_version_min.sh` → `macho9 minos FILE 10.9` |
| `rename_segment` | `rename_segment.sh` → `macho9 segment FILE OLD NEW` |
| `retag_swift_classes` | `retag_swift_classes.sh` → `macho9 retag-swift FILE`, once per file |
| `fix_macho` | still `fix_macho.c` — see below |

plus the two files every wrapper sources:

| file | installed as | what it does |
|---|---|---|
| `translate.sh` | `macho9-translate.sh` | old argv → the `macho9` command line(s) it means. Pure text; runs nothing. |
| `macho9-compat.sh` | `macho9-compat.sh` | finds `macho9`, prints the teaching message, runs the command lines, and keeps a multi-command sequence from leaving a half-converted binary behind. |

## Why the names are unchanged

`mavericksforever.com/claude/install.sh` fetches `patch_macho`,
`change_dylib` and `add_version_min` **by those names** and its generated
`/usr/local/bin/claude` wrapper invokes them by those names.

**Today** it fetches them from
[`Wowfunhappy/Mavericks-Porting-Resources`](https://github.com/Wowfunhappy/Mavericks-Porting-Resources),
not from this repo — so renaming them right now would break nothing live (see
`PROVENANCE.md`, "Extracted from a branch, not from master"). Keeping the
names matching is about the adoption path: PRs #11/#12 upstream this repo's
fixes to that repo, and if/when Wowfunhappy merges them, or
`mavericksforever.com` points `install.sh` at this repo instead (see
`docs/PROPOSAL.md`, "one repo, first-party"), `install.sh`'s existing
invocations have to already resolve to the same names here.

### What a packager has to change, and who has not been told

**A wrapper cannot work without `macho9`, `macho9-compat.sh` and
`macho9-translate.sh` sitting in the same directory.** `install.sh` fetches
`patch_macho`, `change_dylib` and `add_version_min` **by name**, three files;
those three now need three more beside them. Fetch the three alone and you get
three names that cannot run.

That is inherent to replacing the binaries with wrappers at all, not to how
these particular ones are written, and it is why everything installs **flat**
into one `bin` rather than into a `libexec/` subdirectory — a flat layout
needs only extra file names from that script, where a subdirectory would need
it restructured.

**Nobody has told whoever owns that script.** `install.sh` lives at
`mavericksforever.com` and is not this repo's to change; the retirement plan's
Task 3 is already blocked on it moving, and this is a second, earlier reason
the same conversation has to happen. Until it does, a CDN built from this repo
would ship three names that cannot run. `.github/workflows/release.yml` puts
all six files in the release artifact, which is the most this repo can do on
its own.

## What "drop-in" means here, precisely

The exit codes are identical to the C tools', and the rewritten file's bytes
are identical everywhere `tests/differential.sh` and `tests/compat-sweep.sh`
check them, with three known exceptions, each measured at its own site: one
reproduced on a real file (one out of 300 in the differential corpus, below),
the other two argued unreachable in practice rather than observed:

  * `rename_segment` on a binary carrying `LC_LAZY_LOAD_DYLIB` refuses where
    the C tool renamed, because the shared rewriter builds its
    library-ordinal map before it looks at whether any operation could
    renumber. `compat/rename_segment.sh`'s header has the measurement. It is
    one file out of 300 in `tests/differential.sh`'s corpus, and closing it
    means changing `macho9`.
  * `retag_swift_classes` on a file that changed under it mid-run
    (`MSWIFT_RACED`) exits 1 where the C tool exited 0, because reporting
    success for a write that did not happen is the silent-success shape this
    codebase refuses. `compat/retag_swift_classes.sh`'s header has the
    measurement; a race is not something a test can stage.
  * The writability pre-check `rename_segment.sh` runs (`test -w`, to fail
    before any analysis exactly as the C tool's `open(O_RDWR)` did) can
    disagree with the real open at the edges -- it consults the real uid and
    does not see ACLs. It agrees on the two cases that actually reach a
    caller (absent, and mode-denied); `compat/rename_segment.sh`'s header has
    the detail.

There is a fourth gap this list used to omit entirely: no argument
combination in `tests/compat-sweep.sh`'s 1227-row matrix ever exercises
`mg_grow_header` (`grep -c "grew header pad" tests/compat-matrix.tsv` is 0)
-- `tests/fixture.macho`'s header pad is large enough, and the sweep's
argument vocabulary short enough, that nothing in it ever needs to grow. 72
of those rows DO emit two `--allow-grow` macho9 invocations for one old
mixed-family `-grow` (compat/change_dylib.c issued one grow call for the
whole operation set; the emitted sequence issues one dylib-family call and
one rpath-family call, each capable of growing on its own), and no row forces
either of those to actually grow. `tests/change_dylib_test.sh`'s "mixed-family
double grow" case closes that gap directly (not through the sweep) with
inputs sized to force a real double grow, and compares the result byte-for-
byte against a single combined `mr_apply_file` call built the way
`compat/change_dylib.c` used to build one. On that case the two routes are
byte-identical: `mg_grow_header` grows by the excess over whatever pad it
sees at the moment, rounded up to a whole page, so growing twice in sequence
composes losslessly with growing once for the summed delta (a page-aligned
grow does not change what the next `ceil` rounds to). That is a property of
the growth algorithm, not a coincidence of one fixture, but it is verified
here only for two sequential grows on one image, not for three or more mixed
families, a fat container, or every possible order.

Stdout is identical everywhere a caller or an in-repo test can see it, and
each wrapper's own header **enumerates** the places where it is not, with the
measurement behind each one (`tests/compat-matrix.tsv` records what all 1227
enumerated argument combinations did on both sides, stdout included).

Stderr is where the wrappers deliberately differ: each one prints the
`macho9` equivalent of the invocation it just received, so the caller's
script keeps working while the message teaches the new grammar. That is the
retirement plan's "phase one", and stdout stays clean precisely so this can
go on stderr.

`tests/known-callers.sh` replays every caller Task 0 of that plan found — the
production `install.sh` wrapper pipeline first — and `tests/wrapper_test.sh`
covers the wrappers' own grammar, exit-code and stdout mapping.

## Why `fix_macho` is still C — and why the plan's goal is not met

It was attempted as a wrapper and measured, on real 10.9, against the
binaries that shipped before the wrappers. It cannot be wrapped without
changing what it does: `macho9 dylib -replace` rewrites a longer path using
header pad where `fix_macho` refuses outright (different exit code AND
different bytes), and a chained `-rename_seg` has no `macho9` equivalent at
all, so a wrapper would refuse an invocation `fix_macho` accepts. Three
smaller differences follow (no `mg_plausible` gate, a tolerated bad fat
slice, and stdout nothing could reconstruct). `compat/fix_macho.c`'s own
header has the detail. Converging it in C is the honest way to retire it, and
that is a decision to take deliberately rather than a wrapper to slip in.

So this repo still ships **two** Mach-O rewriting binaries, not one.
Converging `fix_macho` onto the shared drivers in C is real work with its own
behaviour decisions, and it belongs to its own task.

Its two repeated options are capped now, in `fix_macho.c`, with
`change_dylib`'s exact wording — the fixed-size arrays they filled had no
bounds check at all, which is the same stack smash `docs/PROPOSAL.md` records
being fixed in `change_dylib` alone.
