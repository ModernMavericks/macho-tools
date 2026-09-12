# Relations and Verb Lowering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Declare what points at what as data, derive from it both "which operations carry follow-up work" and "which checks apply to this run", and make the CLI verbs build an `ms_script` and call `me_run` instead of carrying a second grammar.

**Architecture:** One new module, `src/relations.c` (`mrel_`), holding five relation declarations and the derivation over them. The operation table created by the edit-scripts work gains a "disturbs" column, becoming the single place a new operation declares its consequences; `DYLIB_OPS` merges into it. The hand-written `mr_is_rename_only` is proved equivalent-modulo-an-enumerated-list against the derivation, then deleted.

**Tech Stack:** C99, stock 10.9 AppleClang 6.0, CMake + ctest. Hermetic C unit tests in the `trie_test.c`/`linkedit_test.c` idiom; CLI behaviour in `tests/cli_test.sh`.

**Spec:** `docs/superpowers/specs/2026-09-10-relations-and-verb-lowering-design.md`

**Depends on:** `docs/superpowers/plans/2026-09-10-edit-scripts.md` having landed. `MS_TABLE`, `ms_script`, `ms_stmt` and `me_run` all come from it. **Do not start this plan before that one is merged** — every task here consumes one of those four names.

## Global Constraints

- **`tests/EXPECTED` is a characterization reference and is never edited.** `tests/characterize.sh` must keep reproducing `ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792`. It covers emitted bytes: this plan changes *which checks run*, never what gets written, so a moved digest is a real defect.
- **The six `compat/` wrappers' stdout must stay byte-identical.** `tests/known-callers.sh` and `tests/wrapper_test.sh` are the gates. The verbs' `printf` calls do not move in this plan.
- **The tools must never move a byte of file data.** Two reviewed exceptions: `-grow`'s memmove after the load commands, and the export-trie append past `__LINKEDIT`'s end.
- **POSIX `/bin/sh` only** in `compat/*.sh` and test scripts. No `[[`, `local`, `+=`, arrays, `<<<`, `$'...'`, `function`, `source`, `shopt`.
- **Stock 10.9 AppleClang 6.0, warning-free** (`-Wall -Wextra` on new targets).
- **A comment or doc that claims more than the code does is a defect.** This repo means it literally, and this plan deletes a predicate whose replacement must be *proved* rather than asserted.
- **Relations declare referent and liveness only.** No relation declares a check or a repair function; repair code does not move. That is Decision 1, and widening it is out of scope.

---

### Task 1: Declare the relations

Data and one derivation function. Nothing consumes it yet, so this task is safe to land on its own and is where a reviewer can argue with the model before any behaviour depends on it.

**Files:**
- Create: `src/relations.h`, `src/relations.c`
- Create: `tests/relations_test.c`
- Modify: `CMakeLists.txt` — add `src/relations.c` to `machotoolcore`; add the `relations_test` target and test

**Interfaces:**
- Consumes: `mi_image` (`src/image.h`).
- Produces:

```c
/* What is pointed AT. A bitmask so an operation can disturb several. */
enum {
    MREL_ORDINAL    = 1u << 0,  /* the ordinal-carrying LC subsequence */
    MREL_BASE_REL   = 1u << 1,  /* the image base */
    MREL_FILE_OFF   = 1u << 2,  /* __LINKEDIT's blobs */
    MREL_FUNC_START = 1u << 3,  /* LC_FUNCTION_STARTS */
    MREL_HEADER_PAD = 1u << 4   /* sizeofcmds, i.e. the first section's offset */
};

/* Which relations are LIVE in this image -- present and therefore checkable. */
unsigned mrel_live(const mi_image *im);

/* Human name for one bit, for diagnostics. NULL if `bit` names no relation. */
const char *mrel_name(unsigned bit);
```

- [ ] **Step 1: Write the failing test**

`tests/relations_test.c`:

```c
/*
 * tests/relations_test.c — hermetic tests for src/relations.c.
 *
 * Images are built by hand with mi_wrap (same reasoning as linkedit_test), so
 * this is host-agnostic. What is under test is mrel_live: which relations a
 * given image actually has, which is the applicability half of the design.
 *
 * Build: clang -O2 -Wall -Isrc -o /tmp/reltest tests/relations_test.c \
 *   src/relations.c src/image.c && /tmp/reltest
 */
#include "relations.h"
#include "../src/image.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg, ...) do { if (!(cond)) { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); fails++; } } while (0)

static void test_header_pad_is_always_live(void) {
    /* Every Mach-O has a sizeofcmds and a first section, so the header-pad
     * relation is live in every image. If this ever returns 0 the derivation
     * silently stops guarding growth. */
    uint8_t buf[4096]; mi_image im;
    build_minimal_image(buf, sizeof buf, &im);   /* helper below */
    CHECK((mrel_live(&im) & MREL_HEADER_PAD) != 0, "header pad is live in a minimal image");
}

static void test_func_start_liveness_follows_the_load_command(void) {
    uint8_t buf[4096]; mi_image im;
    build_minimal_image(buf, sizeof buf, &im);
    CHECK((mrel_live(&im) & MREL_FUNC_START) == 0,
          "no LC_FUNCTION_STARTS -> the func-start relation is NOT live");
    add_function_starts(buf, &im, 0x10);          /* helper below */
    CHECK((mrel_live(&im) & MREL_FUNC_START) != 0,
          "with LC_FUNCTION_STARTS -> it IS live");
}

static void test_ordinal_liveness_follows_ordinal_carrying_commands(void) {
    uint8_t buf[4096]; mi_image im;
    build_minimal_image(buf, sizeof buf, &im);
    CHECK((mrel_live(&im) & MREL_ORDINAL) == 0,
          "no dylib commands -> the ordinal relation is NOT live");
    add_load_dylib(buf, &im, "/usr/lib/libSystem.B.dylib");   /* helper below */
    CHECK((mrel_live(&im) & MREL_ORDINAL) != 0,
          "one LC_LOAD_DYLIB -> it IS live");
}

static void test_names_round_trip(void) {
    CHECK(strcmp(mrel_name(MREL_ORDINAL), "library ordinal") == 0, "ordinal name");
    CHECK(mrel_name(1u << 20) == NULL, "an unknown bit has no name");
}
```

Write `build_minimal_image`, `add_function_starts` and `add_load_dylib` in this file, following `tests/linkedit_test.c`'s hand-built-image idiom. Do not add a fixture file: a fixture built by the host toolchain would make liveness depend on what the local linker emits, which is the exact failure that made `tests/cli_test.sh`'s build-version fixtures red on the cross runner and green here.

- [ ] **Step 2: Run it and watch it fail to build**

```bash
clang -O2 -Wall -Isrc -o /tmp/reltest tests/relations_test.c src/relations.c src/image.c
```

Expected: failure — `src/relations.h` does not exist.

- [ ] **Step 3: Write `src/relations.h`**

Declare the enum, `mrel_live` and `mrel_name` exactly as in the Interfaces block above. Above the enum, write the five-row table from the spec — relation, referent, and what repairs it today — because the enumerator names alone do not say what is pointed at, and that is the whole content of this module.

State in the header that a relation declares **referent and liveness only**, that no relation declares a check or a repair, and that repair code stays where it is. That is Decision 1 of the spec, and a later reader will otherwise assume the module is half-finished.

- [ ] **Step 4: Write `src/relations.c`**

`mrel_live` walks the load commands once via `mi_each_lc` and sets:

- `MREL_HEADER_PAD` — always; every image has a `sizeofcmds`.
- `MREL_FUNC_START` — when `LC_FUNCTION_STARTS` is present with a non-zero `datasize`.
- `MREL_ORDINAL` — when at least one ordinal-carrying command is present. **Call `mo_is_ordinal_lc` (`src/ordinals.h`); do not re-list the four command kinds.** A second list of which commands carry ordinals is precisely the drift this module exists to prevent, and `mo_is_ordinal_lc` is already the one place that knowledge lives.
- `MREL_FILE_OFF` — when any command from `src/linkedit.h`'s list is present. Build the case labels from that list, the way `ml_bump_lc` does; do not re-list them.
- `MREL_BASE_REL` — when the image has a segment mapping the header, i.e. `mi_image_base` returns 0. Use `mi_image_base`, **not** `mi_text_base`: a dylib links at base 0, and reading that 0 as "no segment maps the header" is the bug that made `mg_plausible` refuse every dylib.

- [ ] **Step 5: Run the tests and watch them pass**

```bash
clang -O2 -Wall -Wextra -Isrc -o /tmp/reltest tests/relations_test.c \
    src/relations.c src/image.c src/ordinals.c && /tmp/reltest
```

Expected: `relations_test: 0 failure(s)`, no warnings.

- [ ] **Step 6: Prove the tests can fail**

Make `mrel_live` return `MREL_HEADER_PAD` unconditionally and confirm the func-start and ordinal tests fail. Revert. Record the mutation and what broke in the task report.

- [ ] **Step 7: Wire into CMake and commit**

```cmake
# Hermetic tests for src/relations.c: hand-built images, no fixture file, so
# liveness never depends on what the host linker emits -- the dependence that
# made cli_test's build-version fixtures pass here and fail on the cross runner.
add_executable(relations_test tests/relations_test.c)
target_compile_options(relations_test PRIVATE -O2 -Wall -Wextra)
target_link_libraries(relations_test PRIVATE machotoolcore)
add_test(NAME relations_test COMMAND relations_test)
```

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
git add src/relations.h src/relations.c tests/relations_test.c CMakeLists.txt
git commit -m "feat: declare the five relations and which are live in an image"
```

---

### Task 2: One operation table, with a disturbs column

`DYLIB_OPS` merges into the edit-script statement table, and every row gains the referents it disturbs.

**Files:**
- Modify: `src/script.h`, `src/script.c` — the table gains columns
- Modify: `cli/machotool.c` — delete `DYLIB_OPS`; the verb parser and `--capabilities` read the merged table
- Modify: `tests/script_test.c`, `tests/cli_test.sh`

**Interfaces:**
- Consumes: `MS_TABLE` and the `MS_*` enumerators (edit-scripts Task 2); `MREL_*` (Task 1).
- Produces:

```c
/* Referents this operation disturbs, as an MREL_* mask. */
unsigned ms_disturbs(int kind, int op);

/* Enumerate the table for --capabilities and for the verb parser.
 * `flag` is the verb spelling ("-replace") or NULL for script-only rows;
 * `modes` is a bitmask of MS_MODE_DYLIB / MS_MODE_RPATH, 0 when not
 * mode-scoped. Returns 0 when `i` is past the end. */
int ms_table_row(int i, const char **kind, const char **op, int *nargs,
                 const char **flag, unsigned *modes, unsigned *disturbs);
```

Note this widens edit-scripts' `ms_table_row`, which had four out-parameters. Update its existing caller in the same commit.

- [ ] **Step 1: Write the failing test**

Append to `tests/script_test.c`:

```c
static void test_disturbs_matches_the_spec_table(void) {
    /* These are the rows the design checked against src/rewrite.h's documented
     * semantics rather than reasoning from the operation's name. Two of them
     * came out the opposite of the design's own first draft, so they are
     * pinned here: a regression would be silent otherwise, because "disturbs
     * nothing" is a plausible-looking answer for every row. */
    CHECK(ms_disturbs(MS_SEGMENT, MS_RENAME) == 0, "segment rename disturbs nothing");
    CHECK(ms_disturbs(MS_SWIFT_ABI, MS_SET) == 0, "swift-abi set disturbs nothing");

    /* reexport is an IN-PLACE promotion of LC_LOAD_DYLIB to LC_REEXPORT_DYLIB
     * (src/rewrite.h:62). Both carry ordinals, so membership, order and length
     * of the subsequence are all unchanged. */
    CHECK(ms_disturbs(MS_DYLIB, MS_REEXPORT) == 0, "dylib reexport disturbs nothing");

    /* append lands LAST (src/rewrite.h:85), taking the highest ordinal, so no
     * existing ordinal moves -- only the command region grows. */
    CHECK(ms_disturbs(MS_DYLIB, MS_APPEND) == MREL_HEADER_PAD,
          "dylib append disturbs the header pad only");

    /* insert lands FIRST and becomes ordinals 1..n (src/rewrite.h:76-87). */
    CHECK(ms_disturbs(MS_DYLIB, MS_INSERT) == (MREL_ORDINAL | MREL_HEADER_PAD),
          "dylib insert disturbs ordinals and the pad");
    CHECK(ms_disturbs(MS_DYLIB, MS_DELETE) == (MREL_ORDINAL | MREL_HEADER_PAD),
          "dylib delete disturbs ordinals and the pad");

    CHECK(ms_disturbs(MS_RPATH, MS_APPEND) == MREL_HEADER_PAD,
          "rpath commands carry no ordinal");
    CHECK(ms_disturbs(MS_LOAD_COMMAND, MS_DELETE) == MREL_HEADER_PAD,
          "load-command delete frees pad and moves no section offset");
    CHECK(ms_disturbs(MS_VERSION_MIN, MS_SET) == MREL_HEADER_PAD,
          "version-min set appends a command");

    CHECK(ms_disturbs(MS_FIXUPS, MS_SET) == (MREL_FILE_OFF | MREL_BASE_REL),
          "fixups set classic rebuilds __LINKEDIT and re-bases");
}

static void test_every_row_declares_its_disturbs(void) {
    /* "Nothing" is a real and common answer, so it must be SPELLED. This
     * catches a row added with a default-zero disturbs column that nobody
     * thought about -- which would silently opt the new operation out of
     * every check. */
    int i = 0; const char *k, *o, *f; int n; unsigned modes, d;
    while (ms_table_row(i, &k, &o, &n, &f, &modes, &d)) {
        CHECK(ms_row_disturbs_declared(i),
              "row %d (%s %s) declares its disturbs explicitly", i, k, o);
        i++;
    }
    CHECK(i > 0, "the table is not empty");
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: `ms_disturbs` and `ms_row_disturbs_declared` do not exist.

- [ ] **Step 3: Widen the table**

Each `MS_TABLE` row gains: `flag` (the verb spelling, `NULL` for script-only), `modes`, `disturbs`, and `declared`. Fill `flag` and `modes` from the `DYLIB_OPS` rows being absorbed, and `disturbs` from Task 2 Step 1's values, which are the spec's checked table.

**Make "declared" impossible to skip.** A row is written through a macro that takes the disturbs mask as a required argument and sets `declared = 1`; `ms_row_disturbs_declared` returns that field. A row added without the macro fails `test_every_row_declares_its_disturbs`. This is the tripwire the spec asks for, in the spirit of `linkedit.h`'s link error — weaker, because C cannot make a missing initializer a link error here, so the test is the enforcement and its comment must say so rather than implying the compiler catches it.

- [ ] **Step 4: Point the verb parser and `--capabilities` at the merged table**

Delete `DYLIB_OPS` and `N_DYLIB_OPS` from `cli/machotool.c`. `cmd_dylib_or_rpath`'s flag matching and the capability printer both walk `ms_table_row`, filtering on `modes`.

The `--capabilities` **output text must not change**: it is a documented interface that wrappers read. Assert that in `tests/cli_test.sh`:

```sh
# The merged table must produce the SAME capabilities text as the two tables
# did. This is a documented interface -- compat/machotool-compat.sh probes it --
# so merging the tables is allowed to change where the text comes from and
# not what it says.
"$MACHOTOOL" --capabilities >"$T/caps_merged.out" 2>&1
grep -q "verb dylib ops=replace,delete,append,insert,reexport" "$T/caps_merged.out" \
    && ok "capabilities: dylib op list unchanged by the merge" \
    || bad "capabilities merge" "dylib ops line changed: $(grep "^verb dylib" "$T/caps_merged.out")"
grep -q "verb rpath ops=replace,delete,append,insert" "$T/caps_merged.out" \
    && ok "capabilities: rpath op list unchanged, still omits reexport" \
    || bad "capabilities merge" "rpath ops line changed: $(grep "^verb rpath" "$T/caps_merged.out")"
```

Before writing those two `grep` patterns, run `machotool --capabilities` on the pre-merge build and copy the real lines. Do not trust the patterns above to match verbatim — they are the shape, and the build is the authority.

- [ ] **Step 5: Run everything and watch it pass**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/known-callers.sh /private/tmp/mm-build/schmonz/macho-tools/native
```

`known-callers` is the one that matters here: it replays the shapes real callers use, and the verb parser was just rewritten underneath them.

- [ ] **Step 6: Commit**

```bash
git add src/script.h src/script.c cli/machotool.c tests/script_test.c tests/cli_test.sh
git commit -m "refactor: one operation table, carrying what each op disturbs"
```

---

### Task 3: Derive which operations carry follow-up work

**Files:**
- Modify: `src/edit.c` — the follow-up decision reads the table
- Modify: `tests/edit_test.c`

**Interfaces:**
- Consumes: `ms_disturbs` (Task 2).
- Produces: `me_followups(const ms_script *s)` returning the union of every statement's disturbs mask.

- [ ] **Step 1: Write the failing test**

```c
static void test_followups_are_the_union_of_the_statements(void) {
    /* A rename plus a swift-abi retag disturbs nothing; adding one dylib
     * delete makes the whole script disturb ordinals. The union, not the
     * last statement and not the first. */
    ms_script s; char err[256] = {0};
    static const char quiet[] = "segment rename __A __B\nswift-abi set legacy\n";
    CHECK(ms_parse(quiet, sizeof quiet - 1, &s, err, sizeof err) == 0, "parses (%s)", err);
    CHECK(me_followups(&s) == 0, "a quiet script disturbs nothing");
    ms_free(&s);

    static const char loud[] =
        "segment rename __A __B\nswift-abi set legacy\ndylib delete /x.dylib\n";
    CHECK(ms_parse(loud, sizeof loud - 1, &s, err, sizeof err) == 0, "parses (%s)", err);
    CHECK((me_followups(&s) & MREL_ORDINAL) != 0,
          "one dylib delete makes the whole script disturb ordinals");
    ms_free(&s);
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: `me_followups` does not exist.

- [ ] **Step 3: Implement it, and delete what it replaces**

`me_followups` is a loop OR-ing `ms_disturbs(st->kind, st->op)` over the statements. Then find the places that currently decide "does this need the ordinal renumbering pass?" by testing operation kinds directly, and route them through it. **Do not leave both**: a hardcoded test surviving beside the derivation is the two-lists-disagreeing defect this plan exists to remove. If you find a site whose condition is *not* expressible as a disturbs mask, stop and report it — that is a gap in the model, not a reason to keep the hardcoded test.

- [ ] **Step 4: Run and commit**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
git add src/edit.c tests/edit_test.c
git commit -m "refactor: derive follow-up work from what an operation disturbs"
```

---

### Task 4: The differential harness

Both predicates run side by side, with the spec's expected-difference list asserted. The old predicate stays in control; nothing changes behaviour in this task. This is the task that earns the right to delete `mr_is_rename_only`.

**Files:**
- Create: `tests/rename_only_differential.c`
- Modify: `CMakeLists.txt`
- Modify: `src/rewrite.c` — expose `mr_is_rename_only` to the test (it is `static` today)

**Interfaces:**
- Consumes: `ms_disturbs`, `me_followups`, `mrel_live`.
- Produces: nothing the shipping code uses. This harness is deleted by Task 6.

- [ ] **Step 1: Write the harness**

For each operation-set shape the suite exercises, compute both answers:

- **old:** `mr_is_rename_only(ops)` — "skip the gate"
- **new:** the gate applies when `mrel_live(image) & disturbed & MREL_FUNC_START`, where `disturbed` is the union over the statements plus `MREL_BASE_REL` if the run grew the header

Then assert against the spec's table. Encode it as data, so the list in the test is the list in the spec:

```c
/* The complete expected-difference list from the design (Decision 3). A
 * difference ON this list is expected and asserted. A difference anywhere
 * else FAILS -- that is the whole point of the harness. */
static const struct { const char *shape; int old_skips; int new_skips; } EXPECTED[] = {
    { "segment rename alone",          1, 1 },   /* unchanged */
    { "swift-abi set alone",           0, 1 },   /* narrowed */
    { "dylib reexport alone",          0, 1 },
    { "load-command delete alone",     0, 1 },
    { "version-min set alone",         0, 1 },
    { "dylib append alone",            0, 1 },
    { "dylib replace alone",           0, 1 },
    { "dylib insert alone",            0, 1 },
    { "dylib delete alone",            0, 1 },
    { "rpath append alone",            0, 1 },
    { "fixups set classic",            0, 0 },   /* unchanged: still gated */
    { "anything that grew the header", 0, 0 },   /* unchanged: still gated */
};
```

- [ ] **Step 2: Run it**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native -R rename_only_differential
```

Expected: passes. **If it does not, stop and report rather than adjusting `EXPECTED` to match what you observed.** The table came from the design; a disagreement means either the model or the design is wrong, and both are decisions above an implementer's pay grade. Adjusting the expectation to fit the output is how a differential test becomes a rubber stamp.

- [ ] **Step 3: Prove the harness can fail**

Add a thirteenth row asserting `{ "segment rename alone", 0, 1 }` — a difference that does not exist. Confirm the harness fails. Remove it. Record this in the task report.

- [ ] **Step 4: Commit**

```bash
git add tests/rename_only_differential.c CMakeLists.txt src/rewrite.c src/rewrite.h
git commit -m "test: prove the derived applicability against mr_is_rename_only"
```

---

### Task 5: Rewrite the scope assertion

`tests/cli_test.sh:2109-2122` asserts `lc -delete uuid` is refused on the implausible fixture, to prove the rename skip is "narrow, not a hole". Task 6 makes that operation skip the gate, so the assertion must move to an operation that genuinely disturbs the relation — **before** Task 6 lands, so the suite is never red.

**Files:**
- Modify: `tests/cli_test.sh:2109-2122`

- [ ] **Step 1: Rewrite the case**

Keep the fixture, the structure, and the failure message `"was NOT refused, so the gate is gone"`. Change the operation to one that rebuilds `__LINKEDIT` and re-bases — `fixups set classic` via `machotool edit`, or the `declassify` verb if that is the spelling at the time.

**Do not delete the case.** Its purpose — proving the skip is narrow rather than a hole — is *more* important after Task 6, not less, because the skip gets much wider.

Correct its premise while you are there. The old label reads *"an operation that CAN move an offset still meets the gate"*, and `lc -delete` does not move a section offset: it frees header pad and `mr_build_lcs` repacks the command region. The test passed because the gate ran, not because the premise held. The new comment should say what the new operation actually does and why that makes the claim true this time.

- [ ] **Step 2: Run and commit**

```bash
sh tests/cli_test.sh /private/tmp/mm-build/schmonz/macho-tools/native 2>&1 | tail -1
git add tests/cli_test.sh
git commit -m "test: gate-is-narrow assertion moves to an op that disturbs the relation"
```

---

### Task 6: Verbs build scripts; `mr_is_rename_only` is deleted

**Files:**
- Modify: `cli/machotool.c` — each `cmd_*` builds an `ms_script` and calls `me_run`
- Modify: `src/rewrite.c` — delete `mr_is_rename_only` and its layout tripwire; the gate's applicability comes from the derivation
- Delete: `tests/rename_only_differential.c`; remove its CMake entries
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Tasks 1–4.

- [ ] **Step 1: Route the verbs**

Each `cmd_*` parses its `argv` into an `ms_script` in memory and calls `me_run`. **Not by generating script text** — round-tripping `argv` through quoting would put a quoting bug on the compat wrappers' path, where today it could only reach `edit`.

**The verbs' `printf` calls do not move.** The wrappers' stdout must stay byte-identical; what changes is how the edit is applied, not how it is described.

- [ ] **Step 2: Run the wrapper gates before anything else**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/known-callers.sh /private/tmp/mm-build/schmonz/macho-tools/native
tests/wrapper_test.sh /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/characterize.sh /private/tmp/mm-build/schmonz/macho-tools/native check
```

All four must be green *before* the predicate comes out. If a wrapper's stdout moved, the routing is wrong and deleting the predicate on top of it would confuse two failures.

- [ ] **Step 3: Delete the predicate**

Remove `mr_is_rename_only` and the `mr_ops_layout_is_still_what_mr_is_rename_only_checks` tripwire that guards it — the tripwire's only purpose was protecting that predicate's conjunction, so it goes with it. `mg_plausible`'s call site takes its applicability from the derivation instead.

Replace the long comment at `src/rewrite.c:865-912` rather than deleting it. It currently explains why a rename-only set skips the gate; the replacement explains the general rule, names the enumerated narrowing, and says plainly what it costs — that `mg_plausible` is defence against rewriter bugs as well as against declared intent, that the case it exists for (`patch_macho`'s chained-fixups conversion, ~94,900 rebases with no self-check of its own) is preserved because that conversion disturbs the relation, and that incidental catches in offset-preserving operations are what is given up.

- [ ] **Step 4: Delete the differential harness**

It has done its job. Leaving it would keep a copy of the deleted predicate alive to compare against, which is the opposite of the point.

- [ ] **Step 5: Run everything**

```bash
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/characterize.sh /private/tmp/mm-build/schmonz/macho-tools/native check
```

The digest must be unmoved: this plan changes which checks run, never what gets written.

- [ ] **Step 6: Commit**

```bash
git add cli/machotool.c src/rewrite.c src/rewrite.h CMakeLists.txt
git rm tests/rename_only_differential.c
git commit -m "refactor: verbs lower to scripts; applicability is derived, not hand-written"
```

---

## Self-review

**Spec coverage.** "This mechanism already exists here, twice" → Task 1 Step 4 reuses `mo_is_ordinal_lc` and `linkedit.h`'s list rather than re-listing. "Decision 1: derivation only" → Task 1. "Decision 2: what each operation disturbs" → Task 2, with the spec's checked values pinned as assertions. "Decision 3" → Tasks 4, 5, 6 in that order: prove, move the assertion, then delete. "Decision 4: verbs build scripts" → Tasks 2 and 6. "The objection this will draw" → Task 6 Step 3's replacement comment. "Testing" → the differential (Task 4), the declared-disturbs tripwire (Task 2), `known-callers`/`wrapper_test` (Task 6 Step 2), `characterize` (Task 6 Step 5).

**Placeholders.** None. Every code step carries its code; the three mutation steps name what to break and what must fail.

**Type consistency.** `mrel_live`, `mrel_name`, `MREL_*`, `ms_disturbs`, `ms_table_row`, `ms_row_disturbs_declared`, `me_followups` are spelled identically everywhere they appear. Task 2 widens edit-scripts' `ms_table_row` from four out-parameters to seven and says so, with its existing caller updated in the same commit.

**One ordering constraint worth restating.** Task 5 must land before Task 6. `cli_test.sh:2109`'s assertion fails the moment the derivation takes over, and its failure message reads `"the gate is gone"` — which, arriving in the same commit that deletes the predicate, would look exactly like the regression it is not.

**One dependency this plan cannot satisfy itself.** Every task consumes `MS_TABLE`, `ms_script` or `me_run` from `plans/2026-09-10-edit-scripts.md`. Starting before that plan merges is not slow, it is impossible.
