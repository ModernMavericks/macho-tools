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
| 9 | `macho9` never writes its input (replaces "skip the write when nothing changed") | `specs/2026-09-11-never-write-the-input-design.md` | — | spec written; runs after 10 and 11 |
| 10 | `allow-grow` everywhere it is expected | `specs/2026-09-11-allow-grow-everywhere-design.md` | `plans/2026-09-11-allow-grow-everywhere.md` | plan written |
| 11 | `edit` on fat (universal) files | `specs/2026-09-11-edit-on-fat-files-design.md` | `plans/2026-09-11-edit-on-fat-files.md` | plan written |

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
- thin 64-bit input only; a fat file is refused;
- `allow-grow` reaches `dylib` and `rpath` only; `version-min set` refuses
  "no room" even under it;
- statement order is execution order, so two `dylib insert` lines give the
  reverse of `-insert A -insert B`. A generator that emits scripts from verb
  lines (`compat/translate.sh`, per the spec's "Consumers") must reverse them.

**For item 6** (pre-existing, found along the way): `src/rewrite.c`'s three
unchecked `calloc`s (two in `mr_process_thin`, one in `mr_process_fat`) crash
rather than refuse on allocation failure; `src/swift_retag.h`'s "only
MSWIFT_ERROR is a failure of the tool itself" is false since `MSWIFT_RACED`
also exits 2; about 87 older comments across the tree still name plan
artifacts ("Task N", briefs, rounds) — all predate item 2.

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
