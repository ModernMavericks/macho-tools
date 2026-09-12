# Work queue

The agreed order. Each item names its spec and, once written, its plan.

| # | item | spec | plan | state |
|---|---|---|---|---|
| 1 | Report what macho9 did | — | `plans/2026-09-10-report-what-macho9-did.md` | **done**, pushed, CI green at `77f076a` |
| 2 | Edit scripts | `specs/2026-09-10-edit-scripts-design.md` | `plans/2026-09-10-edit-scripts.md` | **done**, pushed, CI green at `36703e0` |
| 3 | Rename + target | `specs/2026-09-10-machotool-rename-and-target-design.md` | `plans/2026-09-10-machotool-rename-and-target.md` | plan written; unblocked |
| 4 | Release conformance | `specs/2026-09-10-release-conformance-design.md` | `plans/2026-09-10-release-conformance.md` | plan written; shelved until item 3 merges |
| 5 | Relations + verb lowering | `specs/2026-09-10-relations-and-verb-lowering-design.md` | `plans/2026-09-10-relations-and-verb-lowering.md` | plan written before item 2 shipped; re-check against it before starting (see below) |
| 6 | **Human code review + excellent documentation** | — | — | not started |
| 7 | History rewrite + the three rename steps | — | — | last of the in-tree work |
| 8 | `.pkg` + Sparkle updater | — | — | after 7; not yet designed |
| 9 | `macho9` never writes its input (replaces "skip the write when nothing changed") | `specs/2026-09-11-never-write-the-input-design.md` | `plans/2026-09-11-never-write-the-input.md` | plan written; runs after 10 and 11 |
| 10 | `allow-grow` everywhere it is expected | `specs/2026-09-11-allow-grow-everywhere-design.md` | `plans/2026-09-11-allow-grow-everywhere.md` | **done**, pushed, `b76ddf1..9ae6835` |
| 11 | `edit` on fat (universal) files | `specs/2026-09-11-edit-on-fat-files-design.md` | `plans/2026-09-11-edit-on-fat-files.md` | **done**, pushed, `8f17001..956b4f6` |
| 12 | An `insert_dylib` wrapper | — | — | not started; not yet designed |

Items 9–11 follow from item 2 and run **before item 3**, in the order 10, 11, 9: item 9's wrappers emit edit scripts for multi-command invocations, which needs item 11's fat support. Their plans are
written against today's names (`macho9`, `cli/macho9.c`) and today's
`--verbose` flag. Item 3 renames the product by sweeping the tree, which
picks up whatever 9–11 added, and its Task 6 ("always verbose, on stderr")
deletes the flag -- turning every verbose-only line 9–11 add, such as the
per-slice lines, into always-on output in the same pass. Running item 3
first would mean rebasing all three plans onto the new names.

## Why this order

**2 before 5.** Relations and verb lowering have nothing to attach to until
`MS_TABLE`, `ms_script` and `me_run` exist.

**3 before 4.** `release.yml`'s artifact list names `macho9`, `macho9-compat.sh`
and `macho9-translate.sh`; the rename changes all three. Landing release
conformance first would edit the same lines twice.

**8 is split out of 4, and goes after 7.** The `.pkg` and the Sparkle updater.
Its own spec section calls it "the largest single piece and the one most
reasonably split into its own increment", and everything else in item 4 can land
first and produce a GitHub Release of the binaries. The repo owner placed it
after the history rewrite; it needs a spec and a plan before it starts, and
neither is wanted yet. The constraint to carry into them is that the updater
must not link the product it updates.

**6 before 7, and 6 after everything else.** A human review wants the code in its
final shape — renamed, versioned, with the language in place — so it is not
reviewing something about to be restructured. And it must come *before* the
history rewrite for two reasons: a review that produces changes puts those
changes in the history being rewritten, and a rewrite done first would mean the
reviewer is reading commits that no longer exist by the time their comments land.

**Documentation belongs with the review, not after it.** Item 6 is one item on
purpose. The docs are part of what gets reviewed — a human reading this codebase
reads the comments and the READMEs as much as the code, and this repo already
treats a comment that claims more than the code does as a defect. Sequencing a
documentation pass *after* the review would mean the reviewer read the version
that was not yet worth reading; sequencing it before would mean polishing prose
about code the review is about to change.

**The README is a named input to item 6**, raised by the repo owner
2026-09-11: it is far too long, and its opening sentence does not parse.

- **Length.** 360 lines, of which the `macho9 edit` manual (its file format,
  statements, directives, worked example and limits) is about 200 — 55% of the
  front door spent on one verb. That material is reference, not introduction;
  it wants its own file, with the README keeping a short pointer.
- **The opening sentence.** "Mach-O surgery for hosts too old to have any"
  reads as *hosts too old to have any surgery*, which is not the claim. The
  claim is that the tools that would normally do this work do not exist for,
  or do not run on, 10.9. Say that plainly.
- Also: the Layout section explains `compat/` at a length that belongs in
  `compat/README.md`, which already exists and says it.

One known input to that pass, raised by the repo owner while approving item 5's
design: **the module prefixes** (`mi_`, `mr_`, `mg_`, `mo_`, `mseg_`, `mswift_`,
`wa_`, `ms_`, `me_`) mean nothing to a reader who has not learned them. That is a
readability decision no single design should make unilaterally, and item 6 is
where it belongs.

**7 after everything but 8** because rewriting history invalidates every commit
SHA this repo's docs, ledgers and plans cite.

## Carried out of item 2

Item 2 shipped `cbcacd3..36703e0`. What it deliberately left for later, so the
record does not live only in a git-ignored ledger:

**For item 5.** Its plan predates what item 2 built, and should be re-read
against `src/edit.c`, the buffer-level seams item 2 exposed (`mr_apply_image`,
`mv_add_version_min_image`, `mswift_retag_image`, `md_declassify_buf`),
`lc_kind_by_name`, and the `mr_ops` result pointers (`segment_renamed`,
`renumbering`). Open items that belong to it:

- During an `edit` run the operations still print their own stdout progress
  lines, so a run later refused can show "updated" on stdout. Those lines are
  the compat wrappers' byte-identical contract; silencing them per front-end is
  the output restructuring verb lowering exists to do. The README says stderr's
  refusal line and the exit code are authoritative meanwhile.
- The segment-name validity check lives in the front-ends (`cmd_segment`,
  `src/edit.c`), not in the operation.
- An allocation failure inside `mg_grow_header` or `mg_plausible` is reported
  as refused (1), not error (2): splitting their per-slice status widens into
  `src/grow.c`'s contracts. Disclosed in `src/rewrite.c`.
- The core's no-room message says "pass -grow to enlarge it", naming neither
  `edit`'s `allow-grow` directive nor the CLI's `--allow-grow`.

**Limits of `edit` as shipped**, each refused rather than wrong:
- thin 64-bit input only; a fat file is refused — *closed by item 11*;
- `allow-grow` reaches `dylib` and `rpath` only; `version-min set` refuses
  "no room" even under it — *closed by item 10*;
- statement order is execution order, so two `dylib insert` lines give the
  reverse of `-insert A -insert B`. A generator that emits scripts from verb
  lines (`compat/translate.sh`, per the spec's "Consumers") must reverse them.

**For item 6** (pre-existing, found along the way): `src/rewrite.c`'s three
unchecked `calloc`s (two in `mr_process_thin`, one in `mr_process_fat`) crash
rather than refuse on allocation failure; about 87 older comments across the
tree still name plan artifacts ("Task N", briefs, rounds) — all predate item 2.
In `md_declassify_buf` (`src/declassify.c`), the first-section walk and the
`__LINKEDIT` extension go through `segs[]` pointers taken before the loop that
`memmove`s the removed load commands out, and never refreshed: in a crafted
file where a removed command precedes a segment command, both would read and
write shifted content.

## Carried out of item 10

**A confirmed silent-corruption bug, pre-existing: fixed in `66ca5ce`.**
`mg_first_sect_off` answered 4096 for an image with no section data, and
`src/grow.h` called that "a real, if unusual, answer". It was not: on a
sectionless image of 4096 bytes or more, `mr_process_thin` took it as the
header-pad boundary, and its commit zeroed everything up to it. Reproduced on
an 8192-byte image: `macho9 dylib F -append /x` exited 0 having zeroed bytes
200..4095, with or without `MACHO_NO_VERIFY`, because `mg_plausible` accepts
that image. (An earlier version of this note said `mg_plausible` happened to
refuse the fixture without the variable; it does not, so it was never a
guard.) `src/declassify.c` had its own copy of the same 4096 fallback, checked
against `buf + 4096` and never against the file size. The smaller case -- a
sectionless image shorter than 4096 bytes, which wrote past the buffer -- was
fixed in item 10 (`34a3187`). `66ca5ce` stops treating "no sections" as
"4096": `mg_first_sect_off` answers `MG_NO_SECTION_DATA`, every caller that
writes into the pad refuses it (declassify also refuses a first section past
the end of the image), and `macho9 info` reports the pad as unknown.
`7ea664a` makes `mg_grow_header` refuse a first section whose file offset lies
past the end of the image, the bound every other caller already had; before
it, `macho9 grow` on such an image died of SIGSEGV.

**Four wording overclaims for item 6's documentation pass**, two of them
resolved by `66ca5ce`, which rewrote both passages: `src/grow.h:99-101` (an
image with no section data was refused only when shorter than 4096 bytes) and
`tests/leaf-tool-crashes.sh:19-23` (past tense about a clearing that still
happened on large images). Still open: `README.md:300` and `src/edit.h:153`
("Nor does the conversion need it" needs the same "on a modern chained binary"
qualifier as the sentence after it); `src/edit.h:108-114` (omits the grow line
printed before a refusal on a non-PIE image).

## Carried out of item 11

Item 11 shipped `8f17001..956b4f6`. Its final review found nothing critical or
important; what it left deliberately:

**For item 9** (`macho9` never writes its input). `me_fat_slice` sets
`*changed` for every selected slice, so a fat `edit` always reassembles and
rewrites the container even when no statement changed a byte — and reassembly
sizes the output from the furthest slice end, so bytes trailing the last slice
are dropped (a 16613-byte input with a no-op script gives a 16600-byte output).
The verb path drops them too, but only when something actually changed, so this
is not a regression in `mr_process_fat` — it is new only in that `edit` reaches
the reassembly unconditionally. Item 9's "skip the write when nothing changed"
closes both halves.

**For item 6** (documentation and review): `--capabilities` advertises
`statement` rows but no directives at all, so the `translate.sh` emitter the
edit-scripts spec names cannot discover `arch` — nor `allow-grow` nor
`fatal-warnings`. Consistent with the existing protocol rather than a gap item
11 opened, but worth deciding once.

**Pre-existing, not touched:** `src/fat.c:189` truncates a slice's offset and
size to `uint32_t`, so a container larger than 4 GiB is silently mis-laid-out.
The line moved verbatim out of `mr_process_fat` into `mfat_rewrite`, which is
now the one place a bound on `max_end` would go. `edit`'s post-reassembly
`mfat_parse` would likely catch the result; the verb path has no such re-parse.
`src/edit.c`'s `char have[256]` slice list, printed by the "no such slice"
refusals, truncates silently — unreachable with five arch names.

## Item 12: an `insert_dylib` wrapper

Unlike the six in `compat/`, this one would emulate a tool this repo never
shipped. `Wowfunhappy/insert_dylib` (a fork of `Tyilo/insert_dylib`) is prior
art we measured ourselves against and took the export-trie rebuild from —
`docs/prior-art.md`, `src/trie.c`. Nothing in `tests/known-callers.sh`'s
caller set invokes it, so this is convenience for people who already know that
grammar, not compatibility debt.

The capability is already here under our own grammar: `macho9 dylib FILE
-append PATH` is insert_dylib's core act, `-insert` puts it at ordinal 1, and
`lc -delete codesig` covers `--strip-codesig`. What a wrapper adds is its CLI
shape — `insert_dylib dylib_path binary [new_binary]`, with `--inplace`,
`--all-yes`, `--weak`, `--strip-codesig` — which is the part worth designing
rather than guessing. Two things to settle first: which of its flags have an
equivalent at all (`--weak` means `LC_LOAD_WEAK_DYLIB`, which the `dylib` verb
does not emit today), and what the repo owner's actual use of the fork is —
the answer decides whether this is a full wrapper or one worked example in the
README. Runs after item 9, since it inherits the `FILE OUT` grammar.

## Outstanding owner actions

- Review the four specs above (items 2–5).
- **The three rename steps are deferred to item 7**, immediately before the
  history rewrite — not to item 3. See below.

## The rename steps sit with the history rewrite, not with item 3

Item 3 renames the *product*: the binary, the CMake targets, the shared wrapper
scripts, the prose. That is all in-tree and lands on its own. The three steps the
repo owner takes — moving the agent's project directory, moving the clone,
renaming the GitHub repo — are about **where things live**, and were checked
against the tree rather than assumed:

- **Nothing in the repo keys on the clone's directory name.** The only
  `macho-tools` strings outside `docs/` are `CMakeLists.txt`'s status messages
  and `release.yml`'s artifact name, and item 3 renames both as product names
  wherever the clone happens to sit.
- **The build directories are a convention, not a derivation.** No shipyard
  script generates `/private/tmp/mm-build/schmonz/macho-tools/…` from the repo
  path. Moving the clone does invalidate the configured build dirs, because
  `CMakeCache.txt` holds absolute source paths — one reconfigure, not a redesign.
- **Both checkout roots are one filesystem object** (same inode), so moving
  either moves both views at once.

So the product rename and the location rename are independent, and the location
rename belongs here because **the history rewrite is the other change that
invalidates infrastructure** — every commit SHA cited across these docs and the
SDD ledgers. Doing both at one break costs one disruption instead of two.

**The order within the pair is still invariant.** The project directory must move
immediately before the clone, in the same sitting, with no session live in that
directory:

```sh
mv ~/.claude/projects/-Users-schmonz-Documents-code-trees-mavericks-macho-tools \
   ~/.claude/projects/-Users-schmonz-Documents-code-trees-mavericks-machotool
mv ~/Documents/code/trees/mavericks-macho-tools \
   ~/Documents/code/trees/mavericks-machotool
```

Renaming the clone first orphans the memories and every transcript of this work.

**The GitHub rename is independent of both** and can happen whenever — GitHub
redirects the old URL, so the local remote keeps working either way. Grouping it
here only keeps the mental model to one "rename day".
