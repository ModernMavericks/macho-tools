# compat/

The six original entry points, kept for compatibility. Five of them are now
`/bin/sh` wrappers around `macho9`; the sixth is still C.

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

**One thing a wrapper does change for that caller**, and it is a packaging
fact rather than a behavioural one: a wrapper cannot work without `macho9`,
`macho9-compat.sh` and `macho9-translate.sh` sitting in the same directory.
A caller that fetches three files by name now needs six. That is inherent to
replacing the binaries with wrappers at all, not to how these particular ones
are written, and it is why all four are installed **flat** into the same
`bin` rather than into a `libexec/` subdirectory.

## What "drop-in" means here, precisely

The rewritten file's bytes and the exit codes are identical to the C tools'.
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

## Why `fix_macho` is still C

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

Its two repeated options are capped now, in `fix_macho.c`, with
`change_dylib`'s exact wording — the fixed-size arrays they filled had no
bounds check at all, which is the same stack smash `docs/PROPOSAL.md` records
being fixed in `change_dylib` alone.
