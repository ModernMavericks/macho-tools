# Work queue

The agreed order. Each item names its spec and, once written, its plan.

| # | item | spec | plan | state |
|---|---|---|---|---|
| 1 | Report what macho9 did | — | `plans/2026-09-10-report-what-macho9-did.md` | **done**, pushed, CI green at `77f076a` |
| 2 | Edit scripts | `specs/2026-09-10-edit-scripts-design.md` | `plans/2026-09-10-edit-scripts.md` | **done**, pushed, CI green at `36703e0` |
| 3 | Rename + target | `specs/2026-09-10-machotool-rename-and-target-design.md` | `plans/2026-09-10-machotool-rename-and-target.md` | **done**, pushed, `9e39a57..770433f` |
| 4 | Release conformance | `specs/2026-09-10-release-conformance-design.md` | `plans/2026-09-10-release-conformance.md` | plan written; shelved until item 3 merges |
| 5 | Relations + verb lowering | `specs/2026-09-10-relations-and-verb-lowering-design.md` | `plans/2026-09-10-relations-and-verb-lowering.md` | plan written before item 2 shipped; re-check against it before starting (see below) |
| 6 | **Human code review + excellent documentation** | — | — | not started |
| 7 | History rewrite + the three rename steps | — | — | last of the in-tree work |
| 8 | `.pkg` + Sparkle updater | — | — | after 7; not yet designed |
| 9 | `machotool` never writes its input (replaces "skip the write when nothing changed") | `specs/2026-09-11-never-write-the-input-design.md` | `plans/2026-09-11-never-write-the-input.md` | **done**, pushed, `bccd008..1c0c38c` |
| 10 | `allow-grow` everywhere it is expected | `specs/2026-09-11-allow-grow-everywhere-design.md` | `plans/2026-09-11-allow-grow-everywhere.md` | **done**, pushed, `b76ddf1..9ae6835` |
| 11 | `edit` on fat (universal) files | `specs/2026-09-11-edit-on-fat-files-design.md` | `plans/2026-09-11-edit-on-fat-files.md` | **done**, pushed, `8f17001..956b4f6` |
| 12 | An `insert_dylib` wrapper | — | — | not started; not yet designed |
| 13 | What real app backports need and we lack | — | — | not started; researched 2026-09-11, see below |
| 14 | Spike: weaken binds in memory at load time | — | — | **spike done** 2026-09-12: answered NO; see below |
| 15 | Flat-namespace shim: satisfy missing symbols at runtime | — | — | not started; came out of item 14's spike |

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

- **Length.** 360 lines, of which the `machotool edit` manual (its file format,
  statements, directives, worked example and limits) is about 200 — 55% of the
  front door spent on one verb. That material is reference, not introduction;
  it wants its own file, with the README keeping a short pointer.
- **The opening sentence.** "Mach-O surgery for hosts too old to have any"
  reads as *hosts too old to have any surgery*, which is not the claim. The
  claim is that the tools that would normally do this work do not exist for,
  or do not run on, 10.9. Say that plainly.
- Also: the Layout section explains `compat/` at a length that belongs in
  `compat/README.md`, which already exists and says it.

**Deleting the README's unreviewed marker is part of item 6.** `README.md` now
carries, under its heading, the family's generated-README marker — visible
prose, so it renders on the repo's front page. Shipyard's `publish-release.yml`
refuses a repo's *first* release while that line is present, so item 4 cannot
ship until a human has read this README and removed it. That is the intended
loop: item 6 is already the pass where a human rewrites the front door, and the
two complaints recorded below — the length, and the opening sentence that does
not parse — are exactly what the marker is asking someone to fix.

**Comment density is the other named input to item 6**, raised by the repo
owner 2026-09-12 after the rename: *"my eyes glaze over attempting to skim the
compat wrappers, several screens of comments away from finding where they
actually happen."* Measured that day:

| file | total | comments | code | first code at |
|---|---|---|---|---|
| `fix_macho.sh` | 284 | 259 (91%) | **20** | line 251 |
| `change_dylib.sh` | 221 | 196 (88%) | **20** | line 184 |
| `rename_segment.sh` | 232 | 190 (81%) | 31 | line 168 |
| `patch_macho.sh` | 205 | 156 (76%) | 40 | line 128 |
| `translate.sh` | 808 | 469 (58%) | 311 | line 240 |

A 284-line wrapper where 20 lines do anything, reached after 250 lines of
prose. That is a defect in placement, not a matter of taste.

**The expensive class is narration**, not comments as such: measured
transcripts, repro steps, quotations of earlier comment text, accounts of what
a retired tool did on a particular day. It goes stale silently, and the rename
(item 3) paid a fix round for it at nearly every task — most of one whole task
was spent classifying comments as "describes the tool now" versus "reports what
happened then", and getting it wrong is invisible to a grep, because a
substitution erases the tell.

**Prefer a test to a comment.** Item 3 demonstrated the asymmetry: the comments
went stale repeatedly and nothing caught them, while every mutation thrown at
the tests failed loudly. A test named for a quirk fails when someone "fixes"
the quirk; a comment describing the quirk merely becomes wrong. This repo's
tests are strong enough that much of the narration describes something already
pinned.

**The order to apply, per passage:** can it be a test? Then can it be a commit
message — history belongs in history? Then can it be `compat/README.md`, which
already exists and is where divergence tables and measured transcripts belong?
Only what survives all three stays inline, and only what a reader must see *at
that line* to avoid breaking it: a sentence, not a screen.

What is worth keeping inline, on the evidence: the short load-bearing kind. The
comment explaining why the tool's own diagnostics carry its name is what made
the rename mandatory rather than optional. The one-liner saying to pass a value
through `ENVIRON` rather than `awk -v` prevents a bug that had already shipped
once. Both are a sentence at the point of danger.

**A note on how it got this way, so the pass does not just blame the past.**
Dense comments are cheap to write and their cost lands later, on whoever reads
or renames. The assistant added prose at nearly every review cycle of items 9
and 3; the trend was worsening, not historical. Whatever rule item 6 lands on
should bind new work, not only clean up old.

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

**For item 9** (`machotool` never writes its input). `me_fat_slice` sets
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

The capability is already here under our own grammar: `machotool dylib FILE
-append PATH` is insert_dylib's core act, `-insert` puts it at ordinal 1, and
`lc -delete codesig` covers `--strip-codesig`. What a wrapper adds is its CLI
shape — `insert_dylib dylib_path binary [new_binary]`, with `--inplace`,
`--all-yes`, `--weak`, `--strip-codesig` — which is the part worth designing
rather than guessing. Two things to settle first: which of its flags have an
equivalent at all (`--weak` means `LC_LOAD_WEAK_DYLIB`, which the `dylib` verb
does not emit today), and what the repo owner's actual use of the fork is —
the answer decides whether this is a full wrapper or one worked example in the
README. Runs after item 9, since it inherits the `FILE OUT` grammar.

## Item 13: what real app backports need and we lack

Researched 2026-09-11 after the repo owner recalled a project running newer
iLife/iWork on Mavericks. Full report, with sources and line references, in
`.superpowers/research-ilife-iwork-backports.md`. **That path is untracked, not
ignored** — `.gitignore` covers only the build directories, `/VERSION` and
`CMakeUserPresets.json` — so the file is one `git clean -fdx` from gone. Both it
and item 14's spike report want moving into `docs/` when either item gets a
spec, after a read-through: this repo is public, so committing them is
publishing, and they are subagent prose that has not had a documentation pass.

**What exists.** One direct hit:
`nfzerox/MavericksAppCompatibilityLayer`, which patches Keynote 6.6.2 / Pages
5.6.2 / Numbers 3.6.2 to run on 10.9.5 — published once in May 2016 and
abandoned (one squashed commit, no issues, no forks). Its relatives run the same
direction without being Apple apps: `Wowfunhappy/Celeste-64-Patched-For-Mavericks`,
whose `COMPAT_WRITEUP.md` is the best operation-by-operation source found and
whose `patch_macho.c` is this repo's own ancestor, and `landonf/XcodePostFacto`,
which does the equivalent work **in memory** at image-state-change time and so
never writes or re-signs anything. No iMovie, GarageBand or Photos backport
appears to exist. `Wowfunhappy/Pages-Mavericks-Workaround` looks like a hit and
is not — it patches iWork '09 on Mavericks, an old app on a newer OS.

**The gaps, ranked.** The first two are one subsystem and are what their whole
approach rests on:

1. **Edit an existing `LC_DYLD_INFO[_ONLY]` bind stream** — OR
   `BIND_SYMBOL_FLAGS_WEAK_IMPORT` into a named symbol's trailing-flags byte, so
   a missing symbol becomes a weak import instead of a load failure. Size
   preserving, so no `grow` or `__LINKEDIT` interaction. We already write that
   exact byte in `src/declassify.c` when synthesising a stream; what is missing
   is any way to reach into one that already exists.
2. **Rewrite a bind entry's dylib ordinal to `BIND_SPECIAL_DYLIB_FLAT_LOOKUP`**,
   padding the shortened ULEB with `SET_TYPE_IMM(BIND_TYPE_POINTER)` (`0x51`)
   no-ops to keep the stream's byte length identical. Same walker and selectors
   as gap 1; the filler trick is worth lifting verbatim. Together, 1 and 2 are
   the difference between making a modern image *loadable* and making it
   *satisfiable by a stub dylib*.
3. **`LC_LOAD_WEAK_DYLIB`** — emit it on `dylib append`/`insert`, and flip an
   existing `LC_LOAD_DYLIB` to weak and back. The flip is one `uint32_t` write
   with no size change and no ordinal movement, since `mo_is_dylib_lc` already
   counts both kinds. This is also the last flag-axis gap against
   `insert_dylib --weak`, so it belongs with item 12.
4. **`minos set`** — rewrite an existing `LC_VERSION_MIN_MACOSX.version` or
   `LC_BUILD_VERSION.minos` rather than only appending or deleting. Their
   `patch_min_version.py` exists because Keynote 9's bundled frameworks each
   declare 10.13 in a load command that is otherwise 10.9-parseable; our
   `minos` is a no-op when one is present, and `lc delete build-version` throws
   the information away instead of correcting it.
5. **`section retype`** — normalize `S_NON_LAZY_SYMBOL_POINTERS` /
   `S_SYMBOL_STUBS` to `S_REGULAR`. One `flags` write per section, but resolve a
   tension first: `grow` refuses an unrecognized section type, so the two
   features must agree on ordering.
6. **Relative → absolute ObjC method lists.** Not a statement, a subsystem: new
   segment and section, 12 → 24-byte entries, `entsize` change, fresh rebase
   entries in a writable segment. Per Celeste's writeup this is what stands
   between `declassify` succeeding and a modern Obj-C binary actually running.
   Wants its own spec.
7. **Machine-readable import reporting.** Both projects' hardest work is the
   *diff*, not the patch — enumerate every `(install_name, symbol)` a binary
   imports and ask the live 10.9 loader which are missing. Our `info` is
   structural. Emitting the import list as parseable output is also the natural
   on-ramp to gaps 1 and 2, since that manifest is exactly their selector list.

**Where their practice bears on our refusals:**

- **`FAT_MAGIC_64`: they parse it, and for their operation class they are right
  — because every edit they make preserves byte length inside a slice.** That
  argues for accepting `fat_arch_64` for the size-preserving statements while
  keeping it refused for `grow`, `declassify`, and any append that overflows
  slack, which genuinely need `fat_arch_64.offset`/`.size` rewritten. Caveat
  worth carrying: nothing in their corpus actually is `fat_arch_64`, so their
  support is untested.
- **32-bit: no challenge.** Their code path exists but every target is
  x86_64-only and nothing exercises it. Dead code on their side; our refusal
  stands. (The condition that *would* reopen it is recorded in
  `docs/prior-art.md`.)
- **One place we are already right where they are wrong:** their
  `--prepend-dylib` path shifts load commands down without renumbering
  `SET_DYLIB_ORDINAL` opcodes — a latent bug no shipped script of theirs
  invokes. Our `dylib insert` renumbers.

**A documentation gap this turned up, for item 6.** Ad-hoc re-signing
(`codesign --force --deep --sign -`) is **mandatory** in their flow, not
optional: their installer aborts when `codesign --verify` fails, because a
modified bundle carrying Apple's original signature is rejected at exec. Since
`machotool lc delete codesig` makes re-signing unavoidable, our README's
capability list has an undocumented dependency — a user could follow it
exactly and end up with a bundle the kernel kills. The README should say that
re-signing is a required external step, and what it costs: ad-hoc signing drops
private entitlements (their iCloud sync does not work, and CloudKit had to be
neutered at runtime because amfid rejects an ad-hoc binary that keeps iCloud
entitlements). Note SIP and Gatekeeper never obstructed them, by design —
everything is written inside the `.app`, and the one system-framework
substitution is bundled and reached through an injected `@executable_path`
`LC_LOAD_DYLIB` rather than replacing the system copy.

## Item 14: spike — weaken binds in memory at load time

Raised by the repo owner 2026-09-12, reading item 13's note that
`landonf/XcodePostFacto` does equivalent work in memory and so never writes or
re-signs anything. A **spike**: the output is an answer, not code we keep.

**Why it is worth asking.** It dissolves the one item 13 finding that no amount
of load-command coverage fixes. Nothing on disk changes, so the code signature
stays valid: no ad-hoc re-sign, no dropped private entitlements, no CloudKit
workaround, no SIP question, and undoing it means not injecting. XcodePostFacto
never touched the app bundle at all, because it is a *launcher* — it sets
`DYLD_INSERT_LIBRARIES` and execs the target, so even `Info.plist` stays
untouched, and editing `Info.plist` is precisely what forced their re-sign.

**Confirmed available here 2026-09-12:** `_dyld_register_image_state_change_handler`
is exported from 10.9.5's `/usr/lib/system/libdyld.dylib` (build 13F1911). The
declaration is not in the SDK — `dyld_priv.h` ships with dyld's source, not
Xcode — so a caller declares it itself.

**What it does not avoid.** The operations are item 13's gaps 1 and 2 —
weakening an import, and rewriting its ordinal to flat lookup. In-memory
relocates that work rather than removing it. The opcode walker is reusable; the
*locating* arithmetic is not, because this repo's buffer-level operations assume
file layout, and `LC_DYLD_INFO.bind_off` is a file offset: in a mapped image you
need `__LINKEDIT`'s vmaddr plus the slide. `__LINKEDIT` is also mapped
read-only, so patching means `vm_protect` out and back.

**The question that decides feasibility, and is genuinely open:** which images
can a handler still reach in time? Registered from an inserted library, it fires
for images mapped *after* it — whether it can still catch the **main
executable**, which dyld may already have bound by the time our constructor
runs, is unknown. XcodePostFacto's targets were frameworks Xcode loads later,
which does not settle the general case. If only later-loaded images are
reachable, this is a technique for framework-shaped problems, not a general
porting tool.

**The probe.** Declare the SPI, register at `dyld_image_state_dependents_mapped`,
and try to weaken one bind against a test binary that links a deliberately
missing symbol — once where the symbol is in a `dlopen`ed dylib, once where it
is in the main executable. Report which worked. Also worth noting the blunt
alternative and why it is not this: `DYLD_FORCE_FLAT_NAMESPACE` abandons
two-level namespace wholesale, which is presumably why XcodePostFacto rewrote
individual binds instead.

**If it works,** the shape is two front ends over one walker: one writes a file,
one patches mapped memory. That is the same split this repo already has between
file-level and buffer-level operations, so it is not a new architecture — but a
runtime injector is a new *kind* of artifact, with a test strategy that cannot
be "hash the output file", so it wants its own spec rather than being folded
into item 13.

### Spike result, 2026-09-12: **no.** Do not build this.

Run natively on 10.9.5 (`dyld-239.5`). Full evidence in
`.superpowers/spike-inmemory-bind-weakening.md`.

| case | reachable in time? | weakening effective? |
|---|---|---|
| `dlopen`ed dylib | **yes** — handler fires for just that image | **yes** — refs resolve to 0; guarded code survives, unguarded SIGSEGVs |
| main exe, non-lazy (data) | **no** — dyld aborts inside `link()`; the injected constructor never runs | n/a |
| main exe, lazy (function) | **yes** — stream patched `0x40`→`0x41` and read back as weak | **no** — dyld's stub-time binder ignores the flag |
| load-time dependency dylib | **no** — dependencies bind before the main executable | n/a |

**Two complementary walls: everything reachable is ineffective, everything
effective is unreachable.** The decisive one is that dyld's lazy-binding path
ignores `BIND_SYMBOL_FLAGS_WEAK_IMPORT` altogether — proven not to be a
patching artifact, since a genuine `__attribute__((weak_import))` function
called unguarded dies the same way (`dyld: lazy symbol binding failed`). And
guarding a function reference forces it *non-lazy*, because you take its
address — so "reachable in time" and "guarded" are mutually exclusive. What
timing left open, dyld's binder closes.

**A finding that bears on item 13's gaps 1 and 2, which are the ON-DISK version
of the same edit.** Those gaps survive this result — patching the file puts the
flag in place before dyld ever looks, so the timing problem is specific to
memory. But the spike sharpens *why* weakening alone is not the win:
`if (&sym)` against a strong import is **compiled away** (`movb $0x1,%cl;
testb $0x1,%cl` even at `-O0`), so a binary whose source never used
`weak_import` has no guard to satisfy. Weakening such a bind converts a clean
launch-time abort into a SIGSEGV at the use site — worse, not better. Gap 1 is
only useful *with* gap 2: the symbol must end up resolving to a real stub, not
to 0. Any spec for gaps 1–2 should state that as a precondition.

## Item 15: flat-namespace shim — satisfy missing symbols at runtime

Came out of item 14's spike, which found it while proving the other approach
dead. `DYLD_FORCE_FLAT_NAMESPACE=1` plus an inserted shim dylib that *defines*
the missing symbols rescued **all six** probe variants, main executable
included, with no memory patching and nothing written to disk at all.

Why this is worth a design rather than a shrug:

- **Nothing on disk changes, so the signature survives.** Verified: `md5`
  identical and `codesign -v` still passes, even with `CS_KILL` set. This is the
  property that motivated item 14 in the first place, and this route delivers it
  without the patching.
- **No bundle edit is needed.** `open` forwards `DYLD_INSERT_LIBRARIES` through
  LaunchServices on 10.9 — verified with two distinct values plus an unset
  control — so `Info.plist` stays untouched, and XcodePostFacto's launcher app
  may have been unnecessary.
- **It inverts the work.** Instead of rewriting a binary so its imports become
  satisfiable, supply the imports. The hard part stops being Mach-O surgery and
  becomes *knowing which symbols to stub and what they should do* — which is
  what item 13's gap 7 (machine-readable import reporting) exists to answer, and
  where this repo's `info` verb is the natural on-ramp.

Costs and blockers to settle in the design, not discover later:

- **Process-wide flat lookup** is the price. Two libraries exporting the same
  name now collide where two-level namespacing kept them apart — a real hazard
  in a large app with bundled frameworks, and the reason XcodePostFacto
  presumably rewrote individual binds instead. Whether that is tolerable is the
  central design question.
- **A `__RESTRICT` segment is a hard blocker**: it strips every `DYLD_*`
  variable, so nothing is injected at all. Determine how common that is in the
  target population before promising anything.
- A stub that returns the wrong thing is worse than a missing symbol, because it
  fails later and less legibly. The design needs a story for what a stub does
  when it cannot do the real work.

This is a different product from `machotool` — a runtime library plus a launch
wrapper, not a file transformer — so it gets its own spec, and its verification
cannot be "hash the output file".

## Carried out of item 3

**The multi-family wrappers leak the compat temp's name on stderr.** Since the
report became unconditional, a multi-family `change_dylib` or `fix_macho` run —
the invocations that translate to one `machotool edit` — ends its stderr with a
line naming the hidden temp:

    ./.mf.machotool-compat.37920: written (8,504 bytes)

That is the same path `mw_run_to_tmp` already filters out of *stdout*, and
hiding it is the entire reason the wrapper's install mechanism exists. Measured
cosmetic: nothing gates wrapper stderr byte-for-byte and all 182 wrapper
assertions pass with the line present. Single-family runs are unaffected —
they reach `machotool dylib`, which produces no report at all.

**Where the fix belongs, and why not the obvious one.** In the wrapper, not in
`edit`: `edit` naming its `OUT` is correct behaviour, since `OUT` is what the
caller of `machotool edit` asked for — the wrapper is the party that chose a
hidden temp as `OUT`. Extend `mw_run_to_tmp` so the one predicate that already
knows `$MW_TMPFILE` filters both streams, rather than teaching a second place
the temp's name. Do **not** suppress the edit run's stderr wholesale: real
diagnostics share that stream, and one was caught in the same capture
(`machotool: no load command of kind build-version to delete`, from
`fix_macho`), which a caller should see.

It needs a stream swap (`3>&1 1>&2 2>&3`) around the `awk` in the most
depended-on wrapper, plus a new `wrapper_test` assertion pinning the temp's
absence from stderr — a new construct and a new gate, which is why it was not
bought with the last of item 3's verification budget. `tests/characterize.sh`'s
own console output shows the same line, and is a convenient reproduction.

**A latent collision worth knowing before anything else emits `target`.**
`me_log_derived` and the empty-expansion line emit **four-space-indented**
report lines, and since the report became unconditional those go to stderr
always. `tests/wrapper_test.sh`'s taught-block extractor pulls commands out of
wrapper stderr with `awk '/^    /{sub(/^    /,""); print}'` — also four spaces.
No collision exists today, because no wrapper translates to a `target`
statement and the report's statement echoes are two-space indented (confirmed
by running that awk over a real multi-family stderr: only the two taught lines
come back). But if a wrapper ever emits `target`, its expansion listing would
be captured as taught commands. Either indentation is the thing to change then.

## For shipyard: two gaps a self-upstream repo falls through

Found 2026-09-12 while making this repo releasable. Both are shipyard's, not
this repo's, and both are the same shape: machinery written for the **port**
case that silently does nothing for a repo that is its own upstream.

- **`previous-release-tag.sh` globs `*-mavericks.*`.** An `X.Y.Z` tag can never
  match, so `PREV` is permanently empty here and **no release body will ever
  carry its `[All changes since X](…/compare/…)` footer** — not just the first
  one. Verified against a scratch repo tagged `0.1.0`, `0.2.0`, `backup/foo`:
  every lookup returns empty. Teaching it the self-upstream shape would give
  releases 2+ their compare link.
- **`release-notes-file.sh` hardcodes "Requires Mac OS X 10.9.5 or later"** —
  the `.pkg` floor — while these binaries target 10.9. Harmless for a repo that
  ships a `.pkg`; wrong for one that ships bare binaries.

Neither blocks a release. Both want a shipyard change rather than a local
workaround, since a workaround here would be the fourth copy of a thing that
should live in one place.

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
