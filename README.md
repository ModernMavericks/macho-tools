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
  their original names because `install.sh` fetches some of them by name.
  Five are now `/bin/sh` wrappers that print the `macho9` equivalent of what
  they were asked to do and then do it through `macho9`; `fix_macho` is still
  C, because it could not be wrapped without changing what it does — so this
  repo still ships **two** Mach-O rewriting binaries, not the one the
  retirement plan is aiming at. Also here: `translate.sh`, the
  old-grammar-to-`macho9` translator the wrappers source, and
  `macho9-compat.sh`, the machinery they share. See `compat/README.md`.

  **Packaging note:** the five wrappers need `macho9`, `macho9-compat.sh` and
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
