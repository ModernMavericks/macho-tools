# Finish the Convergence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Empty the repository root of code. Every `.c` and `.h` at the top level
moves into `src/` (library), `cli/` (verbs), or `compat/` (historical entry
points), and the duplication those files carry is removed in the move rather
than carried along with it.

**Why now:** the 2026-09-08 plan built the library but converted almost none of
the callers. Measured after it: **54** hand-rolled `ncmds` walks where there were
41, because the library added its own without removing the originals. Three of
six tools consume nothing from `src/`. That plan's own target layout named
`src/grow.c`, `src/linkedit.c` and `src/live.h` — and no task in it ever created
them. This plan finishes what that one started.

**What changed that makes this safe now:** the test suite. At the start of the
previous plan there were 4 ctest entries and 15 `change_dylib_test` assertions.
There are now 7 ctest entries, 32 `change_dylib_test` assertions, a 54-assertion
`cli_test`, plus `image_test` and `trie_test` — and `characterize` pins the
tools' output byte-for-byte against `tests/EXPECTED`. That is the safety net the
previous plan lacked, and it is why the conversions it deferred can be done now.

**Spec:** `docs/PROPOSAL.md` — its "Shape" section is the target layout. Read it
first. This plan implements the parts the previous plan left untasked.

## Where things stand (read before starting)

Root currently holds, and all of it must move:

| file | walks | destination |
|---|---|---|
| `macho_grow.h` | 12 | `src/grow.c` + `src/grow.h` (Task 3) |
| `macho_grow_test.c` | 6 | `tests/grow_test.c` (Task 3) |
| `patch_macho.c` | 1 | `compat/` (Task 1, Task 5) |
| `change_dylib.c` | 1 | `compat/` (Task 1, Task 5) |
| `add_version_min.c` | 1 | `compat/` (Task 1, Task 5) |
| `fix_macho.c` | 1 | `compat/` (Task 1, Task 5) |
| `rename_segment.c` | 1 | `compat/` (Task 1, Task 5) |
| `retag_swift_classes.c` | 1 | `compat/` (Task 1, Task 5) |
| `change_dylib_test.sh` | — | `tests/` (Task 5) |

Already in `src/`: `uleb`, `image`, `ordinals`, `fat`, `trie`. Not yet built:
`linkedit`, `grow`, `live`.

## Global Constraints

Identical to the previous plan; they still bind.

- **Every task ends green.** `ctest` passes, `sh ./change_dylib_test.sh` passes,
  `sh ./tests/cli_test.sh` passes, and
  `sh "$SHIPYARD_SCRIPTS/check-family-conventions.sh"` says ok. This code ships
  to real users; a red `main` is not a work-in-progress state.
- **Do not set `-std=`.** `patch_macho.c` assigns to an anonymous struct, which
  clang rejects under `-std=c11` and accepts in its default gnu dialect.
- **`tests/EXPECTED` is a characterization reference, not an output to update.**
  If `characterize` fails you changed behaviour. Say so in the same commit and
  explain why; never edit that file to make a test pass. In this plan
  `characterize` is the primary proof that a move changed nothing.
- **The tools must never move a byte of file data.** Two reviewed exceptions
  already exist and stay: `-grow`'s `memmove` after the load commands, and the
  export-trie append past `__LINKEDIT`'s end.
- **`-grow` refuses rather than guessing.** Never widen a default case.
- **Build both ways:** `cmake --preset native` on 10.9 and `cmake --preset cross`
  on a modern host. CI does the cross half.
- **Do not break installs.** `mavericksforever.com/claude/install.sh` builds
  `patch_macho`, `change_dylib` and `add_version_min` **by those names**. They
  must keep building under those names from this repo throughout. Moving a
  source file is fine; changing an installed binary's name is not.

## Host portability (learned the expensive way)

The previous plan went red on the modern cross runner **seven times** and every
single one was a test assumption, never a tool defect. `tests/README.md` records
the rules; follow them:

- Fixtures need `-mmacosx-version-min=10.9` or they ask a different question per host.
- Never parse `nm`/`otool` human-readable output as an oracle.
- A 2026 linker emits `LC_BUILD_VERSION` where a 2014 one emits `LC_VERSION_MIN_MACOSX`.
- `-Wl,-no_version_load_command` exists only on 10.9's ld.
- Modern macOS SIGKILLs a resized binary (signature invalidation), so
  "run the rewritten binary" assertions are 10.9-only: hard on target, loud skip
  elsewhere, and FAIL rather than SKIP on an unrecognised signal.
- Assert the behaviour, not which guard fired — fixture byte sizes vary by toolchain.

---

### Task 1: Convert the six leaf tools — smallest walks, biggest coverage win

Each of the six tools has exactly one `for (i = 0; i < hdr->ncmds; i++)` walk and
its own `open`/`fstat`/`malloc`/`read`/check-magic preamble. Three of them
(`add_version_min`, `rename_segment`, `retag_swift_classes`) consume nothing from
`src/` at all.

**Files:** modify all six `.c` files at root and `CMakeLists.txt`. No moves yet.

**Interfaces:** consume `mi_open` / `mi_open_slack` / `mi_release` / `mi_each_lc` /
`mi_find_segment` / `mi_find_section` from `src/image.h`. Note `patch_macho`
needs `mi_open_slack` (it appends into its buffer tail) and `change_dylib` needs
`mi_release` (it reallocs via `mg_grow_header`) — both already converted once and
`change_dylib`'s was later undone; restore it.

- [ ] **Step 1: One tool per commit, `characterize` green between each**

Start with `add_version_min` (simplest, ~70 lines). Then `rename_segment`,
`retag_swift_classes`, `fix_macho`, `patch_macho`, `change_dylib`.

- [ ] **Step 2: Each conversion must reduce the walk count, not add one**

```bash
grep -c 'ncmds; i++' *.c *.h | sort -t: -k2 -rn
```

Record the number before and after in each commit message. If a conversion does
not remove a walk, it is not a conversion.

- [ ] **Step 3: Link only what each tool actually uses**

`target_link_libraries(<tool> PRIVATE macho9core)` per tool, added as the tool
gains its first `src/` dependency — so the dependency is visible in
`CMakeLists.txt`, not implicit.

- [ ] **Step 4: Push and confirm CI is green before Task 2**

---

### Task 2: A stop-capable `mi_each_lc`, then extract `linkedit`

**Task 2a — give `mi_each_lc` a way to stop and a way to mutate.** Task 1's
review accepted three walks as unconvertible and identified the single change
that would retire the whole category: `mi_lc_fn` returns `void` and
`mi_each_lc` has no abort, so a walk with an early `return -1` on malformed
input (`change_dylib.c:209,251`) cannot be expressed in it — a converted version
would keep writing after the refusal. And `fix_macho.c:57` mutates the chain
mid-walk (`memmove`, `ncmds--`, `continue` without advancing), which the
iterator's contract forbids.

- [ ] Add an `int`-returning callback (non-zero stops the walk) and have
      `mi_each_lc` return whether it completed, so a refusal propagates.
- [ ] Decide deliberately whether a mutating variant is worth it, or whether
      chain-editing walks stay hand-rolled. Either answer is fine; record it.
- [ ] Fix `src/image.h`'s contract comment: "must not change any `cmd`,
      `cmdsize`, or `hdr->ncmds`" — the current wording is read by converters
      as either a prohibition on what `rename_segment` already does, or as
      blanket permission.
- [ ] Then convert `change_dylib.c`'s `build_lcs` and `patch_macho.c`'s
      collecting walk, which the new form makes expressible.

**Task 2b — extract `linkedit`, the offset-bump table**

`macho_grow.h`'s densest internal duplication: symtab, strtab, indirect symbols,
and the exhaustive list of `__LINKEDIT` file-offset fields that every grow must
bump. `mg_bump` and its call sites are the seam.

**Files:** create `src/linkedit.c` / `src/linkedit.h`, `tests/linkedit_test.c`;
modify `macho_grow.h`, `CMakeLists.txt`.

**Interfaces:** `ml_bump_all(mi_image *, uint32_t insert, uint32_t grow)` plus
per-command helpers. Prefix `ml_`.

- [ ] **Step 1: Write `tests/linkedit_test.c` first**

Build a synthetic image via `mi_wrap` with `LC_SYMTAB`, `LC_DYSYMTAB`,
`LC_FUNCTION_STARTS` and `LC_DATA_IN_CODE`, bump it, assert every offset moved by
exactly the right amount and that nothing else changed. Watch it fail to link.

- [ ] **Step 2: Verbatim move, then rename in a separate commit**

The previous plan's Task 1 established this rhythm and it caught real problems.
Keep it: a behaviour change hidden inside a move is the hardest kind to find.

- [ ] **Step 3: `characterize` is the gate**

This code decides where every byte of `__LINKEDIT` lands. If `characterize`
passes, the move preserved behaviour. If it fails, stop and explain.

---

### Task 3: Move `macho_grow.h` to `src/grow.c` — the twelve walks

The big one. 12 walks in one header, plus 6 more in its test.

**Files:** create `src/grow.c` / `src/grow.h`; delete `macho_grow.h`; move
`macho_grow_test.c` to `tests/grow_test.c`; modify `change_dylib.c`,
`CMakeLists.txt`, `change_dylib_test.sh`.

- [ ] **Step 1: Move the file wholesale, unchanged, in one commit**

Header-only to `.c` + `.h` means the `static` functions gain external linkage.
Do that mechanically and nothing else. `characterize` and `macho_grow_test` are
the proof. Update `change_dylib_test.sh`'s standalone compile line — it compiles
`change_dylib.c` itself and lists `src/*.c` explicitly.

- [ ] **Step 2: Convert the 12 walks, in small commits, `characterize` between each**

Use `mi_each_lc` where a walk only iterates, and `mi_find_segment` /
`mi_find_section` where it searches. Some walks accumulate into several locals —
where a callback context would be a lateral move rather than a de-duplication,
say so in the commit message and leave that one. That judgement was made
correctly for `patch_macho` in the previous plan; the same standard applies.

- [ ] **Step 3: Fold the test's 6 walks into shared helpers**

`tests/grow_test.c` builds synthetic images. It should use `mi_wrap` and the
finders like everything else.

---

### Task 4: `src/live.h` — the malloc-free header for avxemu

**Confirmed requirement:** avxemu consumes this. Its core must stay VEX-free and
its SIGILL handler async-signal-safe, so it cannot link anything that might
allocate. The shared thing is the *structure knowledge*, not the code path.

**Files:** create `src/live.h`, `tests/live_test.c`; modify `CMakeLists.txt`.

- [ ] **Step 1: Header-only, no `.c`, ever**

`src/live.h` must never grow a `.c`. Every function `static inline`. No `malloc`,
no `free`, no `stdio`, no locks, no anything a signal handler cannot call.

- [ ] **Step 2: Test it against a loaded image, not a file**

The queries run against `_dyld_get_image_header`-style pointers, not a buffer
read from disk. `tests/live_test.c` should load itself and query its own image.

- [ ] **Step 3: Pin the constraint with a test that would catch a regression**

Assert the header pulls in no allocating header: a test translation unit that
`#include`s `src/live.h` and nothing else must compile and link with no
references to `malloc`/`free`. `nm` the object and check.

---

### Task 5: Empty the root

**Files:** move the six tool sources to `compat/`; move `change_dylib_test.sh` to
`tests/`; modify `CMakeLists.txt`, `.github/workflows/`, `docs/`.

- [ ] **Step 1: Move the six tools to `compat/`, keeping their installed names**

```cmake
add_executable(change_dylib compat/change_dylib.c)
```

The binary is still `change_dylib`. `install.sh` builds it by name and must keep
working. Only the source path changes.

- [ ] **Step 2: Move `change_dylib_test.sh` into `tests/`, and stop it rebuilding the tool by hand**

It `cd`s to its own directory and compiles `change_dylib.c` ITSELF to stay
standalone-runnable. Both need updating — but fix the deeper problem while you
are there.

Line 43 today reads:

```sh
"$CC" -O2 -I src -o "$T/change_dylib" change_dylib.c \
  src/uleb.c src/image.c src/ordinals.c src/fat.c src/trie.c \
  src/lc_kinds.c src/atomic_write.c src/linkedit.c src/grow.c
```

That source list is **a second place deciding what the library contains**, and
it must agree with `CMakeLists.txt` or ctest stays green while the standalone
path breaks. It has needed hand-updating five times already (uleb, image,
ordinals, fat, trie, lc_kinds, atomic_write, linkedit, grow) — which is this
repo's signature bug class living in a test script.

**Four of the six suites already do this correctly**: `chained-fixups.sh`,
`characterize.sh`, `cli_test.sh` and `leaf-tool-crashes.sh` all receive
`$<TARGET_FILE_DIR:...>` from CMake and use the binary CMake built. Make this
one match: take the built binary when a build directory is passed, and compile
from source ONLY as the standalone fallback. If the fallback keeps a source
list, derive it (a glob of `src/*.c`) rather than enumerating, so it cannot
drift.

The 12 helper programs the suites compile at runtime (`ordinal_of`, `makefat`,
`fatcheck`, `has_lc`, `has_bytes`, `mkfixture`, …) are a smaller instance of the
same thing. Converting them to CMake targets is optional here — say what you
did and why. Note none of this is a speed fix: the whole `ctest` run is ~3s.
It is a correctness fix, and the repo owner asked for it on those terms.

It `cd`s to its own directory and compiles `change_dylib.c` itself to stay
standalone-runnable. Both need updating.

- [ ] **Step 3: Confirm the root holds no `.c` or `.h`**

```bash
ls *.c *.h 2>/dev/null && echo "STILL CODE AT ROOT" || echo "root is clean"
```

- [ ] **Step 4: Update `PROVENANCE.md`, `README.md`, `tests/README.md` and `docs/prior-art.md`**

Paths appear in all of them.

---

## What is NOT in this plan

`port`, the `.pkg`, the Sparkle updater and the publish job — packaging work with
its own shape, deserving its own plan. The family conventions skill
(`modernmavericks-conventions`, in the `modernmavericks` plugin) is its spec.

Retiring the `compat/` entry points is also out of scope. `change_dylib` survives
as a compatibility entry point over `macho9 dylib`; removing it is a later
generation's work, and it breaks real installs until `install.sh` moves first.

## Self-Review

**Spec coverage.** `docs/PROPOSAL.md`'s "Shape" lists `image`, `linkedit`, `uleb`,
`bind`, `chained`, `grow`, `ordinals`, `live.h`, `cli/macho9.c`. Built already:
image, uleb, ordinals (+ fat, trie, which the layout did not name). This plan adds
linkedit (Task 2), grow (Task 3), live.h (Task 4). **Still untasked after this
plan: `bind.c` and `chained.c`** — they live inside `patch_macho.c` today and
extracting them needs a fixture whose chained fixups genuinely convert, which
only exists on a modern host. Recorded here deliberately rather than pretended
away.

**Placeholders.** None: every step names its files, its command, or its check.

**Consistency.** Prefixes: `mu_` uleb, `mi_` image, `mo_` ordinals, `mfat_` fat,
`mt_` trie, `ml_` linkedit (Task 2), `mg_` grow (Task 3, keeping the existing
prefix through the move).

**Measurable done.** Root holds no `.c`/`.h`; `grep -c 'ncmds; i++'` across the
repo is materially below 54 and every remaining instance has a recorded reason.
