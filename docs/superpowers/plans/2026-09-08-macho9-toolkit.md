# macho9 Toolkit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn seven separate Mach-O rewriters that never share a line into one
library plus one multi-call CLI (`macho9`), without losing a single behaviour or
test along the way.

**Architecture:** Extract the duplicated primitives bottom-up (`uleb`, `image`,
`ordinals`), then converge `fix_macho` and `change_dylib` behind the settled
subcommand grammar so the CLI breaks exactly once. Each step must leave the repo
green — this code is shipped to real users through
`mavericksforever.com/claude/install.sh`.

**Tech Stack:** C (no `-std`; see below), CMake against `ModernMavericks/shipyard`,
CTest, stock 10.9 clang natively and a modern clang cross.

**Spec:** `docs/PROPOSAL.md` — read it first and in full. It settles the verb
grammar, the one-repo decision, and why `install_name_tool` is not an
alternative. This plan implements its "Sequencing" section.

## Where things stand (read before starting)

You are in `ModernMavericks/macho-tools`, checked out locally as
`~/Documents/code/trees/mavericks-macho-tools`. 40 commits, extracted with full
history from `Wowfunhappy/Mavericks-Porting-Resources` — see `PROVENANCE.md`.

**Already done, do not redo:**

| proposal step | state |
|---|---|
| 4. `verify` | **done.** `mg_verify` (invariant) and `mg_plausible` (no "before" needed) live in `macho_grow.h`; `change_dylib` runs the latter immediately before writing and refuses rather than committing a bad rewrite. `MACHO_NO_VERIFY=1` opts out. |
| 6. `release.yml` | **half done.** CI cross-builds on `macos-26`, gates on the compat guard, runs the suite, and proves build equivalence. No `.pkg` or Sparkle updater yet, and no publish job. |

**Current shape:** seven `.c` files plus `macho_grow.h` at the repo root, 3469
lines total. `change_dylib.c` is 713 and `macho_grow.h` is 1129 — the two that
carry most of the duplication the proposal is about.

**Tests (all green, `ctest`):** `macho_grow_test` (hermetic), `change_dylib_test`
(builds real dylibs, rewrites a real executable, runs it), `chained_fixups`
(modern-host only; SKIPs on 10.9), `characterize` (build equivalence).

## Global Constraints

- **Every task ends green.** `ctest` passes and the family conventions gate
  (`sh $SHIPYARD/scripts/check-family-conventions.sh`) says ok. This code ships
  to real users; a red main is not a work-in-progress state here.
- **Do not set `-std=`.** `patch_macho.c` assigns to an anonymous struct, which
  clang rejects under `-std=c11` and accepts in its default gnu dialect.
- **`tests/EXPECTED` is a characterization reference, not an output to update.**
  If `characterize` fails, either you changed behaviour (say so in the same
  commit and explain why) or the build is not equivalent to the native one.
  Never edit it to make a test pass.
- **The tools must never move a byte of file data** — they edit within existing
  header padding. That constraint is the whole reason `-strip-lc` and `-grow`
  exist, and it is what distinguishes these from `install_name_tool`.
- **`-grow` refuses rather than guessing.** Every load command and section type
  is classified; anything unrecognised refuses. Preserve that: adding a case
  means adding it deliberately, never widening the default.
- **Build both ways before claiming a change is safe:** `cmake --preset native`
  on 10.9 and `cmake --preset cross` on a modern host. CI does the cross half.

---

## File Structure

The proposal's target layout. Build it incrementally — do not create empty files
ahead of the task that fills them.

| file | responsibility |
|---|---|
| `src/uleb.c` / `.h` | decode / `encode_fixed` / `minlen`. Today duplicated in `macho_grow.h` and re-derived in `patch_macho.c` |
| `src/image.c` / `.h` | open, validate, iterate; find segment, section, load command |
| `src/ordinals.c` / `.h` | the library-ordinal map: build, apply, validate |
| `src/linkedit.c` / `.h` | symtab, strtab, indirect symbols; the offset-bump table |
| `src/grow.c` / `.h` | header growth plus every base-relative fixup it invalidates (from `macho_grow.h`) |
| `src/live.h` | header-only, malloc-free: the same queries against loaded images. **Never grows a `.c`** — avxemu's SIGILL handler must stay allocation-free |
| `cli/macho9.c` | the verbs |
| `tests/` | existing suites, unchanged in behaviour |

---

### Task 1: Extract `uleb` — the smallest shared thing, to prove the pattern

**Files:**
- Create: `src/uleb.h`, `src/uleb.c`
- Modify: `macho_grow.h` (delete its copies, include the new header)
- Modify: `CMakeLists.txt`
- Test: existing `macho_grow_test` covers this — it already tests
  `mg_uleb_decode`, `mg_uleb_minlen`, `mg_uleb_encode_fixed` directly

**Interfaces:**
- Produces: `mu_decode(const uint8_t *p, const uint8_t *end, uint64_t *out) -> int`
  (bytes consumed, 0 malformed), `mu_minlen(uint64_t v) -> int`,
  `mu_encode_fixed(uint8_t *p, uint64_t v, int width) -> int` (1 ok, 0 does not
  fit). Same semantics as today's `mg_uleb_*`; only the prefix changes.

- [ ] **Step 1: Confirm the existing tests actually exercise these**

```bash
grep -n 'uleb' macho_grow_test.c | head
```

Expected: `test_uleb_decode`, `test_uleb_minlen`, `test_uleb_encode_fixed`. If
they were not already there this task would need new tests first; they are.

- [ ] **Step 2: Move the three functions verbatim into `src/uleb.c`**

Copy `mg_uleb_decode`, `mg_uleb_minlen` and `mg_uleb_encode_fixed` from
`macho_grow.h` unchanged, renaming the prefix `mg_uleb_` → `mu_`. Do not
"improve" them in this commit — a behaviour change hidden inside a move is the
hardest kind of regression to find.

- [ ] **Step 3: Have `macho_grow.h` include it, and delete its copies**

```c
#include "uleb.h"
#define mg_uleb_decode       mu_decode
#define mg_uleb_minlen       mu_minlen
#define mg_uleb_encode_fixed mu_encode_fixed
```

The defines keep this task to one concern. Task 2 removes them.

- [ ] **Step 4: Build and test both presets**

```bash
cmake --preset native && cmake --build --preset native && ctest --preset native
```

Expected: 4/4 (3 passed, `chained_fixups` skipped on 10.9). Push and confirm CI
is green before continuing — that is the cross half.

- [ ] **Step 5: Commit**

```bash
git add src/uleb.c src/uleb.h macho_grow.h CMakeLists.txt
git commit -m "Extract uleb: decode, minlen, encode_fixed

Verbatim move, prefix mg_uleb_ -> mu_, with defines in macho_grow.h so
this commit changes no behaviour. patch_macho.c re-derives the same
decode; Task 3 points it here too.

macho_grow_test already tested all three directly, which is why this is
the first extraction: the safety net predates the move."
```

- [ ] **Step 6: Drop the defines**

Replace the `mg_uleb_*` call sites in `macho_grow.h` with `mu_*` and delete the
`#define`s. Rebuild, retest, commit separately — a rename is easier to review
apart from a move.

---

### Task 2: Extract `image` — open, validate, iterate

**Files:**
- Create: `src/image.h`, `src/image.c`
- Modify: `macho_grow.h`, `change_dylib.c`, `patch_macho.c`
- Test: `tests/image_test.c` (new), plus the existing suites as regression

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces:
  - `mi_open(const char *path, mi_image *out) -> int` — read whole file, validate
    magic/filetype, populate `mi_image { uint8_t *buf; size_t size; struct mach_header_64 *hdr; }`
  - `mi_each_lc(const mi_image *, mi_lc_fn cb, void *ctx)` — iterate load commands
  - `mi_find_segment(const mi_image *, const char *name) -> struct segment_command_64 *`
  - `mi_find_section(const mi_image *, const char *seg, const char *sect) -> struct section_64 *`
  - `mi_text_base(const mi_image *) -> uint64_t` — the segment mapping the header

Every one of these exists three or four times today under different names. Grep
before writing: `grep -n 'ncmds' *.c *.h` finds the hand-rolled walks.

- [ ] **Step 1: Write the failing test**

```c
/* tests/image_test.c */
#include "image.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    mi_image im;
    assert(mi_open("tests/fixture.macho", &im) == 0);
    assert(im.hdr->magic == MH_MAGIC_64);
    assert(mi_find_segment(&im, "__TEXT") != NULL);
    assert(mi_find_segment(&im, "__NOPE") == NULL);
    assert(mi_text_base(&im) != 0);
    printf("image_test: all cases pass\n");
    return 0;
}
```

- [ ] **Step 2: Run it, watch it fail to link**

```bash
cc -I src -o /tmp/it tests/image_test.c
```

Expected: undefined symbols for every `mi_*`. That is the correct red for C.

- [ ] **Step 3: Implement `src/image.c` minimally**

Only what the test needs. Resist adding the whole proposal's API here.

- [ ] **Step 4: Green, then register in CMake and CTest**

```cmake
add_executable(image_test tests/image_test.c src/image.c)
target_include_directories(image_test PRIVATE src)
add_test(NAME image_test COMMAND image_test WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
```

- [ ] **Step 5: Convert ONE caller, and only one**

`patch_macho.c` has the simplest walk. Convert it, run the full suite —
`chained_fixups` is the one that exercises `patch_macho` hardest, and it only
runs on a modern host, so **push and check CI** rather than trusting a local
native pass.

- [ ] **Step 6: Commit, then convert the remaining callers one commit each**

`change_dylib.c` and `macho_grow.h` are larger. One per commit, suite green
between each. If a conversion is not behaviour-preserving, that is a finding
worth its own commit and message, not something to smuggle into a refactor.

---

### Task 3: Extract `ordinals`, so the emitter and the renumberer agree by construction

**Files:**
- Create: `src/ordinals.h`, `src/ordinals.c`
- Modify: `change_dylib.c`
- Test: `change_dylib_test.sh` already covers this end to end

**Interfaces:**
- Produces: `mo_map_build`, `mo_map_apply`, `mo_map_validate` over a
  `mo_map { int *old_to_new; int n; }`.

The proposal's argument for this one is the point: `-delete` once produced
binaries dyld refused (`library ordinal (4) too big`) because the renumberer and
the emitter disagreed. Sharing the map makes that class of bug unrepresentable
rather than merely fixed.

- [ ] **Step 1: Confirm the existing coverage before moving anything**

```bash
sh change_dylib_test.sh 2>&1 | grep -c PASS
```

Expected: 14 or more, including "renumbered, survivors still resolve" and
"refuses to orphan symbols bound to the deleted dylib". These are the net.

- [ ] **Step 2: Move `renumber_ordinals` and its map into `src/ordinals.c`**

Verbatim first. Rename after, in a separate commit.

- [ ] **Step 3: Full suite, both presets, then commit**

---

### Task 4: The `macho9` CLI, behind the settled grammar

**Files:**
- Create: `cli/macho9.c`
- Modify: `CMakeLists.txt`
- Test: `tests/cli_test.sh` (new)

**Interfaces:**
- Produces the grammar `docs/PROPOSAL.md` settles. Do not re-litigate it; it was
  decided with reasons, including why Apple's spellings are deliberately not
  adopted as synonyms.

```
macho9 declassify IN OUT
macho9 dylib FILE [--allow-grow] OP...     -replace -delete -append -insert -reexport
macho9 rpath FILE [--allow-grow] OP...     -replace -delete -append -insert
macho9 lc FILE -delete KIND
macho9 grow FILE N
macho9 minos FILE 10.9
macho9 info FILE
macho9 verify FILE
```

- [ ] **Step 1: `macho9 --capabilities` first**

The proposal's migration hinges on it: the tool reports which grammar and verbs
a given build accepts, so the tool and the wrapper never have to move in
lockstep. Build this before any verb, and test it.

- [ ] **Step 2: One verb at a time, each with a test, each delegating to existing code**

`macho9 verify` first — it already exists as `mg_plausible`, so the verb is a
thin shell and proves the dispatch works.

- [ ] **Step 3: Keep the old entry points working**

`change_dylib` survives as a compatibility entry point over `macho9 dylib` for
the transition, per the proposal. Removing it is a later `MF_GEN`, not this plan.
`mavericksforever.com/claude/install.sh` builds `patch_macho`, `change_dylib` and
`add_version_min` by those names; breaking them breaks real installs.

---

### Task 5: Close the insert_dylib parity gaps

**Files:** `src/grow.c` (or `macho_grow.h` if Task 2 has not moved it yet)

See `docs/prior-art.md` for the measured comparison. Three gaps, each already
filed as an issue:

1. **Export trie rebuild** when an address's ULEB would widen. Today we refuse.
   The existing implementation is Wowfunhappy's own commit in `insert_dylib`, so
   it is his to relicense — **ask before taking**; that repo states no licence.
2. **32-bit Mach-O.** `mg_grow_header` requires 64-bit.
3. **Fat binaries in the rewrite path.** `fix_macho` handles fat; `change_dylib`
   does not. The proposal's step 2 converges them, which is where this lands.

Each needs a test that fails first. For 1, that means a fixture whose trie
genuinely widens — construct it rather than hunting for one.

---

## What is NOT in this plan

`port`, the `.pkg`, the Sparkle updater and the publish job. The proposal
sequences them after `verify`, and `verify` is done, so they are ready to start —
but they are packaging work with their own shape and deserve their own plan. The
family conventions skill (`modernmavericks-conventions`, in the `modernmavericks`
plugin) is the spec for that one.

## Self-Review

**Spec coverage.** Proposal steps 1 and 3 → Tasks 1–3. Step 2's grammar → Task 4.
Step 4 `verify` → already done, recorded above. Step 5 `port` and step 6's
packaging half → explicitly out of scope, with a reason.

**Placeholders.** None: every step names its files, its command, or its code.

**Consistency.** Prefixes are `mu_` (uleb), `mi_` (image), `mo_` (ordinals),
matching the proposal's `src/` layout. The existing `mg_` prefix stays until a
file actually moves.
