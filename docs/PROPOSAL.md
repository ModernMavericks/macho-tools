# Proposal: extract the Mach-O surgery into one toolkit

Discussion doc for `Mavericks-Porting-Resources`. **Whether to build the toolkit
is not decided**; the repo, its ownership and the sequencing below are all still
proposals.

What *is* decided, as of 2026-09-08, is the CLI grammar and naming under
"Verbs". Those choices were worked through on their own merits against the
existing `change_dylib`, and they hold wherever the code ends up — inside a new
toolkit, or applied in place if this proposal goes nowhere. They are recorded
here rather than in a separate spec so there is one document, not two that
drift.

## What exists today

Eleven pieces of code rewrite or read Mach-O binaries, in two families that
never share a line.

**File rewriters** — read a whole file into a buffer, mutate, write back:

| file | lines | job |
|---|---|---|
| `patch_macho.c` | 356 | chained fixups → `LC_DYLD_INFO_ONLY`; strips exports trie + build version; extends `__LINKEDIT` |
| `change_dylib.c` | 677 | dylib `-change`/`-delete`/`-reexport`/`-add`/`-insert` with library-ordinal renumbering; the same three verbs for `LC_RPATH`; `-strip-lc` |
| `macho_grow.h` | 566 | grow the header pad by lowering the image base; the exhaustive file-offset bump table; function-starts, `__init_offsets` and export-trie handling |
| `fix_macho.c` | 194 | `-change` + strip build version, **fat binaries** |
| `add_version_min.c` | 66 | append `LC_VERSION_MIN_MACOSX` |
| `rename_segment.c` | 96 | `__DATA_CONST` → `__DATA`, so 10.9's libobjc finds the metadata |
| `retag_swift_classes.c` | 156 | move the is-Swift tag from the stable-ABI bit to the legacy one |

Updated 2026-09-08. `change_dylib.c` has nearly tripled since this table was
first written, which is itself part of the argument: the tool that was going to
be folded in is growing faster than the toolkit that would fold it.

**Live-image walkers** — same structures, but `_dyld_*` + slide, in-process:

| file | what it walks |
|---|---|
| `avxemu/src/handler.c` | a loaded image's symtab (`real_sym`); indirect symbol table → pointer slots (the rebind) |
| `avxemu/src/patch_mem.c`, `tramp.c` | `__TEXT` bounds, page protections, in-place code patching |
| `mavericks-legacy-support/src/init_offsets.c` | the main image's `__TEXT,__init_offsets` |
| `mavericks-legacy-support/src/dlopen_interpose.c` | image lookup |

Plus a dozen avxemu test tools that parse the binary to find candidate
instructions.

## The duplication is not subtle

Independent implementations, counted by grep:

- **19 files** iterate load commands from `mach_header_64.ncmds`
- **13** validate `MH_MAGIC_64` themselves
- **13** carry their own ULEB128 code
- **5** compute a `__LINKEDIT` base — and the file form
  (`vmaddr - fileoff`) differs from the in-memory form (`+ slide`), which is
  exactly the kind of off-by-one-concept that produces silent corruption
- `fix_macho -change` and `change_dylib -change` are two answers to one
  question, differing in fat-binary support versus header-pad handling

Nobody shares the two hardest assets: `macho_grow.h`'s enumeration of every
file-offset field a shift must touch, and the library-ordinal model that
`patch_macho` *emits* and `change_dylib` *renumbers* — two files that must agree
about `SET_DYLIB_ORDINAL_IMM` versus `_ULEB` versus `_SPECIAL_IMM`, and
currently agree by coincidence.

## Why not just use install_name_tool

Worth stating plainly, because it is the first question anyone asks and the
answer turns out to be decisive. Measured 2026-09-08 against Claude Code
2.1.263, the binary this whole pipeline exists to patch:

```
$ install_name_tool -change @loader_path/../I.dylib @loader_path/../J.dylib cc
install_name_tool: file not in an order that can be processed (dyld_info out of place)

$ install_name_tool -add_rpath /tmp/zzz cc
install_name_tool: file not in an order that can be processed (dyld_info out of place)

$ change_dylib cc -change @loader_path/../I.dylib @loader_path/../J.dylib
Header pad: 96 bytes available (LC end=2784, first sect=2880)
  Change [56->56 bytes]: @loader_path/../I.dylib -> @loader_path/../J.dylib
Updated cc (sizeofcmds=2752, 208526708 bytes)
```

10.9's `install_name_tool` does not do these operations less conveniently on our
binaries. **It refuses to open them.** It rebuilds `__LINKEDIT` and requires the
2013-era ordering of its pieces; modern linkers emit a different order, so it
bails before touching anything.

On a small, locally-linked binary it works fine — and beats these tools, since it
will happily fit a much longer path by moving data. The distinction is the whole
design: `install_name_tool` **rewrites the file**, and our tools **never move a
byte**, editing only within existing header padding. That constraint is why
`-grow` and `-strip-lc` exist, and why a replacement path that is too long is a
failure here and a non-event there.

Two consequences for this toolkit:

- The overlap in verb names is **illusory**. Do not adopt Apple's spellings
  (`-add_rpath`, bare `-rpath` meaning change) as synonyms or aliases: they would
  advertise an interchangeability that does not exist, on exactly the binaries
  where it does not hold.
- `info` and `verify` are not conveniences. When the standard tool refuses the
  file and 10.9's `otool` prints `?(0x80000034) Unknown load command` for
  everything modern, this toolkit is the only thing that can read these binaries
  at all.

## Shape

One repo, one library, one multi-call CLI. Working name `macho9` — Mach-O
surgery for hosts too old to have any: builds with stock 10.9 clang, no
dependencies, edits binaries from toolchains fifteen years newer.

```
src/
  image.c      open/validate/iterate; find segment, section, load command
  linkedit.c   symtab, strtab, indirect symbols; the offset-bump table
  uleb.c       decode / encode_fixed / minlen
  bind.c       walk, rewrite, and emit bind & rebase opcode streams
  chained.c    parse chained fixups; lower them to classic dyld info
  grow.c       header growth + every base-relative fixup it invalidates
  ordinals.c   the library-ordinal map: build, apply, validate
  live.h       header-only, malloc-free: the same queries against loaded images
cli/macho9.c   verbs
tests/         hermetic fixtures + end-to-end against a real binary
```

`live.h` is deliberately a header, not part of the library. avxemu's core must
stay VEX-free and its handler async-signal-safe; it cannot link something that
might allocate. The shared thing is the *structure knowledge*, not the code
path.

## Verbs

Grammar settled 2026-09-08. The family is a **subcommand**, the operation is a
**flag**, and both are always explicit.

```
macho9 declassify IN OUT      chained fixups -> LC_DYLD_INFO_ONLY   (patch_macho)

macho9 dylib FILE [--allow-grow] OP...        (change_dylib + fix_macho)
    -replace  OLD NEW     rewrite a path in place; position and ordinal kept
    -delete   PATH        remove it; renumber survivors; refuse if symbols bind
    -append   PATH        add a dependency, initialized LAST
    -insert   PATH        add a dependency, initialized FIRST; renumbers
    -reexport PATH        promote LC_LOAD_DYLIB -> LC_REEXPORT_DYLIB

macho9 rpath FILE [--allow-grow] OP...
    -replace  OLD NEW     rewrite a search path in place, keeping its position
    -delete   PATH        remove a search path
    -append   PATH        add one, searched LAST
    -insert   PATH        add one, searched FIRST

macho9 lc FILE OP...
    -delete   KIND        uuid | codesig | source-version | build-version
                          | code-sign-drs

macho9 grow FILE N                                          (macho_grow)
macho9 minos FILE OUT 10.9 [--allow-grow]                   (add_version_min)
macho9 segment FILE OLD NEW                                 (rename_segment)
macho9 retag-swift FILE                                     (retag_swift_classes)
macho9 info FILE              dump load commands, ordinals, pads
macho9 verify FILE            check the invariants
macho9 port FILE --for 10.9 --insert @loader_path/libA.dylib ...
```

### Why these names

- **`-replace`, not `-change`.** It replaces one string with another; "change"
  says nothing about what happens.
- **`-append`, not `-add`.** Position is semantics in both families, and `-add`
  hides that. For dylibs, load order is *initialization* order — which is why
  `-insert` had to exist at all, to get an emulator's SIGILL handler installed
  before any other library's constructor runs. For rpaths, load order is
  *search precedence*: dyld takes the first match. Verified on 10.9 — two
  rpaths, the same `@rpath/libw.dylib` under each, and flipping their order
  flips which one loads.
- **`-insert` for rpath is a new capability.** `change_dylib` today can only
  append a search path, so a newly added rpath can never outrank an existing
  one; the workaround is deleting and re-adding every other entry to reshuffle
  them.
- **`lc -delete` replaces `-strip-lc`.** Stripping *is* deleting, on a third
  family. Making it one verb across three families removes a special case
  rather than adding one.
- **`--allow-grow`, not `-grow`.** It is a permission, not an instruction: "if
  the new load commands do not fit, you may lower the image base." It stays
  opt-in and failing by default, because growth is the only operation here that
  touches the whole file rather than the header, and that machinery has now
  produced four defects (below). A path three bytes too long should not silently
  trigger it. Note this is distinct from the standalone `grow` verb, which
  enlarges the pad on request.
- **No Apple synonyms**, for the reason in "Why not just use install_name_tool".

### One friction worth designing for

The wrapper's live invocation **mixes families in a single command** — it strips
`uuid` and `codesig` to reclaim header bytes, then rewrites three dylib paths,
and the stripping is what makes room for the rewriting:

```sh
change_dylib "$T" -grow -strip-lc uuid -strip-lc codesig \
    -change "/usr/lib/libSystem.B.dylib"  "@loader_path/../S.dylib" \
    -change "/usr/lib/libicucore.A.dylib" "@loader_path/../I.dylib" \
    -change "/usr/lib/libc++.1.dylib"     "@loader_path/../c++.1.dylib"
```

Under a subcommand-per-family CLI that becomes two invocations, `lc` then
`dylib`, which works — the first shrinks the table, the second uses the room —
but writes the file twice and makes the ordering a thing the caller must know.

This is what `port` is for, and it is a stronger argument for that verb than
the original "three-step pipeline" framing: `port` is the one entry point that
can plan across families, do it in a single rewrite, and `verify` before
replacing anything.

### verify

`info` is worth having on its own: 10.9's otool prints
`?(0x80000034) Unknown load command` for everything modern, so today you decode
chained-fixups binaries by hand.

**`verify` is the reason to build this.** Every defect found in this code has
been a silent success — each tool reported OK and the binary died in the loader,
or worse, did not:

1. `-grow` left `__init_offsets` entries a page low (base-relative data not
   re-based) — fixed, PR #5
2. `-delete` left library ordinals stale (`dyld: library ordinal (4) too big`)
   — fixed, PR #6
3. Repeated options wrote past their fixed-size arrays; 33 `-change` flags
   smashed the stack — fixed, PR #9
4. **Two implementations of fix 1 both ran**, so entries gained `2*grow` — fixed,
   PR #10

The fourth is the most instructive, and it is an argument for this toolkit
rather than against it. Two correct functions, written months apart by different
people against different predicates (section *name* versus section *type*), met
in a merge and silently composed. No review catches that; only one place to put
the knowledge does.

It is also an argument for `verify` specifically. None of these are hard to
*check*: ordinals within range and non-orphaned, `__LINKEDIT` covering every
appended blob, function-starts and `__init_offsets` consistent with the image
base, section offsets inside the file. Note that a `verify` run after the grow
would have caught defect 4 without knowing anything about the duplication — the
entries simply would not have pointed at plausible code. A `verify` that runs in
the wrapper between the rewrite and the `mv` converts this whole class from
"binary replaced, re-download that version" into "patch refused, nothing lost".

### Migration

The CLI break is real: every existing invocation changes. Settled approach is
**build it, ship behind a probe** — `macho9 --capabilities` reports the grammar
and verbs a given build accepts, so the tool and the wrapper never have to move
in lockstep, and `MF_GEN` coordinates retiring the old spellings rather than
gating a flag day. `change_dylib` survives as a compatibility entry point over
`macho9 dylib` for the transition, and is removed at a later `MF_GEN`.

## One repo — `macho-tools`

**Naming corrected 2026-09-08.** This section said `mavericks-machotools`, and
the avxemu section `mavericks-avxemu`. The org names repos bare —
`ModernMavericks/golang`, `/clang`, `/tailscale`, `/openssh` — and reserves the
`mavericks-` prefix for the *package* a repo produces. So the repos are
`macho-tools` and `avxemu`; the packages they ship are `mavericks-macho-tools`
and `mavericks-avxemu`. Hyphenated, matching `container-tools`,
`swift-runtime` and `macports-legacy-support`.

An earlier draft of this proposed two: a source repo plus a `mavericks-*`
packaging repo pinning it. That was pattern-matching on
`mavericks-legacysupport`, and it was wrong. Legacysupport is two-layered
because its upstream is **somebody else's** — `macports/macports-legacy-support`,
tracked by Renovate on their tags. Splitting a repo you own means tagging your
own source, waiting for a bot to open a bump PR against yourself, and cutting a
second release, for nothing.

The family already has the first-party shape: `mavericks-magic-trackpad2` holds
its own `src/`, CMake against shared-cmake, tests, a characterization corpus,
the Sparkle updater, and `release.yml` — one repo, no `UPSTREAM_VERSION`,
because it *is* its own upstream. Same for `mavericks-golang`, `-tailscale`,
`-clang`, and most of the family.

So: **one repo, first-party, no `UPSTREAM_VERSION`.** Source, tests, the `.pkg`,
and the Sparkle appcast together. mavericksforever.com keeps building and
hosting `patch_macho`/`change_dylib`/`add_version_min` for `install.sh` from
that same repo, exactly as it builds them from `Mavericks-Porting-Resources`
today; ModernMavericks additionally ships a `.pkg` for people who want the tools
on their 10.9 machine.

**The repo count follows ownership, not function.** If Wowfunhappy would rather
keep the Mach-O tools under his own account — reasonable, `patch_macho` and
`fix_macho` are his — then it becomes third-party to the family and the
legacysupport two-layer shape reappears, with `UPSTREAM_VERSION` pinning his
tags. That is a governance question to settle first; the technical work is
identical either way.

The build-equivalence invariant fits unusually well here. These tools *run* on
10.9 today because they are built there; under `mavericks_build_mode` the native
and cross recipes become one, and the compat guard proves the cross-built
binaries are 10.9-safe without a 10.9 runner.

And `verify` doubles as this project's in-CI characterization proof. You cannot
launch a 10.9 binary on the runner, but you can commit a small pristine
chained-fixups fixture, run the whole pipeline over it, and assert the output's
invariants hold. That is a stronger check than most of the family can manage,
and it is the same code that guards the wrapper at runtime.

## Sequencing

**The precondition is met as of 2026-09-08.** This section used to read "land
the three pending branches first" — reorganising in front of a loader-crash fix
would have delayed it for tidiness and started the new repo from a knowingly
broken base. All seven PRs are now merged (`Mavericks-Porting-Resources` master
`c4ead57`), including the two defects above that were found while preparing this
revision. The base is as sound as it has been.

The order below assumes the new repo from day one rather than a later split:

1. `uleb.c` + `image.c` — mechanical, deletes the most code
2. `fix_macho` and `change_dylib` converge (fat support from one, pad/grow/
   insert from the other) **behind the settled grammar** — this is where
   `dylib`/`rpath`/`lc` subcommands and `-replace`/`-append`/`-insert`/
   `-delete`/`-reexport` land, so the CLI breaks exactly once
3. `ordinals.c` shared, so the emitter and the renumberer agree by construction
4. `verify`, wired into the wrapper before its `mv`
5. `port`, once `verify` exists to make it atomic
6. release.yml + Sparkle, once the tools are worth cutting a .pkg for

**If this proposal stalls**, step 2's grammar is still worth doing in place,
against `change_dylib` as it stands — it is the only part with a user-visible
payoff, and the migration probe means it need not wait for the rest. The one
thing not worth doing twice is renaming: `change_dylib` should become
`macho9 dylib` or stay `change_dylib`, never something in between.

## How this meets avxemu

`avxemu` is the same story: first-party source, one repo, its own `.pkg`
(`mavericks-avxemu`). Two repos total, not four.

It needs `live.h` — the header-only, malloc-free subset — and the family rule is
consume-don't-vendor, so `mavericks-machotools` should install a CMake package
exporting an INTERFACE target that avxemu picks up with `find_package`, the same
way projects consume `shared-cmake`. The constraint travels with it: anything
avxemu's SIGILL handler can reach must stay allocation-free and VEX-free, so
`live.h` is a header and never grows a `.c`.

That does make one `mavericks-*` repo a build-time dependency of another, which
the family hasn't had to express before outside shared-cmake. Worth deciding
deliberately: if it turns out awkward, the fallback is for avxemu to keep its
own copy of the handful of structure walks it needs, which is what it does today
and which costs little — the walks are small; it is the *rewriters* whose
duplication actually hurts.

## Honest costs

This is churn on code that currently works and is shipped to real users. Steps
1–2 are refactors with no user-visible benefit. The case rests on `verify` and
on the emitter and renumberer agreeing about ordinals by construction — if only
part of this happens, do 3 and 4.

It is also Wowfunhappy's code in part: `patch_macho` and `fix_macho` are his;
`change_dylib -grow/-add/-insert`, the renumbering, and the `__init_offsets`
re-base are ours. Whose account the repo lives under is worth settling before
the first commit, not after.
