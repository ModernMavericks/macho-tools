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

## Building

```sh
./build.sh
```

Builds everything into `build/` and runs both test suites.

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
  lists? It needs no "before" image, so `change_dylib` runs it immediately before
  writing and refuses rather than committing a bad rewrite. `MACHO_NO_VERIFY=1`
  opts out.

That gate exists because every defect ever found in this code has been a silent
success: the tool reported OK and the binary died in the loader — or worse,
didn't.

## Notes

- Not yet a drop-in replacement for `insert_dylib` on 32-bit or fat inputs, or on
  a binary whose export trie needs a wider ULEB. See `docs/prior-art.md`.
- `build.sh` is a deviation: the ModernMavericks family builds with CMake against
  [shared-cmake](https://github.com/ModernMavericks/shared-cmake). Tracked.

## Provenance

Extracted with full history from
[Wowfunhappy/Mavericks-Porting-Resources](https://github.com/Wowfunhappy/Mavericks-Porting-Resources).
`patch_macho` and `fix_macho` are substantially Wowfunhappy's; header growth, the
re-basers, ordinal renumbering and verification are Amitai Schleier's. The commit
log is the accurate record.

Four commits in that history also touched files that stayed behind, so their
messages mention unrelated work. The changes are correct; only the messages are
wider than their diffs.
