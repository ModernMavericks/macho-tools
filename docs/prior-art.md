# Prior art: Wowfunhappy/insert_dylib

[`Wowfunhappy/insert_dylib`](https://github.com/Wowfunhappy/insert_dylib), a fork
of [`tyilo/insert_dylib`](https://github.com/tyilo/insert_dylib), independently
grew a header-expansion path — commit `6d3aa61`, "Handle binaries without enough
space. (Vibecoded)", +701 lines — using **the same geometry these tools do**:
lower `__TEXT`'s vmaddr, then fix up what that invalidates.

Measured 2026-09-08 against its `main.c` at HEAD:

| | insert_dylib | macho-tools |
|---|---|---|
| export trie | **rebuilds it** — handles a ULEB that widens | in place at original width; **refuses** if one would widen |
| 32-bit (`LC_SEGMENT`) | yes | **no** — 64-bit only |
| fat binaries in the rewrite path | yes | **no** in `change_dylib`; only `fix_macho` handles fat |
| `S_INIT_FUNC_OFFSETS` | yes | yes |
| `LC_FUNCTION_STARTS` leading delta | no | yes |
| `LC_DATA_IN_CODE` contents | no | yes |
| `__TEXT,__unwind_info` | no | yes |
| unknown load command | proceeds | refuses |
| post-transform verification | none | `mg_verify` + `mg_plausible` |

## Why this matters

Two independent implementations converging on the same trick is evidence the
trick is right. It also means neither is finished: each covers cases the other
misses, and the union is what the tool should be.

The three gaps on this side are tracked as issues. Until they close,
**macho-tools is not a drop-in replacement for insert_dylib** on 32-bit or fat
inputs, or on a binary whose export trie needs a wider ULEB — and it should not
be described as one.

## Status (toolkit plan Task 5)

Of the three, two are resolved:

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
  regression-tested fact (`macho_grow_test.c`'s
  `test_grow_refuses_32bit_mach_header`, `tests/image_test.c`'s
  `test_wrap_refuses_32bit_mach_header`) instead of an incidental side effect.

The **export trie rebuild** is the one gap left open — it is the licensing
question above, which is the human maintainer's to resolve, not something to
work around or reimplement from reading Wowfunhappy's commit.

## On taking the code

Neither `Wowfunhappy/insert_dylib` nor `tyilo/insert_dylib` states a licence, so
the default is all rights reserved, and this repo is CC0.

The trie rebuild is Wowfunhappy's own addition (`6d3aa61`), so it is his to
relicense — he has already stated CC0/WTFPL terms for his original code in
`Mavericks-Porting-Resources` and offered written consent for other licences on
request. **Ask before taking.** The 32-bit and fat handling is closer to tyilo's
base; reimplement rather than copy.
