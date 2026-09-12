# Mach-O Tools for Mavericks

Mach-O surgery for hosts too old to have any. Builds with the stock 10.9 clang,
no dependencies, and edits binaries produced by toolchains fifteen years newer.

| tool | job |
|---|---|
| `patch_macho` | rewrite chained fixups as `LC_DYLD_INFO_ONLY`, so 10.9's dyld can load the image |
| `change_dylib` | edit `LC_LOAD_DYLIB` / `LC_RPATH` — change, delete, add, insert, re-export — with library-ordinal renumbering; `-strip-lc`; `-grow` |
| `add_version_min` | append `LC_VERSION_MIN_MACOSX` |
| `fix_macho` | change install names and strip build version, including in fat binaries |
| `rename_segment` | `__DATA_CONST` → `__DATA`, so 10.9's libobjc finds the metadata |
| `retag_swift_classes` | move the is-Swift tag from the stable-ABI bit to the legacy one |

## Layout

- `src/` — the shared toolkit library (`macho9core`): image parsing, ULEB,
  ordinals, fat-arch validation, export-trie rebuild, `__LINKEDIT` bumping,
  header growth, LC-kind tables, the atomic-write helper, the dylib/rpath
  load-command rewriter, the `LC_VERSION_MIN_MACOSX` appender, the segment
  rename, and the Swift class-record retag.
- `compat/` — these six tools' entry points. They predate `macho9` and keep
  their original names because `install.sh` fetches some of them by name. All
  six are now `/bin/sh` wrappers that print the `macho9` equivalent of what
  they were asked to do and then do it through `macho9`, so **`macho9` is the
  only Mach-O rewriting binary this repo ships** and `compat/` contains no C
  at all. `fix_macho` was the last holdout: wrapping it changes what it does
  in five ways, and those changes were adopted deliberately rather than
  papered over — `compat/fix_macho.sh`'s header states each with its reason.
  Also here: `translate.sh`, the old-grammar-to-`macho9` translator the
  wrappers source, and `macho9-compat.sh`, the machinery they share. See
  `compat/README.md`.

  **Packaging note:** the six wrappers need `macho9`, `macho9-compat.sh` and
  `macho9-translate.sh` installed beside them. Anything that fetches
  `patch_macho`, `change_dylib` or `add_version_min` by name now has three
  more files to fetch. `compat/README.md` says what that means for
  `mavericksforever.com/claude/install.sh`, which has not been told.
- `cli/` — `macho9`, the multi-verb CLI built on `src/`.
- `tests/` — everything `ctest` runs, plus the fixtures it reads.

## Building

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

Needs [shipyard](https://github.com/ModernMavericks/shipyard), the family's
shared CMake helpers — install it once and it self-registers, so `find_package`
finds it with no `CMAKE_PREFIX_PATH`:

```sh
cmake -S ../mavericks-shipyard -B /tmp/sy -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --install /tmp/sy
```

Presets pick the build mode:

```sh
cmake --preset native   # on 10.9, with its own clang
cmake --preset cross    # on a modern host, against the pinned 10.9 SDK
```

Every tool is gated by shipyard's compat guard, which fails the build if a
binary declares a floor above 10.9 or links a symbol 10.9 lacks. That matters
more here than elsewhere in the family: these are the tools that make *other*
binaries loadable on 10.9, so they had better load there themselves.

## Build equivalence

There is no 10.9 runner in CI, so a cross-build has to be shown equivalent to a
native one rather than assumed to be. Comparing the tool binaries is the wrong
test — a 2014 clang and a 2026 one will never emit the same bytes.

These tools are **deterministic file transformers**, so what they *produce* is
the invariant worth pinning. `tests/characterize.sh` runs the whole pipeline
over a committed fixture and compares the output digest against
`tests/EXPECTED`; `ctest` runs it. Build natively and cross, and the digests
must match.

Two properties make that runnable in CI: the pipeline is deterministic (verified
— same input, same flags, identical output), and a cross-built tool is x86_64
with a 10.9 *floor*, which still runs on a modern host, so the runner can
execute what it just built.

Known gap: the committed fixture is a 10.9-built binary, so it has no chained
fixups and does not exercise `patch_macho`'s conversion — the heaviest transform
in the pipeline. A fixture from a modern toolchain should be added; 10.9's clang
cannot emit one.

## Why not install_name_tool

On these binaries, 10.9's `install_name_tool` **refuses to open the file**:

```
install_name_tool: file not in an order that can be processed (dyld_info out of place)
```

It rebuilds `__LINKEDIT` and expects the 2013-era ordering of its pieces; modern
linkers emit a different order, so it bails before touching anything.

The distinction runs deeper than convenience. `install_name_tool` **rewrites the
file**; these tools **never move a byte of data**, editing only within existing
header padding. That is why `-strip-lc` and `-grow` exist, and why a replacement
path that is too long is an error here and a non-event with Apple's tool.

## Prove it or refuse

`-grow` makes header room by lowering the image base, which invalidates every
structure storing an offset *from* that base: the `LC_FUNCTION_STARTS` leading
delta, `__TEXT,__init_offsets`, the export trie, `LC_DATA_IN_CODE`, and compact
unwind. All five are re-based. Anything unrecognised — an unclassified load
command, an unknown section type — is refused rather than grown past.

Two independent checks back that up:

- **`mg_verify`** proves every base-relative structure resolves to the same
  address after the grow as before. A handler that never ran, ran twice, or ran
  with the wrong delta all look the same to it: a moved address.
- **`mg_plausible`** asks a different question of the finished file — do
  initializers and unwind entries still land on an address `LC_FUNCTION_STARTS`
  lists? It needs no "before" image, so the shared rewriter (`change_dylib` and
  `macho9 dylib`/`rpath`/`lc` alike) runs it immediately before writing and
  refuses rather than committing a bad rewrite. `MACHO_NO_VERIFY=1` opts out.
  `macho9 segment` never reaches this gate at all — its only form always
  builds a rename-only operation set, and the shared rewriter skips the gate
  outright for those rather than offering an opt-out: a rename moves no
  offset, so the gate could only re-decide a property the input already had
  (`src/rewrite.c` has the reasoning and the measurement).

That gate exists because every defect ever found in this code has been a silent
success: the tool reported OK and the binary died in the loader — or worse,
didn't.

## `macho9 edit` — edit scripts

`install.sh`-style porting runs several rewrites in sequence — strip a load
command, then repoint a handful of dylibs — each of which is normally its own
`macho9` invocation and its own full write of the file. `edit` takes a script
naming every statement instead, applies them all to one in-memory copy, and
writes once:

```sh
macho9 edit FILE OUT SCRIPT             # apply SCRIPT to FILE, writing OUT
macho9 edit FILE OUT -                  # read the script from stdin
```

`FILE` is only read, and `OUT` must not be it — the same file twice, or a
symlink or hard link to it, is refused before the script is even read, as it is
for every other verb that names an `OUT`. A successful run always leaves `OUT`
there, even when no statement changed anything: `OUT` is the answer. To see what
a script would do without disturbing anything, give it a scratch `OUT` — that is
the same run, and the result is a file you can inspect rather than a prediction.

`--verbose` may appear anywhere among the arguments, not only after `SCRIPT`.
There is no `--` to end flag parsing, so a `FILE` or `SCRIPT` whose real name
starts with `--` is refused as an unknown flag; reference it through a path that
doesn't, e.g. `./--name`. One leading dash is a file name there, as it is for
every other verb. Not for `OUT`, though: an `OUT` beginning with `-` is refused
and says so, because `OUT` is a file this command creates, so a flag-looking one
is a mistake rather than a name.

**Edit writes nothing unless every statement succeeded.** The whole script is
parsed before `FILE` is opened at all, so a typo in the last line of a long
script costs nothing. Each statement then runs against the image in memory, in
the order written; if any statement is refused, `OUT` is not written and `FILE`
is exactly as it was found. The finished image is verified — mandatorily, after
the last statement and before the write, with no opt-out — and only then
written, once.

**`--verbose` logs, on stderr, what the run did.** Each statement as it
starts; beneath it, indented, the follow-up work it did that its line does
not name — for `dylib insert` and `dylib delete` the ordinal renumbering (the
old-to-new map, and how many nlist entries and `SET_DYLIB_ORDINAL` opcodes
changed), for `fixups set classic` the conversion's figures (rebases and
binds emitted, commands stripped, how far `__LINKEDIT` grew) or that an
already-classic image passed through, for `swift-abi set legacy` how many
class records it retagged, and for `version-min set` the
`LC_VERSION_MIN_MACOSX` it appended; then `FILE: verified` and
`OUT: written (N bytes)`.

**On a fat file, each slice is accounted for too.** `slice NAME:` before an
edited slice's statements and `slice NAME: verified` after; `slice NAME: not
selected by arch; passed through unchanged` or `slice NAME: 32-bit; passed
through unchanged` for the rest; and, once the slices are laid out again,
`slice NAME: moved from offset 0x… to 0x…` for any slice an earlier slice's
growth moved.

**The refusal line on stderr and the exit code are what tell you whether the
file was written.** The operations still print their own progress to stdout
as each statement runs — `FILE: updated (sizeofcmds=...)` and the like — but
during an edit run such a line describes the image in memory, not the file. A
run refused at a later statement, or at verification, writes nothing, even
after printing it. On a fat file the refusal line names the slice too — or,
for a statement's own miss (see `fatal-warnings`, below), says it matched
nothing in any selected slice.

**The write replaces `FILE` by rename**, as `objcopy` does: the new image goes
to a temporary file beside `FILE`, which is then renamed over it, keeping
`FILE`'s mode. So a read-only (`0444`) `FILE` in a writable directory is
replaced, and the run exits 0. (A `FILE` with more than one hard link is
written through in place instead, so that every name sees the change; see
`src/atomic_write.h`.) `macho9 dylib`, `rpath`, `lc`, `segment`, `grow` and
`declassify` do not do this at all any more: each takes `FILE OUT` and never
writes `FILE`, so whether `FILE` is writable is not a question they ask.
`edit` is the verb this section is about, and the last one that still rewrites
the file it is given.

### File format

One statement per line. Fields are whitespace-separated, with single- and
double-quote grouping and backslash escapes — shell word rules. `#` begins a
comment only at the start of an unquoted field, matching shell: `a#b` is the
literal field `a#b`, not `a` followed by a comment. Blank lines are ignored.
A CR (or any other control byte except tab) anywhere in a line is a parse
error, not a silently-accepted character — so a script saved with CRLF line
endings will not parse; use LF.

### Statements

```
load-command  delete    KIND        uuid | codesig | source-version
                                    | build-version | code-sign-drs
segment       rename    OLD NEW
version-min   set       10.9
swift-abi     set       legacy
fixups        set       classic
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

The statements mirror `macho9`'s other rewriting verbs, most spelled as that
verb with its `FILE OUT` dropped — `macho9 dylib FILE OUT -replace A B` is the same edit
as the line `dylib replace A B`. Four are renamed: `lc` is `load-command`,
`minos` is `version-min`, `retag-swift` is `swift-abi`, and `declassify` is
`fixups`. One rewriting verb has no statement at all: `grow FILE OUT N` (enlarge
the header pad by an exact byte count) is not expressible as a line here —
`allow-grow`, below, is the directive that lets a `dylib`, `rpath` or
`version-min set` statement grow the pad on its own as a side effect, which
is a different thing from naming a byte count directly.

Statements run one at a time, in the order written, so each `insert` goes to
the front of the image as the statement before it left it: the lines
`dylib insert A` then `dylib insert B` leave B at ordinal 1 and A at ordinal
2, the reverse of `macho9 dylib FILE OUT -insert A -insert B`, which gives A then
B. `rpath insert` works the same way, so dyld searches B before A.

### Directives

Three, and each must precede every operation in the script — a directive
after *any* operation, not only the one it would have governed, is a parse
error:

```
arch NAME           apply the script only to the slice named NAME (lipo's
                    names: x86_64, x86_64h, arm64, arm64e, i386); repeatable.
                    Without it, every 64-bit slice of a fat file is edited
allow-grow          permission to enlarge the header pad by lowering the image
                    base if new load commands do not fit; opt-in, and refused
                    by default. MH_EXECUTE + MH_PIE only -- the image-base
                    trick needs a __PAGEZERO and no absolute relocations to fix.
                    Covers dylib, rpath and version-min set; not fixups set
                    classic -- nothing grows while the image still has chained
                    fixups, so put fixups set classic first
fatal-warnings      an operation that matched nothing refuses the whole run
                    (exit 1, nothing written) instead of only being reported
```

### Worked example

What `install.sh` does today as three tools and three full writes of a ~200MB
binary:

```sh
patch_macho     "$REAL" "$T"
add_version_min "$T"
change_dylib    "$T" -strip-lc uuid -strip-lc codesig \
    -change "/usr/lib/libSystem.B.dylib"  "@loader_path/../S.dylib" \
    -change "/usr/lib/libicucore.A.dylib" "@loader_path/../I.dylib" \
    -change "/usr/lib/libc++.1.dylib"     "@loader_path/../c++.1.dylib"
```

becomes one script, `claude.edits`:

```
# Claude Code -> 10.9
fixups        set      classic
version-min   set      10.9
load-command  delete   uuid
load-command  delete   codesig
dylib         replace  /usr/lib/libSystem.B.dylib   @loader_path/../S.dylib
dylib         replace  /usr/lib/libicucore.A.dylib  @loader_path/../I.dylib
dylib         replace  /usr/lib/libc++.1.dylib      @loader_path/../c++.1.dylib
```

and one invocation:

```sh
macho9 edit "$REAL" "$T" claude.edits
```

### Limits

- **A fat (universal) file is edited slice by slice, and kept whole.** With
  no `arch` directive every 64-bit slice is edited and 32-bit slices pass
  through; with `arch` directives, exactly the named slices. Naming a slice
  the file lacks, or a 32-bit one, is refused. `edit` never drops a slice —
  thin a file with `lipo` if you want one. A 64-bit fat container
  (`fat_arch_64`) is refused.
- **`allow-grow` reaches `dylib`, `rpath` and `version-min set`** — the
  statements whose load commands can outgrow the header pad — and only on a
  64-bit PIE executable. It does not reach `fixups set classic`: growth
  refuses an image that still has chained fixups. Nor does the conversion
  need it: it removes whichever of `LC_DYLD_EXPORTS_TRIE`,
  `LC_DYLD_CHAINED_FIXUPS` and `LC_BUILD_VERSION` are present before adding
  its 48-byte `LC_DYLD_INFO_ONLY`, so on a modern chained binary, which
  carries all three, it frees at least 56 bytes before using 48. On a
  chained image nothing can grow until `fixups set classic` has run; put it
  first. `segment rename` and `load-command delete` never need it, since
  neither adds bytes to the load commands.
- **`fatal-warnings` covers the statements that can match nothing:**
  `load-command delete` (no command of that kind), `dylib replace/delete/
  reexport` and `rpath replace/delete` (no command naming that path), and
  `segment rename` (no segment of that name). `append` and `insert` always
  act, and the three `set` statements treat "already so" as success, so none
  of those can miss. On a fat file, a statement has matched if it matched in
  any selected slice.
- **`MACHO_NO_VERIFY` does not affect `edit`'s own final verification.** A
  `dylib`/`rpath`/`load-command` statement still runs the same per-step
  plausibility check `macho9 dylib`/`rpath`/`lc` run (see "Prove it or
  refuse" above), and that per-step check still honours the variable. But
  the mandatory check `edit` runs after the *last* statement, before the
  single write, has no such escape hatch, by design — no opt-out was
  reintroduced at this higher level.

## Notes

- Not yet a drop-in replacement for `insert_dylib` on 32-bit or fat inputs, or on
  a binary whose export trie needs a wider ULEB. See `docs/prior-art.md`.
- This repo is its **own upstream**: the tools are not a port of somebody else's
  project. `UPSTREAM_VERSION` is still the family's file and the version is still
  `<version>-mavericks.N`; what differs is that no Renovate customManager watches
  it, because nothing external releases it. See `INGREDIENTS.md`.

## Provenance

Extracted with full history from
[Wowfunhappy/Mavericks-Porting-Resources](https://github.com/Wowfunhappy/Mavericks-Porting-Resources).
`patch_macho` and `fix_macho` are substantially Wowfunhappy's; header growth, the
re-basers, ordinal renumbering and verification are Amitai Schleier's. The commit
log is the accurate record. Details, including what was extracted and from where:
[`PROVENANCE.md`](PROVENANCE.md).

Four commits in that history also touched files that stayed behind, so their
messages mention unrelated work. The changes are correct; only the messages are
wider than their diffs.
