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
| `patch_macho` | `patch_macho.sh` → `macho9 declassify IN OUT`, installed over `OUT` |
| `change_dylib` | `change_dylib.sh` → `macho9 lc` / `dylib` / `rpath`, or `macho9 edit FILE -` when more than one of those |
| `add_version_min` | `add_version_min.sh` → `macho9 minos FILE OUT 10.9`, installed over `FILE` |
| `rename_segment` | `rename_segment.sh` → `macho9 segment FILE OUT OLD NEW` |
| `retag_swift_classes` | `retag_swift_classes.sh` → `macho9 retag-swift FILE OUT`, once per file, installed over each `FILE` |
| `fix_macho` | `fix_macho.sh` → `macho9 lc` / `dylib` / `segment`, or `macho9 edit FILE -` when more than one command's worth (two renames already are) |

plus the two files every wrapper sources:

| file | installed as | what it does |
|---|---|---|
| `translate.sh` | `macho9-translate.sh` | old argv → the `macho9` command line(s) it means. Pure text; runs nothing. |
| `macho9-compat.sh` | `macho9-compat.sh` | finds `macho9`, prints the teaching message, and runs the translation. |

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
check them, with four known exceptions, truthfully not all the same KIND of
known: one reproduced on a real file (one out of 300 in the differential
corpus, below), one argued unreachable in practice rather than observed, and
two true by construction rather than by measurement -- they follow
directly from reading what the wrappers' code does, not from a corpus
row that exhibits them, so no file "reproduces" them and no argument is needed
for why they would be rare:

  * `rename_segment` on a binary carrying `LC_LAZY_LOAD_DYLIB` refuses where
    the C tool renamed, because the shared rewriter builds its
    library-ordinal map before it looks at whether any operation could
    renumber. `compat/rename_segment.sh`'s header has the measurement. It is
    one file out of 300 in `tests/differential.sh`'s corpus, and closing it
    means changing `macho9`.
  * `patch_macho`'s `OUT` gets a NEW INODE where the C tool's
    `open(O_WRONLY|O_CREAT|O_TRUNC)` wrote through the path and kept it. The
    install is `mv`, like every other wrapper's, which is what makes `OUT`
    wholly old or wholly new rather than possibly half-written (neither the C
    tool's write nor the `cat TEMP > OUT` that first replaced it was atomic).
    Its MODE is still exactly what the C tool left -- `0755 & ~umask` for an
    `OUT` that did not exist, `OUT`'s own mode for one that did -- and an
    unchanged run (the pass-through, including `patch_macho IN IN`) installs
    nothing, so that case keeps its inode too. What a rename cannot keep is
    `OUT`'s other HARD LINKS, so an `OUT` carrying any is refused (exit 1)
    instead of being silently split, exactly as `FILE` is for the other five;
    a dangling symlink at `OUT` is refused as well, where the C tool created
    the link's target, and an `OUT` that exists but is not a regular file (a
    directory, a fifo, a device) is refused where the C tool's `open()` either
    wrote to it or failed with `EISDIR`. Those `OUT` pre-checks also answer
    BEFORE the input is diagnosed, so when IN **and** OUT are both bad it is now
    OUT that is named — the same shape as `retag_swift_classes`' pre-check
    below, and exit 1 on both sides either way.
    `compat/patch_macho.sh`'s header has all of it.
  * The writability pre-check `rename_segment.sh` runs (`test -w`, to fail
    before any analysis exactly as the C tool's `open(O_RDWR)` did) can
    disagree with the real open at the edges -- it consults the real uid and
    does not see ACLs. It agrees on the two cases that actually reach a
    caller (absent, and mode-denied); `compat/rename_segment.sh`'s header has
    the detail.
  * `change_dylib` and `add_version_min` are the two wrappers that forward
    the shared rewrite drivers' (`mr_apply_file`, `mv_add_version_min`) own
    exit code verbatim, with no mapping at all -- unlike `fix_macho`,
    `patch_macho` and `rename_segment`, which translate every nonzero
    macho9 exit to one flat historical code, and `retag_swift_classes`,
    which has its own real 1-vs-2 mapping (`compat/retag_swift_classes.sh`'s
    header has it) and is likewise unaffected by this. (EVERY wrapper whose
    verb now writes an output the wrapper installs -- all six, `patch_macho`
    included: its verb's output goes to a temp beside the `OUT` it was asked
    for, and is installed onto it --
    has refusals of its OWN on top of that,
    exiting 1, made before macho9 runs for the argument in question: an
    absent or unwritable `FILE`, a `FILE` carrying other hard links, and a
    failed install. Those are the wrapper's, not a forwarded code -- and for
    `retag_swift_classes` an absent or unwritable argument is a WORDING
    divergence too: `tests/compat-matrix.tsv`'s rows for that case (measured
    before this task) have both sides agreeing on `perror(path)`'s
    "`<path>: No such file or directory`", which is still what
    `mswift_retag_file` itself prints when macho9 actually reaches the
    open() -- but the wrapper's own pre-check now answers first, in its own
    words (`open: No such file or directory`), so only the exit code still
    matches. `add_version_min.sh` has no such gap: its own C tool's
    `perror("open")` already said literally "open: ...", so the wrapper's
    identical wording was never a divergence to begin with. A WRITABLE
    `FILE` inside a NON-writable directory is a fourth case neither wrapper's
    own pre-checks catch -- the write itself fails, `mkstemp: Permission
    denied`, because installing needs the directory writable where the old
    tools needed only `FILE` itself to be; `compat/add_version_min.sh` and
    `compat/retag_swift_classes.sh`'s own headers both name it, and for
    `retag_swift_classes` it surfaces as `had_error` (exit 1) rather than
    `add_version_min`'s raw, forwarded 2, since this wrapper never forwards
    one argument's exit code as the whole run's.
    `change_dylib` briefly had an unwritable-`FILE` guard of its own that
    exited 2, chosen to match what `mr_apply_file`'s `open(O_RDWR)` then gave
    on the single-family path; that path opens `FILE` read-only now, so there
    is no such code to match and the guard is gone -- `mw_prepare` answers
    for `change_dylib` as it does for every other wrapper here, with the C
    tool's own flat 1.) A CONSIDERED
    refusal
    (the input examined and declined) still exits 1, matching the C tool by
    coincidence, not by construction; but a genuine operational failure
    (open, fstat, read or write failing,
    or a checked allocation that `src/rewrite.c`'s drivers or
    `mi_open`/`mfat_parse` make -- `src/rewrite.h`'s `MR_FAIL` comment
    names them) now exits 2, where the C tool always exited a flat 1. One
    exception, `change_dylib`'s only: an allocation failure INSIDE
    `mg_grow_header` or `mg_plausible` (`src/grow.c`) exits 1, the same as
    every other reason either one refuses -- and it needs no `-grow`.
    `change_dylib` reaches `mg_grow_header` only through `--allow-grow`,
    but `src/rewrite.c` runs `mg_plausible` on every rewrite that is not a
    pure segment rename -- every rewrite `change_dylib` can ask for --
    unless `MACHO_NO_VERIFY` is set. `src/rewrite.c`'s own comment on that
    fold has the reasoning. An invocation touching more than one family is
    no longer a sequence of `macho9` lines with shell steps between them:
    it is one `macho9 edit FILE -`, whose exit code is `me_run`'s own, from
    the same `MR_REFUSED`/`MR_FAIL` vocabulary. `compat/change_dylib.sh`
    and `compat/add_version_min.sh`'s own headers have the rest of the
    detail.

There is a fourth gap this list used to omit entirely: no argument
combination in `tests/compat-sweep.sh`'s 1227-row matrix ever exercises
`mg_grow_header` (`grep -c "grew header pad" tests/compat-matrix.tsv` is 0)
-- `tests/fixture.macho`'s header pad is large enough, and the sweep's
argument vocabulary short enough, that nothing in it ever needs to grow. 72
of those rows DO give one old mixed-family `-grow` two chances to grow
(compat/change_dylib.c issued one grow call for the whole operation set; the
emitted `macho9 edit` script runs a dylib statement and an rpath statement as
separate passes under one `allow-grow`, each capable of growing on its own),
and no row forces either of those to actually grow. `tests/change_dylib_test.sh`'s "mixed-family
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
else: `macho9` sees one rename at a time either way — its `segment` verb takes
one pair, and an edit script's `segment rename` statement is one pair — so
nothing downstream would ever count them.
