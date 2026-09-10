# `machotool`, and the `target` statement

**Status:** design, agreed 2026-09-10.

**Sequenced after** `docs/superpowers/specs/2026-09-10-edit-scripts-design.md`. That
design establishes the edit-script language; this one renames the tool that runs
it and adds the one statement whose meaning depends on the binary.

## Why these two things are one change

`macho9`'s `9` is a claim the tool is about to stop making. The moment a target
becomes a parameter — `target 10.9` — an OS baked into the binary's name
contradicts the design. Doing the rename separately would mean shipping a name
that is wrong on arrival, or delaying the profile until someone gets around to
renaming.

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
prefixes (`mi_`, `mr_`, `mg_`, `mo_`, `mseg_`, `mswift_`, `wa_`) also stay: they
name modules, not the tool.

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

## Steps the repo owner takes

Three things this plan cannot do for itself. **Do all three before starting a new
session in the renamed clone**, and in this order.

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

## Execution note: the behaviour matrix must be re-run, not rewritten

`tests/compat-matrix.tsv` records 1,227 emitted command lines containing
`macho9`. It must be **regenerated against two builds**, not `sed`-ed. The sweep
script once used one bindir for both sides — measuring a build against itself —
and a mechanical rewrite of the recorded output is exactly the shape that would
hide that class of problem again.

## Out of scope — and a gap found while writing this

**Release and versioning conformance is its own piece of work.** Consulting the
family conventions surfaced a real inconsistency that this design does not fix:

- `UPSTREAM_VERSION` holds `0.1.0` and `release.yml` cuts `<upstream>-mavericks.N`
  — the machinery for a repo that **ports an external upstream**.
- But `docs/PROPOSAL.md` settled that this repo is **first-party**: *"one repo,
  first-party, no `UPSTREAM_VERSION`."* The conventions are explicit that a
  self-upstream repo **drops the `-mavericks` suffix** and versions itself
  directly (`YYYYMMDD.N` or semver), computes its version in `release.yml`
  rather than through `resolve-version.sh`, and hand-bumps its own pin because
  there is nothing external for Renovate to track.
- Also absent: `renovate.json`, `release-notes/`, and the `build/` script
  wrappers (`lib.sh`, `version.sh`, `release-notes-file.sh`) the conventions
  checklist expects.

None of this blocks the rename or the `target` statement, and folding it in would
mix two unrelated concerns. **It does block cutting a release**, which is why it
is written down here rather than discovered at release time.

Also out of scope: any target profile other than 10.9, and `--for`-style
command-line invocation, which "the script is the plan" rules out.
