# Prior art: Wowfunhappy/insert_dylib

[`Wowfunhappy/insert_dylib`](https://github.com/Wowfunhappy/insert_dylib), a fork
of [`tyilo/insert_dylib`](https://github.com/tyilo/insert_dylib), independently
grew a header-expansion path — commit `6d3aa61`, "Handle binaries without enough
space. (Vibecoded)", +701 lines — using **the same geometry these tools do**:
lower `__TEXT`'s vmaddr, then fix up what that invalidates.

Measured 2026-09-08 against its `main.c` at HEAD:

| | insert_dylib | macho-tools |
|---|---|---|
| export trie | **rebuilds it** — handles a ULEB that widens | in place at original width; **rebuilds it** (src/trie.c) when one would widen |
| 32-bit (`LC_SEGMENT`) | yes | **no** — 64-bit only |
| fat binaries in the rewrite path | yes | yes — `change_dylib` now walks fat slices too |
| `S_INIT_FUNC_OFFSETS` | yes | yes |
| `LC_FUNCTION_STARTS` leading delta | no | yes |
| `LC_DATA_IN_CODE` contents | no | yes |
| `__TEXT,__unwind_info` | no | yes |
| unknown load command | proceeds | refuses |
| post-transform verification | none | `mg_verify` + `mg_plausible` |

One row this table omitted, noticed 2026-09-11 while surveying app-backport
projects: **`LC_LOAD_WEAK_DYLIB`**. `insert_dylib` has `--weak`; `macho9`'s
`dylib append` and `dylib insert` emit only `LC_LOAD_DYLIB`, and there is no way
to flip an existing one either way. `mo_is_dylib_lc` already counts both kinds,
so the flip moves no ordinals. Tracked as gap 3 of queue item 13.

## Why this matters

Two independent implementations converging on the same trick is evidence the
trick is right. It also means neither is finished: each covers cases the other
misses, and the union is what the tool should be.

The three gaps on this side were tracked as issues. All three are now closed
(below); **macho-tools remains not a drop-in replacement for insert_dylib** on
32-bit input specifically — that one stays refused on purpose, not as an open
gap — see below.

## Status (toolkit plan Task 5)

All three are resolved:

- **Fat binaries in the rewrite path**: closed. `change_dylib` now walks a fat
  container's slices the way `fix_macho` always has, rewriting each 64-bit
  slice and passing any slice it cannot understand through byte-for-byte
  unchanged (matching `fix_macho`'s own "skip this arch" convention) — see
  `process_fat` in `change_dylib.c` and cases 10/11 in `change_dylib_test.sh`.
- **32-bit (`LC_SEGMENT`)**: stays refused, deliberately, not left open.
  `mg_grow_header` and everything it calls (`mg_first_sect_off`, `mg_collect`,
  `mg_classify`, `mg_unwind_walk`, `mg_init_offsets_pass`, the segment-patching
  loop) walk `LC_SEGMENT_64`/`section_64` directly; supporting 32-bit would mean
  a parallel `LC_SEGMENT`/`struct section` path through roughly seven places in
  the file whose correctness already rests on ULEB-exact, snapshot-verified
  arithmetic. `src/image.h` already draws the identical 64-bit-only line, and
  every one of this toolkit's seven rewriters already refuses 32-bit input at
  its very first header check — this is that same boundary, made an explicit,
  regression-tested fact (`tests/grow_test.c`'s
  `test_grow_refuses_32bit_mach_header`, `tests/image_test.c`'s
  `test_wrap_refuses_32bit_mach_header`) instead of an incidental side effect.

  **What would reopen it** (recorded 2026-09-11, so the condition travels with
  the decision): a target older than 10.9. These tools exist to make software
  run on a host whose own toolchain cannot build it — today that host is
  Mavericks, and its inputs come from toolchains fifteen years newer, which
  emit no `i386` at all. Point the same technique one OS down and that stops
  being true: Snow Leopard is the last release for Core Duo and Core Solo
  hardware, which is 32-bit only, so a backport effort aimed at 10.6 is
  working on `i386` images from the start, not on stragglers.

  That matters because it decides *which* 32-bit work is needed. Editing an
  old binary in place — an install-name change of equal length, a
  `-strip-lc`, a segment rename — needs only an `LC_SEGMENT`/`struct section`
  parsing path, which is bounded and mechanical. Swapping a backport dylib in
  needs header room, which is the parallel growth geometry described above and
  the expensive half. The backport pattern this repo was built around is
  exactly dylib injection, so a real 10.6 effort would need the expensive half
  immediately; there is no cheap subset that gets it started. The condition to
  watch for, then, is not "somebody has a 32-bit file" but "a Snow Leopard
  target is actually being pursued".
- **Export trie rebuild**: closed. When an address's ULEB would widen under an
  in-place patch (`mg_trie_node`'s `return 1`), `mg_grow_header` now rebuilds
  the trie from scratch (`src/trie.c`, `mt_trie_rebuild`) instead of refusing:
  decode it, add the shift to every nonzero address, re-serialize with
  everything minimally encoded. If the result still fits the original
  `export_size`, it's patched in place, same as before; if it doesn't,
  `__LINKEDIT` is grown to hold it (appended at its current end, which the
  code first confirms really is the end of the file — refusing rather than
  guess if it isn't). See `tests/grow_test.c`'s
  `test_grow_rebuilds_widening_export_trie` (a fixture built specifically to
  widen, per this task's own instruction to construct one rather than hunt for
  one) and `tests/trie_test.c` for the module's own hermetic tests. The
  licensing question below is now settled.

## On taking the code

Neither `Wowfunhappy/insert_dylib` nor `tyilo/insert_dylib` states a licence, so
the default is all rights reserved, and this repo is CC0.

**Settled 2026-09-08.** The trie rebuild is Wowfunhappy's own addition
(`6d3aa61`), so it was his to relicense, and he has done so: in
[`Wowfunhappy/Mavericks-Porting-Resources` issue
#4](https://github.com/Wowfunhappy/Mavericks-Porting-Resources/issues/4)
(closed 2026-09-08) he states "Anything original in this repo is released into
the public domain / licensed under CC0 / licensed under WTFPL, please use it
for any purpose you'd like!" — a blanket statement covering his own additions
across his repos, `insert_dylib` included, and the repo owner has confirmed
this settles it. `src/trie.c`/`src/trie.h` are adapted (not copied) from that
commit: no global mutable state, this repo's own `src/uleb.h` instead of a
second ULEB implementation, and every one of the reference's fixed caps
(`TRIE_MAX_EDGES` 128, `payload[64]`, `ch[].lbl[256]`) either removed (edges
and labels are bounded only by the input's own encoding, not a second
arbitrary limit) or replaced with an explicit refusal instead of the silent
truncation those caps produced. See `src/trie.h`'s header comment for the
full adaptation note.

The 32-bit and fat handling is closer to tyilo's base, which states no
licence; that part was reimplemented from the wire format, not copied.
