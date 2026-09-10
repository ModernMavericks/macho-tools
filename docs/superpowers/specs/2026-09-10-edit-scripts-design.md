# `macho9 edit` — edit scripts

**Status:** design, agreed 2026-09-10. Supersedes the `port` verb sketched in
`docs/PROPOSAL.md`, including its name.

**Depends on:** Task 0 of
`docs/superpowers/plans/2026-09-10-report-what-macho9-did.md`. Verification is
currently blind on every dylib and bundle, so a mandatory verify gate would be a
mandatory rubber stamp until that lands. See "Verification" below.

## The problem, measured

`mavericksforever.com/claude/install.sh` patches a ~200MB Claude Code binary by
running three tools in sequence — `patch_macho`, then `add_version_min`, then
`change_dylib` — so **the binary is written three times**, and a failure in the
third leaves a half-converted file where the first two succeeded.

One of those invocations mixes two operation families in a single command:

```sh
change_dylib "$T" -strip-lc uuid -strip-lc codesig \
    -change "/usr/lib/libSystem.B.dylib"  "@loader_path/../S.dylib" \
    -change "/usr/lib/libicucore.A.dylib" "@loader_path/../I.dylib" \
    -change "/usr/lib/libc++.1.dylib"     "@loader_path/../c++.1.dylib"
```

The stripping is what makes room for the rewriting. Under a
subcommand-per-family CLI that becomes two invocations, `lc` then `dylib`, and
`docs/PROPOSAL.md` predicted the cost. The compat-tool retirement then measured
it: two rows of the 1227-row behaviour sweep came back where the C tool refused
**atomically** while the translated sequence refused **after already writing**.

The shipped mitigation is shell: `compat/macho9-compat.sh` copies the target
beside itself, runs the sequence against the copy, and installs on full success.
That restores atomicity but not the single write, and it costs a third copy —
peak footprint ~3x the file where the C tool needed 2x.

## Who this is for

This sets the design pressure, and two earlier drafts of it were wrong — first
arguing from "one Claude binary", then from "the ModernMavericks family". Both
undershot.

**The population is arbitrary modern macOS applications: shipped as binaries,
no source, no control over the vendor.** Built by toolchains fifteen years newer
than the OS they must load on, by people who have never heard of 10.9 and will
ship a new version next month regardless. Binary surgery is not the preferred
lever, it is the only one.

**Today's automated callers are few** — `mavericksforever.com/claude/install.sh`,
one local script in `mavericks-claude-ongoing`, and this repo's own suites. That
was established from evidence, not assumption.

**But attempts on other applications are already in the upstream history.**
`Wowfunhappy/Mavericks-Porting-Resources` carries `Electron stuff` (`38cb0b8`),
`Some attempts at Zoom.` (`602fb89`) and `More attempts at Zoom` (`8e82349`),
alongside the Claude Code work. Its `framework-stubs/README.md` states the
intent outright: *"Stub and wrapper dylibs that let binaries built for newer
macOS load on 10.9. **Nothing here is tied to a particular project.**"* So the
gap between "few callers" and "arbitrary applications" is not aspiration — it
is work already attempted, by hand, on apps nobody has source for.

Four consequences, each of which this design has to answer for:

**The refusal surface is the product.** An arbitrary binary carries load
commands, `__LINKEDIT` layouts and pointer formats nobody here has seen. Every
shape the toolkit does not understand must produce a clear refusal naming what
it did not understand — never a plausible-looking result. "Refuse rather than
guess" is not fastidiousness under these conditions; it is the entire safety
argument, because the alternative is handing someone a binary that loads and
then misbehaves.

**An edit script has to survive version churn.** The vendor ships 1.3 where the edit script
was written against 1.2, and the paths, the ordinals and the load command set
all moved. So an edit script is an artifact that gets re-run against inputs it was not
written for — which makes *an operation matching nothing* the normal case rather
than the exceptional one, and makes reporting it (and `fatal-warnings`) load
bearing rather than a nicety. See the companion plan,
`docs/superpowers/plans/2026-09-10-report-what-macho9-did.md`.

**Diagnosis matters as much as editing.** Nobody knows what surgery an unfamiliar
binary needs until they look, and 10.9's own `otool` prints
`?(0x80000034) Unknown load command` for most of what a modern linker emits. So
`info` and `verify` are not conveniences alongside the rewriting verbs; they are
how an edit script gets written in the first place.

**Centralising the relations stops being tidiness.** One known binary can be
served by knowledge scattered across whichever functions need it. An open-ended
population of unfamiliar shapes is where "only one place to put the knowledge"
becomes the thing that keeps the toolkit correct as coverage grows.

What this does **not** change: arbitrary *applications*, not arbitrary
*formats*. Modern macOS applications are Mach-O. See "Two generalizations, not
one".

## The dominant workload: repointing frameworks at stubs

The Zoom and Electron work in the upstream history reveals what porting an
arbitrary application actually consists of, and it is not three dylib
replacements. It is **framework substitution at scale**.

A modern application binds symbols from frameworks 10.9 does not have
(CoreSpotlight, UserNotifications, AuthenticationServices, Metal, CryptoKit,
Network, Vision, NaturalLanguage…) or does have but whose 10.9 copy lacks
symbols added since. Upstream's answer is a directory of stub, wrapper and
hand-written-shim dylibs, and the step that connects them to the binary is ours.
`framework-stubs/README.md`:

> *"Consumers repoint the binary's `LC_LOAD_DYLIB` at the stub/wrapper —
> placing it beside the binary and referencing `@loader_path/<lib>` keeps the
> path shorter than the `/System/...` one it replaces, so no header padding is
> needed."*

That is `dylib replace` — the single most-used operation in this toolkit, and
the reason `allow-grow` is opt-in and rarely necessary: `@loader_path/libFooStub.dylib`
is shorter than `/System/Library/Frameworks/Foo.framework/Versions/A/Foo`, so
the replacement fits the command it replaces.

Three consequences for this design.

**Edit scripts are the right artifact, and the scale is why.** Upstream's
`framework-stubs/frameworks.json` describes **32 frameworks** — 18 wrappers, 12
stubs, 2 re-exports. An application needing most of them repointed is an edit script
of dozens of lines. As a command line that is unreadable and unreviewable; as a
committed file it is a diff, per application, that someone can inspect and
re-run when the vendor ships an update.

**The 32-operation cap is exactly at the real workload, not comfortably above
it.** `MR_MAX_OPS` is 32, and `-replace`, `-delete` and `-reexport` share that
one array. The manifest lists 32 frameworks. The cap was sized for a tool that
did three replacements from argv; the actual job sits on its boundary, and any
application needing one more framework than Zoom — or an edit script that also deletes
or re-exports something — exceeds it. **An edit script read from a file has no reason
to inherit an argv-shaped cap**: the fixed-size arrays exist because
`compat/change_dylib.c` accumulated into `changes[32]` on the stack. `edit`
should size from the parsed edit script. Note the caps must remain for the compat
wrappers, which reproduce the old tools' refusal exactly — this is a difference
between the two front-ends, not a lifting of the limit everywhere.

**The manifest is a precedent worth matching.** `frameworks.json` is already a
machine-readable description of per-application surgery, generated by scanning
real binaries for undefined symbols and the library each binds to. An edit script is
its companion — what to *do* to the binary, beside what the binary *needs* — and
the two being separate files, both committed beside the application they
describe, is a shape the family has already arrived at once.

Upstream's generator also states the ethic this toolkit shares, in almost the
same words: *"A missing C function is reported, never invented: a no-op that
returns the wrong answer is worse than a link error."*

## The boundary: when surgery is the wrong tool

`Wowfunhappy/WebKit`'s `mavericks-backport` branch is an actively-developed port
of WebKit to 10.9 (near-daily releases; pushed the day this was written). It was
checked for use of these tools and **uses none of them** — no `insert_dylib`, no
`change_dylib`, no `patch_macho`. What it does instead defines the boundary of
this toolkit better than any argument:

- It **builds its own cctools**, because *"`/usr/bin/{otool,lipo,install_name_tool,…}`
  are 14K xcselect shims … and `/usr/bin/dyldinfo` does not exist at all"*
  (`MavericksSupport/toolchain/scripts/build_cctools.sh`), then uses that modern
  `install_name_tool` for `-change`, `-delete_rpath` and `-id`
  (`scripts/stage-frameworks.sh`).
- It compiles everything at `-mmacosx-version-min=10.9`, so its linker emits
  10.9-compatible output in the first place. There is **not one reference to
  chained fixups anywhere in it** — nothing to lower, because nothing modern was
  ever emitted.
- It reserves header padding at link time with
  `-Wl,-headerpad_max_install_names` (`polyfill/build-polyfill.sh:278`) — the
  link-time counterpart of this toolkit's `grow` verb and `allow-grow`
  directive.

So the strategy is decided by one question:

| | with source | without source |
|---|---|---|
| target the old OS | `-mmacosx-version-min=10.9`; the linker emits compatible output | the vendor already emitted chained fixups and `LC_BUILD_VERSION`; someone must lower them after the fact |
| make room for longer paths | `-Wl,-headerpad_max_install_names` at link time | `grow` / `allow-grow`, by lowering the image base of a finished binary |
| adjust dependencies | a modern `install_name_tool`, on a binary its own linker just wrote | edit in place, never moving a byte, because nothing can relink it |

**WebKit is the source case. This toolkit exists for the other column** — and
the Zoom and Electron attempts sit squarely in it. That is not a gap in the
WebKit port; it is two different problems that look similar from a distance.

**One premise this raises, and it is untested.** `docs/PROPOSAL.md`'s "Why not
just use install_name_tool" measured **10.9's** `install_name_tool` refusing
these binaries (*"file not in an order that can be processed (dyld_info out of
place)"*). WebKit's port demonstrates that a **modern** `install_name_tool` is
obtainable on a 10.9 host — it builds one. Whether a modern one can process a
vendor-shipped binary like Claude Code has not been measured here. If it can,
the `-change`/`-add_rpath` part of this toolkit's justification is weaker than
`docs/PROPOSAL.md` states, and that section should be re-measured rather than
re-quoted.

What would remain ours either way: lowering chained fixups to
`LC_DYLD_INFO_ONLY`, ordinal renumbering that refuses rather than corrupts,
growing the pad of a finished binary, and the never-move-a-byte constraint that
`install_name_tool` violates by design when it rebuilds `__LINKEDIT`.

**Partly answered since, by `2026-09-10-toolchain-backport-survey.md`.** Modern
`cctools` does recognise the modern load commands — `libstuff/checkout.c`
handles `LC_DYLD_CHAINED_FIXUPS` and `LC_DYLD_EXPORTS_TRIE` — so a modern
`install_name_tool` may well open a binary 10.9's refuses. But it reaches the
file through `breakout` → `checkout` → `writeout`, which **re-emits it with
recomputed symbol-info sizes and drops or invalidates a code signature**. That
is not the byte-preserving edit this toolkit performs, so the two are not
interchangeable even where both succeed. Whether real Electron and Zoom binaries
pass `checkout.c`'s remaining ordering invariants at all is still untested.

The survey also settles the wider question: `vtool -set-version-min` replaces
`add_version_min` outright, and `install_name_tool -change`/`-id` replaces most
of `fix_macho` — but **no public project lowers chained fixups**. LIEF parses
all three formats and its `Builder` exposes no cross-conversion; every search
returns "relink with `-no_fixup_chains`", which a vendor binary forecloses. That
transform is the load-bearing justification for this repo, and `grow` is the
second: Apple's own answer when the header pad runs out is *"the program must be
relinked"*.

## What this design does

Adds one verb, `macho9 edit`, which takes an **edit script**: a flat sequence of
statements applied to one in-memory image, verified, and written once.

```
macho9 edit FILE SCRIPT                 # rewrite FILE in place
macho9 edit FILE SCRIPT --output OUT    # write elsewhere; FILE untouched
macho9 edit FILE -                      # edit script on stdin
```

`install.sh`'s three writes collapse to one. The mixed-family split disappears,
and with it the temp-copy dance and its extra 200MB.

## Examples

A script's extension is not enforced; `.edits` is used here for readability.

### The production case — three tools, three writes, become one

Today, from `install.sh`:

```sh
patch_macho     "$REAL" "$T"
add_version_min "$T"
change_dylib    "$T" -strip-lc uuid -strip-lc codesig \
    -change "/usr/lib/libSystem.B.dylib"  "@loader_path/../S.dylib" \
    -change "/usr/lib/libicucore.A.dylib" "@loader_path/../I.dylib" \
    -change "/usr/lib/libc++.1.dylib"     "@loader_path/../c++.1.dylib"
```

After — `claude.edits`:

```
# Claude Code -> 10.9
fixups        lower
version-min   set      10.9
load-command  delete   uuid
load-command  delete   codesig
dylib         replace  /usr/lib/libSystem.B.dylib   @loader_path/../S.dylib
dylib         replace  /usr/lib/libicucore.A.dylib  @loader_path/../I.dylib
dylib         replace  /usr/lib/libc++.1.dylib      @loader_path/../c++.1.dylib
```

```sh
macho9 edit "$REAL" claude.edits --output "$T"
```

Three full writes of a 208MB binary become one, and a failure at any statement
leaves `$REAL` untouched instead of `$T` half-converted.

### Framework substitution — the Zoom and Electron workload

```
# zoom.edits -- repoint frameworks 10.9 lacks at the stubs beside the binary
allow-grow

fixups        lower
version-min   set      10.9
swift-abi     set      legacy
segment       rename   __DATA_CONST  __DATA

dylib  replace  /System/Library/Frameworks/AVFoundation.framework/Versions/A/AVFoundation      @loader_path/libAVFoundationWrapper.dylib
dylib  replace  /System/Library/Frameworks/CoreSpotlight.framework/Versions/A/CoreSpotlight    @loader_path/libCoreSpotlightStub.dylib
dylib  replace  /System/Library/Frameworks/UserNotifications.framework/Versions/A/UserNotifications  @loader_path/libUserNotificationsStub.dylib
dylib  replace  /System/Library/Frameworks/Metal.framework/Versions/A/Metal                    @loader_path/libMetalStub.dylib
dylib  replace  /System/Library/Frameworks/Network.framework/Versions/A/Network                @loader_path/libNetworkStub.dylib
# ... 27 more, one per framework in frameworks.json

rpath  insert   @loader_path/../Frameworks
```

Thirty-two of these is a reviewable diff. Thirty-two of these as a command line
is not — and it would exceed the old 32-operation cap, which is why `edit` sizes
from the parsed script instead.

### Every CLI verb is sugar for a one-line script

```sh
macho9 dylib FILE -replace A B          # equivalent to:  dylib replace A B
macho9 load-command FILE -delete uuid   # equivalent to:  load-command delete uuid
macho9 minos FILE 10.9                  # equivalent to:  version-min set 10.9
```

### Generated on the fly, so no temp file

```sh
macho9 edit "$target" - <<'EOF'
load-command  delete   uuid
dylib         replace  /usr/lib/libSystem.B.dylib  @loader_path/../S.dylib
EOF
```

### Verbose — including the work you did not ask for by name

```
$ macho9 edit --verbose /tmp/claude claude.edits
/tmp/claude: header pad 96 bytes available (LC end=2784, first sect=2880)
  fixups lower
      chained fixups -> LC_DYLD_INFO_ONLY
      94,912 rebases and 3,181 binds emitted (462 KB of opcodes)
      stripped LC_DYLD_EXPORTS_TRIE, LC_BUILD_VERSION
      __LINKEDIT extended by 462 KB
  version-min set 10.9
      appended LC_VERSION_MIN_MACOSX 10.9.0 (+16 bytes)
  load-command delete uuid
      removed LC_UUID (-24 bytes)
  dylib replace /usr/lib/libSystem.B.dylib @loader_path/../S.dylib
      [56 -> 56 bytes] ordinal 1 unchanged
/tmp/claude: verified
/tmp/claude: written (208,526,708 bytes)
```

And where an operation carries follow-ups, the follow-ups are what the log is
for — this is the part a user cannot see for themselves:

```
  dylib delete @loader_path/libspare.dylib
      removed LC_LOAD_DYLIB (was ordinal 4)
      renumbered 3 surviving ordinals: 5->4, 6->5, 7->6
          12 nlist entries updated
          847 SET_DYLIB_ORDINAL opcodes updated (bind 811, weak 0, lazy 36)
```

### A refusal, and a dry run

```
$ macho9 edit /tmp/claude claude.edits
  dylib delete /usr/lib/libicucore.A.dylib
      ERROR: a symbol still binds to the dylib being deleted
macho9 edit: refused at statement 6 of 9; /tmp/claude left unmodified
$ echo $?
1
```

```
$ macho9 edit --dry-run --verbose /tmp/claude claude.edits
  ... every statement applied and reported, exactly as above ...
/tmp/claude: verified
/tmp/claude: NOT written (--dry-run) -- would be 208,526,708 bytes
$ echo $?
0
```

The dry run is accurate because it is the same run: only the write is skipped.

## Statements

```
load-command  delete    KIND        uuid | codesig | source-version
                                    | build-version | code-sign-drs
segment       rename    OLD NEW
version-min   set       10.9
swift-abi     set       legacy
fixups        lower
dylib         replace   OLD NEW
dylib         append    PATH
dylib         insert    PATH
dylib         delete    PATH
dylib         reexport  PATH
rpath         replace   OLD NEW
rpath         delete    PATH
rpath         append    PATH
rpath         insert    PATH
```

That is every rewriting operation the toolkit has, spelled as the existing verbs
with the file argument dropped. Nothing new is invented, so a reader who knows
the verbs can read an edit script.

### Some operations do only what they say; others carry their consequences

The one property worth knowing about this set, because it is the difference
between this toolkit and reaching for `install_name_tool`:

**Most operations are self-consistent.** Nothing in the binary points at what
they touch, so doing the edit is the whole job. `load-command delete uuid`
removes a command nothing references. `segment rename` rewrites name characters
and no offsets. `swift-abi set legacy` flips one tag bit per class record. `rpath` operations edit commands that carry no library ordinal.
`dylib replace` keeps the command's position and its ordinal.

**A few require follow-up work to leave the binary valid, and the operation does
that work as part of itself:**

- `dylib delete` renumbers every surviving library ordinal — in the `nlist`
  entries *and* in the `SET_DYLIB_ORDINAL*` opcodes of the bind, weak and lazy
  streams — and refuses outright if any symbol still binds to what you asked to
  remove.
- `dylib insert` renumbers every dylib after the one it inserts.
- `fixups lower` rebuilds `__LINKEDIT`'s bind and rebase streams wholesale.

The caller writes one line and gets the whole consequence. That is the point:
the alternative is doing the edit and finding out later that something else
needed updating, which is how `docs/PROPOSAL.md`'s defect #2 shipped — a
`-delete` that left ordinals stale and produced `dyld: library ordinal (4) too
big`.

**This is not a distinction the script language marks, and it does not need
to.** It is a property of the operations, not a vocabulary the writer chooses
between. The implementation derives which operations carry follow-ups from the
relation table below rather than from a hand-maintained list, so the two cannot
drift apart — and a relation added later moves the set with it.

## The design centre

Settled with the repo owner, 2026-09-10, because it decides questions this
document otherwise argues one at a time:

> **This is primarily a language for scripting Mach-O edits.** The language will
> grow as more needs and use cases surface. Being redundant with tools that
> already exist is acceptable, within reason, because the value is letting
> someone transform a binary **in one pass** rather than shelling out to a second
> or third tool mid-script.

That settles the two verbs the prior-art survey found replacements for.
`vtool -set-version-min` subsumes `version-min set`, and
`install_name_tool -change`/`-id` subsumes much of the dylib rewriting — and we
keep both anyway. Making a script author stop mid-script, run `vtool`, and come
back would defeat the single-write property that is the whole point, and would
reintroduce the multi-write failure mode described at the top of this document.
The redundancy is the feature.

What it does *not* license is redundancy for its own sake. A statement earns its
place by being needed inside a script; if the only reason to add one is that
another tool has it, that is not a reason.

### Why `swift-abi`, not `retag-swift`

An Objective-C class record carries a tag in the low two bits of its data word
saying "this is a Swift class", and which bit is used depends on the deployment
target of whatever emitted it: **bit 1 for 10.14.4+, bit 0 for anything older**.
The Swift runtime tests whichever bit *its own* target implies, and nothing
rejects a mismatch — a 10.9 runtime simply concludes the application has no
Swift classes, treats each as a plain Objective-C class, and takes the wrapper
path. Silent misbehaviour, not a crash.

So it is the same *kind* of operation as `version-min`: adjusting what a binary
claims about its deployment target so an older runtime accepts it. It is named
to match — `swift-abi set legacy` — rather than as a one-off verb.

**Not folded into `version-min`, deliberately.** Identification is precise
rather than heuristic (the tag bit is either set or it is not, so there is no
guessing about which records are Swift classes), so folding is *possible*. The
objection is that `version-min set 10.9` would then silently also rewrite class
metadata in `__DATA` — a consequence invisible in the line that caused it, which
is the shape this design exists to avoid. If bundling is wanted later, it belongs
in an intent-level statement that says it is bundling, not in `version-min`.

## Before any release

Versioning, packaging and release mechanics must match the ModernMavericks family
conventions before this repo cuts a release. That is a repo-level gate, not a
property of this design, and it is recorded here so it is not discovered at
release time.

## What this does to the CLI

If the language is the product, the existing verbs stop being a second grammar
and become **sugar for a one-line script**:

```
macho9 dylib FILE -replace A B        ==   dylib replace A B
macho9 load-command FILE -delete uuid ==   load-command delete uuid
macho9 minos FILE 10.9                ==   version-min set 10.9
```

That is worth more than tidiness. Today `cli/macho9.c` carries a per-verb
argument loop, a `DYLIB_OPS` translation table, verb-level flag handling for
`--allow-grow`, and a `--capabilities` list that has to agree with all of it —
and this repo has already had one defect from two such lists disagreeing. If
every verb lowers to a script statement, there is **one grammar and one parser**,
and `--capabilities` can be generated from the statement table rather than
maintained beside it.

This is not a required part of the design and it does not have to happen at
once. It is recorded because it is the payoff of deciding what this toolkit
primarily is, and because doing it later costs more than doing it while the
statement table is being written.

## Relations — one place to record what points at what

A binary is a graph that has been flattened, and the hard part of editing one is
never the edit: it is the cross-references. Offsets, counts, indices, addresses,
some absolute and some derived. Nearly every defect this codebase records is the
same shape — an edit changed the flattened form without updating something that
pointed into it. Of the four defects `docs/PROPOSAL.md` lists, three are that
(the fourth was an array bounds bug).

So "does this operation need follow-up work?" is not a judgement call. It is
**whether anything points at what the operation touches** — a property of the
format, not of our taste, which is why the set came out small and specific
rather than arguable.

This design names the relations as **data in one place**, rather than leaving
them as knowledge distributed across whichever functions happen to maintain
them:

| relation | referent | maintained today by | historical defect |
|---|---|---|---|
| library ordinal | the subsequence of ordinal-carrying load commands | `mo_map_build`/`mo_map_apply` (`src/ordinals.c`), consumed by `nlist.n_desc` and the `SET_DYLIB_ORDINAL*` opcodes in the bind/weak/lazy streams | PROPOSAL #2: `-delete` left ordinals stale, `dyld: library ordinal (4) too big` |
| base-relative values | the image base | `src/grow.c`'s re-base pass over `__init_offsets`, function starts, and the export trie | PROPOSAL #1 and #4 — and #4 was *two* correct implementations of #1 both running, so entries gained `2*grow` |
| file-offset fields | `__LINKEDIT`'s blobs | `src/grow.c`'s offset-bump table, `src/linkedit.c` | — |
| initializer and unwind targets | `LC_FUNCTION_STARTS` | `mg_plausible` — checked, never repaired | the base-of-zero precondition bug (Task 0 of the reporting plan) |
| `sizeofcmds` | the header pad, i.e. the first section's file offset | `mr_build_lcs`, `mg_grow_header` | — |

Two things follow, and both are requirements of this design rather than
observations about it:

**Which operations carry follow-ups is derived, not hardcoded.** An operation
needs follow-up work exactly when the structure it edits is the referent of some
relation in that table. `dylib delete` and `dylib insert` reorder the ordinal-carrying
subsequence; `fixups lower` rewrites the blobs that file-offset fields name.
Everything else touches nothing anyone points at. If a relation is added, the
set moves with it — nobody has to remember to update a second list.

**PROPOSAL's own argument for this is defect #4.** Two correct functions,
written months apart against different predicates, met in a merge and silently
composed. Its conclusion was *"only one place to put the knowledge does"* —
which this applies one level up, to the knowledge of what points at what.

## Directives

Properties of the run, not steps in it, so they read as declarations:

```
allow-grow          permission to enlarge the header pad by lowering the image
                    base if the new load commands do not fit. Opt-in and
                    failing by default. MH_EXECUTE + MH_PIE only (src/grow.c:846-853)
fatal-warnings      an operation that matched nothing is an error, not a report
```

`allow-grow` keeps its name to match `ld`'s permission convention
(`--allow-multiple-definition`, `--allow-shlib-undefined`). It stays distinct
from the standalone `grow FILE N` verb, which is an instruction rather than a
permission.

`fatal-warnings` matches `ld` and `gas`, and GCC's `-Werror`. An operation that
matched nothing genuinely is a warning; this promotes it.

**Directives must precede every operation.** They describe the run, so a reader
should not have to reach line 40 to learn that growth was permitted, and the
parser should not have to decide what a directive appearing after the statement
it would have governed means. A directive below an operation is a parse error.
Repeating a directive is not — it is idempotent and harmless.

## File format

One statement per line. Fields are whitespace-separated, with single- and
double-quote grouping and backslash escapes — shell word rules. `#` begins a
comment except inside quotes. Blank lines are ignored.

This matches `objcopy --redefine-syms=FILE`, binutils' own edit script-file
convention, in comment character and blank-line handling. Shell quoting is not
an arbitrary pick either: `compat/translate.sh`'s `mt_quote` already emits
exactly this, and it has been through a hostile-argument battery — paths
containing `$( )`, backticks, semicolons, globs, spaces and leading dashes — so
the generator and the parser cannot drift.

```
# port-claude.edits
allow-grow
fixups        lower
version-min   set      10.9
load-command  delete   uuid
load-command  delete   codesig
dylib         replace  /usr/lib/libSystem.B.dylib  @loader_path/../S.dylib
dylib         replace  /usr/lib/libicucore.A.dylib @loader_path/../I.dylib
dylib         replace  /usr/lib/libc++.1.dylib     @loader_path/../c++.1.dylib
```

## Execution model

1. **Parse the whole edit script.** Any syntax error, or a statement this build does
   not know, is reported before
   the file is opened for writing.
2. **Read the image once.**
3. **Apply each statement in order** against the in-memory buffer.
4. **Verify** the finished image.
5. **Write once** — atomically to `FILE`, or to `--output`.

Sequential rather than collapsing the edit script into one operation set, because
`fixups lower` cannot batch with anything (later statements must see the lowered
image), and because "run in sequence" is what a reader will assume. The cost is
rebuilding the load-command table once per statement; on a 200MB binary that
table is a few KB and the expensive part is I/O, which happens once either way.

**Any statement failing means nothing is written.** That is the property the
whole design exists to buy.

**`--dry-run` skips step 5 and nothing else.** Everything is parsed, applied and
verified exactly as it would be; only the write does not happen, and the run
reports what it did. That makes it **accurate by construction** rather than a
prediction — a dry run exercises the same code the real run does, including the
data-dependent follow-ups a static explanation could never show. It is the one
part of this design that is nearly free, and it falls out of applying to an
in-memory image and writing once.

`edit` uses the verbs' existing exit codes, unchanged: `0` on success, `2`
where it examined the file and declined on purpose (a refused statement, a
failed verify, or an unmatched operation under `fatal-warnings`), `1` for an
operational failure (a syscall, a malloc, an unparseable edit script). An edit script that
asked for nothing the image needed still succeeds, and says so, exactly as a
single verb does today.

## Verification

Mandatory, after the last statement and before the write. **No escape hatch.**

This is viable only because of a finding from the spike that preceded this
design: `mg_plausible` refuses every dylib and bundle at a precondition that
mistakes a legitimate image base of 0 for "no segment maps the header", so the
check it exists to perform never runs. Until that is fixed, a mandatory gate
would be a mandatory rubber stamp. Task 0 of
`docs/superpowers/plans/2026-09-10-report-what-macho9-did.md` fixes it.

No opt-out, deliberately: a `MACHO_NO_VERIFY` escape was removed from a shipped
wrapper during the compat-tool retirement, on the grounds that a
disabled-safety-check shaped hole in a shipped artifact is worse than the false
positives it hides. Reintroducing one at a higher level would undo that.

**Verify reads the relation table.** Checking an invariant and repairing it are
the same declaration read in opposite directions, so they are written once.
Today they are not: `mg_verify`, `mg_snapshot_take` and `mg_plausible` each
hand-roll their own version of "is this still consistent", each with its own
hand-rolled applicability condition — and one of those conditions is the bug
Task 0 fixes. A relation declared with its applicability ("the initializer
invariant applies when `LC_FUNCTION_STARTS` is present") could not have grown a
base-of-zero sentinel, because nobody writing the declaration would have written
one.

This widens the design past the `edit` verb: it touches `src/grow.c`'s
verification, which the script language does not otherwise care about. That cost
is accepted rather than hidden, for two reasons. A mandatory verify gate is only
worth having if the thing it runs is trustworthy, and this design makes verify
mandatory. And deriving which operations carry follow-ups needs the same table,
so it is paid for twice over.

## Verbose output must include the follow-ups

The companion plan
`docs/superpowers/plans/2026-09-10-report-what-macho9-did.md` already covers
reporting: Task 1 makes `macho9` say which requested operations matched nothing,
Task 2 adds `fatal-warnings`. This design adds one requirement to it.

**Follow-up work must be logged too.** Verified while writing this:
`src/ordinals.c` prints **only errors** — renumbering, the largest follow-up this
toolkit performs, is entirely silent on success. So a verbose run today would say
"replaced X with Y" and nothing about the ordinals rewritten in both the `nlist`
entries and the `SET_DYLIB_ORDINAL*` opcode streams.

That is exactly backwards. The follow-ups are the part a user **cannot** see for
themselves — they are the work they did not ask for by name — so they are the
part most worth logging. A verbose log should let someone read back everything
that happened to their binary, not just the statements they wrote.

## The script is the plan

`docs/PROPOSAL.md` says `port` *"can plan across families"*, implying the tool
derives the ordering. **This design does not reorder statements.** They run in
the order written.

If `load-command delete uuid` frees the pad a later `dylib replace` needs, the
caller wrote them in that order deliberately. `compat/translate.sh` already
knows to emit `lc` before `dylib`, so the planning lives in the generator that
has the context for it, and the tool keeps a property that is one sentence to
state and one test to check.

## Naming

| was | is | why |
|---|---|---|
| `port` | `edit` | "port" collides with Mach ports, in a Mach-O tool, and "apply" is too broad. `edit` names what it does to the binary and matches the artifact it takes — an edit script — and separates cleanly from `info` and `verify`, the verbs that do not change the file |
| `lc` | `load-command` | matches `otool -l`'s own term and the spec's stated principle that "the family is a subcommand, the operation is a flag, and both are always explicit". Also retires the `-strip-lc` spelling, which invited confusion with binutils' `strip` (symbols and debug info) |
| `declassify` | `fixups lower` | gives it a family, where it was the one family-less verb. "Lower" is the compiler term for translating to a more primitive representation, which is exactly what it does: chained fixups (macOS 12+) down to `LC_DYLD_INFO_ONLY` (10.6+) |
| `retag-swift` | `swift-abi set legacy` | it is the same kind of operation as `version-min` — adjusting what the binary claims about its deployment target so an older runtime accepts it — so it is named to match rather than as a one-off verb. See "Why `swift-abi`, not `retag-swift`" |
| `--for 10.9` | dropped | `version-min set 10.9` says it plainly, and `--for` was the only justification "port" had left |

`docs/PROPOSAL.md`'s "Why these names" section explains every naming choice it
made except `declassify` and `port` — the two this design replaces.

## binutils alignment

**Matched:** `objcopy`'s `infile [outfile]` shape, where omitting the output
modifies in place via temp-and-rename. `--redefine-syms=FILE`'s edit script-file
conventions. `rename`, from `--rename-section`. `--fatal-warnings`, from `ld`
and `gas`. `--allow-*`, from `ld`.

**Deliberately not matched:** `--remove-section` and `--strip-all` — Apple says
*delete* and *load command*, and Mach-O people reading `otool -l` are the
audience. `--wildcard` — patterns that match one more command than the author
expected are the silent-success class this toolkit exists to eliminate.

**Reserved:** `arch`. If an edit script ever targets one slice of a fat container, the
vocabulary to match is `lipo`'s (`-arch`, `-thin`, `-extract`, `-remove`), not
binutils' `--target`/`-I`/`-O`, which is a different concept. Do not spend the
word elsewhere.

**Knowingly divergent:** `objcopy` frames address manipulation mechanically
(`--adjust-vma`, `--change-addresses`). `allow-grow` and the `grow` verb frame
it by intent — enlarge the header pad — with lowering the image base as the
means. The pad is what a caller cares about here.

## Exit codes — corrected while we still can

The shipped scheme is `0` ok, `1` failed, `2` refused, and it is **backwards from
the convention shell authors actually know**: `grep` uses `2` for *error*,
`diff` and `cmp` use `2` for *trouble*. Both reserve the highest code for "the
tool could not do its job", where this reserves it for "the tool did its job and
declined".

Since nothing outside this repo has ever run the compat wrappers, there is no
compatibility to break. **The scheme becomes `0` ok, `1` refused, `2` error.**

To be accurate about the precedent: this does *not* line up with binutils, which
returns 0 or 1 and has no notion of a deliberate refusal. It lines up with
`diff`, `grep` and `cmp` — the tools whose exit codes a shell author has
internalised — and the rule it follows is theirs: **2 means something went
wrong; 1 means a normal, expected, non-success answer.**

`--capabilities`' `exitcodes` line carries the new values, so a wrapper reads
them rather than assuming.

## Out of scope

- Conditionals, variables, includes. The operations are already safe no-ops on
  inapplicable input — `fixups lower` passes an already-lowered binary through
  unchanged, and a `replace` that matches nothing does nothing — so a predicate
  buys almost nothing for a real parser's cost.
- Per-slice targeting of fat containers. See "Reserved" above.
- Symbol-table surgery (renaming, globalizing, localizing). **The reason is that
  nobody has asked, not that it is better done elsewhere** — an earlier draft
  implied the latter and that was wrong. This toolkit already writes `nlist`
  entries during ordinal renumbering, so it is not foreign territory, and
  `nmedit` (in the cctools set) covers globalize/localize today for anyone who
  needs it. If a real need appears, this is a statement away.
- `--explain`, meaning a *static* expansion of an operation into the smaller
  edits it performs. Ordinal renumbering is data-dependent — which ordinals
  shift depends on which symbols currently bind to which dylib — so it cannot be
  shown as a fixed sequence, and an `--explain` that could not explain the three
  operations most in need of explaining would claim more than it delivers.
  **`--dry-run` is the honest version of the same wish, and it is in scope** —
  see "Execution model".

### Two generalizations, not one

The general version of this design is often described as one idea — "a format
description language with an engine that derives repair" — and an earlier draft
of this spec dismissed it as a scope error. That was wrong twice over: it is two
ideas with very different evidence behind them, and the objection to the second
is about **sequencing**, not scope.

The design heuristic that produced this section, and the relation table above,
is worth naming because it is the opposite of the reflex: *what would have made
the current problem easy to solve, and is it worth making that thing exist?*

**(a) Declare the relations for this format; derive repair and verification
from them.** Not speculative. This spec commits to it, and the evidence is that
it would have prevented four failures this codebase actually had:

| failure | what a declared relation would have done |
|---|---|
| `mg_plausible` refusing every dylib on a base-of-zero sentinel | applicability declared ("when `LC_FUNCTION_STARTS` is present"), so no hand-rolled precondition to get wrong |
| PROPOSAL #4 — two correct `__init_offsets` re-base implementations both running, entries gaining `2*grow` | one declaration, one repair; nothing to duplicate |
| PROPOSAL #2 — `-delete` leaving library ordinals stale, `dyld: library ordinal (4) too big` | repair derived from the relation rather than remembered |
| `fix_macho -change` hand-listing `{LOAD, WEAK, ID, REEXPORT}` and silently missing `LC_LOAD_UPWARD_DYLIB` | one declaration of which commands carry an ordinal; the second list cannot drift because there is no second list |

The generalization axis here is **relations, not formats** — and there are five
of them today. That is enough to generalize from.

**(b) Make the format itself data, so a second format could be added.** This is
the sequencing question, and it is genuinely open rather than closed.

Against it now: there is one format and no concrete second. And the deepest
design fork is already decided in a direction that makes the general case
harder — a format-driven engine naturally *rebuilds* a binary from its parsed
model, and this toolkit must never move a byte. That constraint is the reason it
exists: 10.9's `install_name_tool` refuses these binaries outright ("file not in
an order that can be processed") precisely because it rebuilds `__LINKEDIT` and
expects a 2013-era ordering. Repair-in-place is much harder than rebuild, and it
is the whole product. Any general engine would have to be built on the hard side
from the start.

**But there is already a second consumer of the same knowledge, and it is not a
second format.** `src/live.h` walks the same structures against *loaded images*
— `_dyld_*` plus slide, in-process, allocation-free — and `docs/PROPOSAL.md` is
explicit that *"the shared thing is the structure knowledge, not the code
path."* Today that sharing is achieved by writing the walks twice and hoping
they agree. Declared relations are exactly what would let one description serve
both the on-disk rewriter and the in-memory reader. That is a real second
consumer, existing today, in this repository's own orbit — and it arrived
without anyone needing a second binary format.

So the honest position: (b) is not out of scope, it is **not yet**. What would
make it pay, in rough order of how likely each is to arrive:

- the relation table from (a) existing and proving itself in use;
- `live.h` deriving its walks from the same declarations instead of duplicating
  them — the second consumer that already exists;
- a second binary format actually needed, which nothing currently suggests:
  the population is arbitrary macOS applications, and those are all Mach-O.

The first two are reachable from here. Revisit when the first has landed and the
second is the obvious next factoring rather than a guess.

**Prior art, for whoever picks this up.** GNU poke is the closest thing that
exists — a real language for describing and editing binary structures — and it
stops at the structure level: it will let you assign to a field and has no
notion of repairing what the assignment invalidated. Kaitai Struct is
declarative and read-mostly. LIEF derives repair but rebuilds. The
relation-declaring, repair-deriving, never-move-a-byte combination appears not
to exist, which is either a gap worth filling or a signal that it is harder than
it looks. Finding out which is a project, not a task.

### The ceiling this design accepts

Stated plainly, so nobody has to infer it: the statement set is a hardcoded
enumeration of the relations we know, so adding an edit — or a format — means
editing C, not data. The relation table centralises that knowledge; it does not
make it extensible by configuration.

That is generalization (a) without (b): one description of what points at what,
consumed by the code that repairs and the code that verifies, but not itself a
format description an engine reads. It is a deliberate ceiling with a named
condition for raising it, not an oversight.

## Consumers

`compat/translate.sh` emits an `edit` script whenever a translated invocation
needs more than one `macho9` command, and the plain verb otherwise. That targets
exactly the case that measured harm without churning single-command paths.

`compat/patch_macho.sh` can become an `edit --output` call, since `--output`
subsumes its `IN OUT` shape.

Once both hold, `compat/macho9-compat.sh`'s temp-copy-and-install dance has no
remaining caller and can go, taking the extra 200MB with it.
