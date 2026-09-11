# compat/

The six original entry points, kept for compatibility. All six are now
`/bin/sh` wrappers around `macho9`. There is no C left in this directory.

> **The goal is met.** The retirement plan's headline was "`macho9` becomes
> the only Mach-O rewriting binary this repo ships." It is: `compat/` holds
> six shell wrappers and two shell support files, and `macho9` is the only
> binary `CMakeLists.txt` builds or installs. `fix_macho` was the holdout —
> see "Why `fix_macho` could not be wrapped, and what changed" below, which is
> the record of what adopting its five divergences cost and why that was the
> right call rather than a shortcut.

| installed name | what it is now |
|---|---|
| `patch_macho` | `patch_macho.sh` → `macho9 declassify IN OUT` |
| `change_dylib` | `change_dylib.sh` → `macho9 lc` / `dylib` / `rpath` |
| `add_version_min` | `add_version_min.sh` → `macho9 minos FILE 10.9` |
| `rename_segment` | `rename_segment.sh` → `macho9 segment FILE OLD NEW` |
| `retag_swift_classes` | `retag_swift_classes.sh` → `macho9 retag-swift FILE`, once per file |
| `fix_macho` | `fix_macho.sh` → `macho9 lc` / `dylib` / `segment` |

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
check them, with four known exceptions, each measured at its own site: one
reproduced on a real file (one out of 300 in the differential corpus, below),
the other three argued unreachable in practice rather than observed:

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
  * `change_dylib` and `add_version_min` are the two wrappers that forward
    the shared rewrite drivers' (`mr_apply_file`, `mv_add_version_min`) own
    exit code verbatim, with no mapping at all -- unlike `fix_macho`,
    `patch_macho` and `rename_segment`, which translate to their own
    historical codes and are unaffected by this. A CONSIDERED refusal (the
    input examined and declined) still exits 1, matching the C tool by
    coincidence, not by construction; but a genuine operational failure
    (open/fstat/read/write/malloc) now exits 2, where the C tool always
    exited a flat 1. `compat/change_dylib.sh` and `compat/add_version_min.sh`'s
    own headers have the detail.

There is a fifth gap this list used to omit entirely: no argument
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
enumerated argument combinations did on both sides, stdout included). The one
exception is `fix_macho`, whose stdout is deliberately not reproduced at all —
see below.

Stderr is where the wrappers deliberately differ: each one prints the
`macho9` equivalent of the invocation it just received, so the caller's
script keeps working while the message teaches the new grammar. That is the
retirement plan's "phase one", and stdout stays clean precisely so this can
go on stderr.

`tests/known-callers.sh` replays every caller Task 0 of that plan found — the
production `install.sh` wrapper pipeline first — and `tests/wrapper_test.sh`
covers the wrappers' own grammar, exit-code and stdout mapping.

## Why `fix_macho` could not be wrapped, and what changed

It was attempted as a wrapper once before and measured, on real 10.9, against
the binaries that shipped before the wrappers — and it could not be wrapped,
because a wrapper had to **preserve** behaviour and `fix_macho`'s differs from
the shared rewriter's. It stayed C for a whole plan on that basis.

What changed is not the code but the standard: the repo owner ruled those
differences **improvements to adopt deliberately**. There are five, and
`compat/fix_macho.sh`'s "DELIBERATE DIVERGENCES FROM fix_macho" block states
each with its reason:

1. a replacement path longer than the existing load command is now rewritten
   into header pad instead of refused;
2. a chained `-rename_seg A B -rename_seg B C` now produces `C` instead of
   stopping at `B`;
3. the write-back is atomic (`wa_write_atomic`) instead of `lseek` + `write`
   over the original;
4. a fat slice that **is** a 64-bit Mach-O and whose edit fails now refuses
   the whole file instead of being skipped with the rest rewritten. (A slice
   that is not a Mach-O at all is still skipped, exactly as before —
   `tests/wrapper_test.sh` pins that distinction.)
5. a `-change` aimed at the dylib's own install name now matches nothing
   instead of rewriting `LC_ID_DYLIB` — `fix_macho.c`'s own comment said
   "nothing in `changes` is ever meant to match it", but its match block had
   no exclusion for `LC_ID_DYLIB` and rewrote it anyway. Both sides exit 0
   and the bytes differ; nothing on stderr named the reason. `macho9`'s
   `-change` now matches what `install_name_tool` does (`-id`, never
   `-change`, touches identity) — `src/rewrite.c` enforces it, and
   `tests/wrapper_test.sh` pins it on a dylib fixture, alongside a real
   dependency's `-change` in the same run still landing.

`fix_macho`'s stdout is not reproduced either, and that is deliberate:
`Processing thin Mach-O:` / `Changed: X -> Y` / `File updated: F` /
`No changes needed: F` are replaced by `macho9`'s own reporting plus the
per-operation `macho9: <path> matched nothing` lines on stderr, which say more
than `No changes needed` could. This repo's own `tests/change_dylib_test.sh`
was `fix_macho`'s only caller.

Its two repeated options are still capped, in `compat/translate.sh`'s
`mt_room`, with the same wording — the fixed-size arrays they filled had no
bounds check at all, which is the same stack smash `docs/PROPOSAL.md` records
being fixed in `change_dylib` alone. The `-rename_seg` cap exists nowhere
else: each pair becomes its own `macho9 segment` invocation, so nothing
downstream would ever count them.
