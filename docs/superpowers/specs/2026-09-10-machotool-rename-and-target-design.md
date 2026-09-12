# `machotool`, and the `target` statement

**Status:** design, agreed 2026-09-10. Name reconsidered and kept 2026-09-12.

**Sequenced after** `docs/superpowers/specs/2026-09-10-edit-scripts-design.md`. That
design establishes the edit-script language; this one renames the tool that runs
it and adds the one statement whose meaning depends on the binary.

## Why these two things are one change

`macho9`'s `9` is a claim the tool is about to stop making. The moment a target
becomes a parameter — `target 10.9` — an OS baked into the binary's name
contradicts the design. Doing the rename separately would mean shipping a name
that is wrong on arrival, or delaying the profile until someone gets around to
renaming.

## The name was reconsidered once, and kept

Before item 3 started, the repo owner asked to weigh Mavericks-themed names —
`shipyard`, `porthole` and that vein — against `machotool`. Candidates came from
two directions: the surf break the OS is named for (`shaper`, after the trade
that shapes a blank into a board; `stringer`; `bombora`) and the shipyard
(`drydock`, which sharpened once this toolkit stopped editing in place, since a
drydock is precisely where you take a vessel out of service to reach what you
otherwise cannot; `careen`; `shipwright`).

**`machotool` won, 2026-09-12.** What the themed names cost is the thing the
name is for: `machotool` says *Mach-O* to someone who has never heard of this
project, and every themed candidate trades that for character. Two of the
suggested names are also taken — Shipyard is a Docker management UI, Porthole is
Pi-hole's dashboard — which is a hazard the whole vein shares.

Recorded so the question is settled rather than reopened. Nothing in this
document changed as a result.

## The rename

| what | from | to |
|---|---|---|
| binary | `macho9` | **`machotool`** |
| repo | `macho-tools` | **`machotool`** |
| package | `mavericks-macho-tools` | **`mavericks-machotool`** |
| library target | `macho9core` | **`machotoolcore`** |
| shared wrapper script | `macho9-compat.sh` | **`machotool-compat.sh`** |
| translator | `macho9-translate.sh` | **`machotool-translate.sh`** |
| app identity | — | **"Mavericks Machotool"** |
| prose name | — | **"Machotool for Mavericks"** |
| bundle id | — | **`dev.modernmavericks.machotool`** |

The two naming registers and the bundle-id form come from the family
conventions: app identity is brand-forward ("Mavericks Foo") for the `.app`
bundle, `CFBundleName`, and the Sparkle `PRODUCT_NAME`; prose is descriptive
("Foo for Mavericks") for the `.pkg` title, appcast channel title, and README.

**`machotool`, not `machoedit` or `macho-edit`,** because the verb is `edit` and
a binary named for editing stutters: `macho-edit edit FILE` reads badly. The name
wants to be a noun for the domain, like `otool` and `install_name_tool` — the
Apple tools it sits beside — with the verbs doing the work.

**Repo renamed too, because after the compat retirement this repo holds one
tool.** "Tools" plural was accurate while six historical binaries lived here; it
stops being accurate when they are gone. The only other deliverable is
`src/live.h`, and that is a library for avxemu, not a second tool.

**What does NOT change:** the six historical wrapper names — `patch_macho`,
`change_dylib`, `add_version_min`, `fix_macho`, `rename_segment`,
`retag_swift_classes`. They are the interface callers actually invoke, and
`mavericksforever.com/claude/install.sh` fetches three of them by name. Module
prefixes (`mi_`, `mr_`, `mg_`, `mo_`, `mseg_`, `mswift_`, `wa_`, plus the `ms_`
and `me_` the edit-script work adds) also stay: they name modules, not the tool.
That they are hard to read is acknowledged and deferred — see "Out of scope".

## `target 10.9` — a third kind of line

The language has **directives** (`allow-grow`, `fatal-warnings`), which describe
the run and must precede operations, and **operations**, which edit the binary.
`target` is a third kind, and it earns the category on a property neither of the
others has:

> **It is the only line whose meaning depends on the binary.** `dylib replace A B`
> means the same thing for every input — it may match nothing, but what it asks
> for is fixed. `target 10.9` asks a different question of every binary and
> answers it differently.

It is the intent level, arriving as a named line rather than as hidden behaviour.

### It expands where it is written

```
# zoom.edits
allow-grow

target 10.9

dylib replace /System/Library/Frameworks/Metal.framework/Versions/A/Metal  @loader_path/libMetalStub.dylib
rpath  insert  @loader_path/../Frameworks
```

Position is not cosmetic. `fixups set classic` strips `LC_BUILD_VERSION` and
rewrites `__LINKEDIT`, which changes the header pad available to every
`dylib replace` after it. Since the tool does not reorder statements — the script
is the plan — the operator has to control where the expansion lands, which means
it must be a statement and not a flag.

### What it covers

Two of the four reasons to run a statement are detectable from the binary alone.

**Won't load** — dyld refuses the image:

| detected | expands to |
|---|---|
| `LC_DYLD_CHAINED_FIXUPS` present | `fixups set classic` |
| `LC_BUILD_VERSION` present | `load-command delete build-version` |
| no `LC_VERSION_MIN_MACOSX` | `version-min set 10.9` |

**Loads, then silently misbehaves** — the runtime accepts it and does the wrong
thing, which is the worse failure because nothing announces it:

| detected | expands to |
|---|---|
| `__DATA_CONST` carrying `__objc_*` sections | `segment rename __DATA_CONST __DATA` |
| class records carrying the stable-ABI Swift tag | `swift-abi set legacy` |

**Never**: `dylib` or `rpath` work. No tool can guess which stub dylib you meant,
and that is the dominant real workload.

Detection is exact rather than heuristic in every case above — a load command is
present or it is not; a tag bit is set or it is not. `swift-abi`'s detection is
already how the existing implementation decides which records to touch.

### Rules

- **One `target` per script.** A second is a parse error.
- **An unknown target is a refusal.** `target 10.10` errors until someone adds
  the profile, rather than silently doing 10.9's work.
- **`target` never counts as unmatched under `fatal-warnings`.** "This binary
  already targets 10.9 correctly" is a correct answer for a profile, unlike for
  an explicit operation.
- **Writing `target 10.9` *and* an explicit statement it would have derived makes
  the explicit one redundant, and `fatal-warnings` will flag it.** That is right,
  and is documented rather than special-cased.

## Always verbose, on stderr

**There is no `--verbose` flag, because there is no quiet mode.** A tool whose job
is to make edits nobody can see afterwards should not have an option to say
nothing about them.

> **Conflict with the edit-scripts design, and how it resolves.** That document
> (`2026-09-10-edit-scripts-design.md`) shows `macho9 edit --verbose` in two
> worked examples, and its plan builds the flag — `me_opts.verbose`, a
> `--verbose` argument, and assertions on both. Since this design is sequenced
> *after* that one, as written the sequence builds a flag and then deletes it.
>
> **This design removes it**, and that is the resolution of record. But the
> cheaper fix is available while the edit-scripts plan remains unstarted:
> **never build it.** `me_opts` keeps its `log` field, the report is
> unconditional, and the two worked examples drop the flag from their command
> lines without changing a line of their output. Whoever executes the
> edit-scripts plan should take that option if it has not yet begun; if it has,
> the deletion lands here as described.

This is simplification, not just policy: one output path, no flag, no "did they
pass `-v`" branch to test.

**The report goes to stderr**, which is what makes it free:

- the compat wrappers' **stdout** stays byte-identical to the C tools they
  replaced, which is a hard requirement carried from the retirement plan;
- `machotool edit` itself writes **nothing** on stdout, so it is pipe-safe;
- `info` and `verify` keep stdout for their data, which is genuine output rather
  than progress;
- anyone who wants silence has `2>/dev/null`, the Unix answer, which needs no
  flag of ours.

The report must include the follow-up work operations perform on their own — the
ordinal renumbering in both the `nlist` entries and the `SET_DYLIB_ORDINAL*`
opcode streams — because that is the part a user cannot see for themselves. It
must also include `target`'s expansion, line by line, since that line does
different things to different binaries.

## Steps the repo owner takes — deferred to just before the history rewrite

Three things this design cannot do for itself. **They do not happen with this
design's work.** They are queue item 7's, and `docs/superpowers/QUEUE.md` carries
the reasoning; the short version is that this design renames the *product*, which
is entirely in-tree, while these three rename *where things live*, and nothing in
the repo couples the two:

- no file outside `docs/` keys on the clone's directory name — the only
  `macho-tools` strings are `CMakeLists.txt`'s status messages and
  `release.yml`'s artifact name, both of which this design renames as product
  names wherever the clone sits;
- the `/private/tmp/mm-build/…` build directories are a chosen convention, not
  derived from the repo path by any shipyard script, so moving the clone costs
  one `cmake` reconfigure (their `CMakeCache.txt` holds absolute source paths);
- both checkout roots are the same filesystem object, so moving either moves both.

Deferring them groups this disruption with the history rewrite, which is the
other change that invalidates infrastructure — every commit SHA these documents
and the SDD ledgers cite.

**The order within the pair is still invariant**, and steps 1 and 2 must happen in
one sitting with no session live in that directory.

**1. Move the agent's project directory first.** It is keyed to the working
directory's path, so renaming the clone without moving it orphans four memories
and every transcript of this work:

```sh
mv ~/.claude/projects/-Users-schmonz-Documents-code-trees-mavericks-macho-tools \
   ~/.claude/projects/-Users-schmonz-Documents-code-trees-mavericks-machotool
```

**2. Rename the local clone.** Note both roots are the same filesystem object
(identical inode), so this moves both views at once:

```sh
mv ~/Documents/code/trees/mavericks-macho-tools \
   ~/Documents/code/trees/mavericks-machotool
```

**3. Rename the GitHub repo** — `ModernMavericks/macho-tools` →
`ModernMavericks/machotool` — and update the local remote. This is outward-facing
and yours; GitHub redirects the old URL, so nothing breaks in the meantime, and
every other step of the plan lands without it.

It does **not** affect the adoption path: `install.sh` fetches `patch_macho`,
`change_dylib` and `add_version_min` by name from Wowfunhappy's repo, and none of
those names change.

## Execution note: the behaviour matrix is a dated artifact, and is left alone

**This section previously said the matrix "must be regenerated against two
builds, not `sed`-ed". That instruction is now unexecutable**, and the reason is
worth keeping rather than quietly deleting.

`tests/compat-matrix.tsv` records 1,209 emitted command lines containing
`macho9`. The original instruction existed because the sweep script once used one
bindir for both sides — measuring a build against itself — so a mechanical
rewrite of recorded output is exactly the shape that would hide that class of
problem again. That reasoning still holds; what changed is that **regeneration is
no longer possible.** The old side of that comparison was the six historical C
tools, and after the compat retirement `compat/` contains no `.c` file at all. A
re-run would measure something different and quietly relabel a historical record.

So the matrix is **left exactly as it is**, data rows untouched. Its header
already carries a dated scope note saying what was measured, against what, and
which rows are now historical — added when the retirement landed, precisely so a
reader who arrives after a rename is not misled by command lines naming a binary
that no longer exists.

**What the rename does to it: nothing.** The `macho9` strings in those rows are
part of the measurement, not references to a live binary. Renaming them would be
falsifying a record. If the stale names bother a future reader more than the
falsification would, the honest options are to delete the file or to write a
fresh measurement under a new name — not to `sed` this one.

## Out of scope

**Release and versioning conformance.** Writing this design surfaced that
`UPSTREAM_VERSION` and `release.yml` carry the machinery for a repo that ports an
external upstream, while `docs/PROPOSAL.md` settled that this repo is
first-party. That finding has since become its own design —
`2026-09-10-release-conformance-design.md` — and is queue item 4, sequenced
immediately after this one because `release.yml`'s artifact list names all three
of the files this design renames. It is not restated here; the pointer is the
whole of it.

**The module prefixes.** `mi_`, `mr_`, `mg_`, `mo_`, `mseg_`, `mswift_`, `wa_`,
and the `ms_`/`me_` the edit-script work adds, are opaque to a reader who has not
learned them — the repo owner said so directly. This design deliberately keeps
them (see "What does NOT change" above) because a rename of the tool is not the
occasion to relitigate every internal name. The question is logged against queue
item 6, the human code review and documentation pass, which is where a
readability decision of that size belongs.

**Any target profile other than 10.9**, and `--for`-style command-line
invocation, which "the script is the plan" rules out.
