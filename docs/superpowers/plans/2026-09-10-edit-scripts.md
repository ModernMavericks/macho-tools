# Edit Scripts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `macho9 edit FILE SCRIPT`, which applies a flat sequence of statements to one in-memory image, verifies it, and writes it once.

**Architecture:** Two new modules. `src/script.c` (`ms_`) turns script text into a parsed `ms_script` — a tokenizer with shell word rules, a data-driven statement table, and two directives. `src/edit.c` (`me_`) walks that script against one in-memory image, applying each statement in order through the existing `mr_`/`mg_` operations, verifying once at the end, and writing once. The CLI gains one verb. Nothing is written unless every statement succeeded.

**Tech Stack:** C99, stock 10.9 AppleClang 6.0, CMake + ctest. Hermetic C unit tests in the `trie_test.c`/`linkedit_test.c` idiom; CLI behaviour in `tests/cli_test.sh`.

**Spec:** `docs/superpowers/specs/2026-09-10-edit-scripts-design.md`

## Global Constraints

- **`tests/EXPECTED` is a characterization reference and is never edited.** `tests/characterize.sh` must keep reproducing `ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792`.
- **The six `compat/` wrappers' stdout must stay byte-identical** to the C tools they replaced. `tests/known-callers.sh` and `tests/wrapper_test.sh` are the gates. (Their **exit codes** do change in Task 0 — that is the one sanctioned exception, and it is the whole of Task 0.)
- **The tools must never move a byte of file data.** Two reviewed exceptions: `-grow`'s memmove after the load commands, and the export-trie append past `__LINKEDIT`'s end.
- **POSIX `/bin/sh` only** in `compat/*.sh` and test scripts. No `[[`, `local`, `+=`, arrays, `<<<`, `$'...'`, `function`, `source`, `shopt`.
- **Stock 10.9 AppleClang 6.0, warning-free** (`-Wall -Wextra` on new targets).
- **A comment or doc that claims more than the code does is a defect.** This repo means it literally.
- **No hardcoded operation cap for `edit`.** `MR_MAX_OPS` (32) and `MR_MAX_STRIP` (16) are CLI-side array sizes, not format limits; `edit` sizes its storage from the parsed script. `MO_MAX_DYLIBS` (253) stays, because it is `MAX_LIBRARY_ORDINAL`, a real format limit.
- **Verification after the last statement is mandatory, with no escape hatch.** A `MACHO_NO_VERIFY`-shaped hole was deliberately removed from a shipped wrapper; do not reintroduce one at a higher level.

## Spec defect resolved before execution

The spec contradicts itself on exit codes, and this plan resolves it rather than leaving it for an implementer to guess:

- **"Execution model"** says *"`edit` uses the verbs' existing exit codes, unchanged: `0` on success, `2` where it examined the file and declined on purpose … `1` for an operational failure."*
- **"Exit codes — corrected while we still can"** says *"The scheme becomes `0` ok, `1` refused, `2` error."*

**The corrected scheme wins**, and the "Execution model" paragraph is stale. Three reasons: it is the later and deliberate decision, the repo owner approved it explicitly, and the spec's own worked example agrees with it — the refusal example prints `refused at statement 6 of 9` and then `$? = 1`. Task 0 makes the tree match, and Task 4's `edit` is built on the corrected scheme from its first commit.

---

### Task 0: Correct the exit-code scheme

The shipped scheme is `0` ok, `1` failed, `2` refused. It is backwards from what shell authors know: `grep` uses `2` for *error*, `diff` and `cmp` use `2` for *trouble*. Both reserve the highest code for "the tool could not do its job"; this repo reserves it for "the tool did its job and declined". Nothing outside this repo has ever run the compat wrappers, so there is no compatibility to break — and after `edit` ships there will be.

**New scheme: `0` ok, `1` refused, `2` error.**

**Files:**
- Modify: `src/rewrite.h` — `MR_REFUSED`
- Modify: `cli/macho9.c` — `EX_REFUSED`, the `mr_refused_is_ex_refused` tripwire, the `--capabilities` `exitcodes` line, and every comment stating the old values
- Modify: `tests/cli_test.sh` — the exit-code assertions
- Modify: `tests/wrapper_test.sh`, `tests/known-callers.sh` — any wrapper exit-code assertion
- Modify: `compat/*.sh` — any wrapper that maps or tests an exit code
- Modify: `README.md`, `compat/README.md` — any documented exit code

**Interfaces:**
- Consumes: nothing.
- Produces: `MR_REFUSED == 1` (`src/rewrite.h`), `EX_REFUSED == 1` and a new `EX_FAIL == 2` (`cli/macho9.c`). Every later task depends on these values.

**Do not confuse `MR_ERROR` with an exit code.** It is `#define MR_ERROR (-1)`, private to `src/rewrite.c:578`, and it is `mr_process_fat`'s **per-slice status** — "this slice is a 64-bit Mach-O whose edit failed", as against `MR_SKIP` for a slice that is not one. It never reaches a caller as a process exit status and this task must not touch it.

- [ ] **Step 1: Find every site that names an exit code**

```bash
grep -rn "MR_REFUSED\|EX_REFUSED\|MR_ERROR\|exitcodes\|refused=2\|failed=1" \
    src/ cli/ compat/ tests/ README.md docs/ | tee /tmp/exitsites.txt
wc -l /tmp/exitsites.txt
```

Read every line before changing any. The compile-time tripwire at `cli/macho9.c:122` (`typedef char mr_refused_is_ex_refused[(MR_REFUSED == EX_REFUSED) ? 1 : -1];`) exists so these two cannot drift — it must still hold after the change, with both equal to 1.

- [ ] **Step 2: Write the failing test**

In `tests/cli_test.sh`, next to the existing `--capabilities` assertions:

```sh
# The corrected scheme: 0 ok, 1 refused, 2 error. Backwards from what shipped,
# and deliberately so -- diff/grep/cmp all reserve 2 for "something went
# wrong" and 1 for "a normal, expected, non-success answer". Nothing outside
# this repo has run the compat wrappers, so this is the last chance to fix it.
"$MACHO9" --capabilities >"$T/caps.out" 2>&1
grep -q "exitcodes ok=0 refused=1 failed=2" "$T/caps.out" \
    && ok "capabilities: the corrected exit-code scheme" \
    || bad "capabilities exitcodes" "expected 'ok=0 refused=1 failed=2', got: $(grep exitcodes "$T/caps.out")"
```

- [ ] **Step 3: Run it and watch it fail**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/cli_test.sh /private/tmp/mm-build/schmonz/macho-tools/native 2>&1 | grep "capabilities exitcodes"
```

Expected: `FAIL capabilities exitcodes: expected 'ok=0 refused=1 failed=2', got: exitcodes ok=0 refused=2 failed=1`

- [ ] **Step 4: Change the two constants and the capabilities line**

`src/rewrite.h`: `#define MR_REFUSED 1`. `cli/macho9.c`: `#define EX_REFUSED 1`, and add `#define EX_FAIL 2` beside it. Do NOT add an `MR_ERROR` to `rewrite.h` -- that name is already taken, privately, for something else (see above). The `--capabilities` line becomes:

```c
printf("exitcodes ok=0 refused=%d failed=%d\n", EX_REFUSED, EX_FAIL);
```

Introduce `EX_FAIL` (2) rather than a bare literal, so the two codes are named the same way and a future change touches one place each.

- [ ] **Step 5: Update every comment that states the old values**

`cli/macho9.c:194-220` and `src/rewrite.h:203-227` both narrate the old scheme in prose. A comment that claims more than the code does is a defect here, so these are not optional. Each should say what the code now is **and why** — the `diff`/`grep`/`cmp` precedent, and that this does *not* match binutils (which returns 0 or 1 and has no notion of a deliberate refusal).

- [ ] **Step 6: Run the full suite and fix every assertion the change moved**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
```

Every assertion that changes must change because the *meaning* moved, not to make a suite green. For each one, confirm from the surrounding comment which of "refused" or "error" the site meant, then write the new value. If a site is ambiguous about which it meant, that ambiguity is a finding — say so in the report rather than picking one.

- [ ] **Step 7: Verify the characterization digest did not move**

```bash
sh tests/characterize.sh /private/tmp/mm-build/schmonz/macho-tools/native check
```

Expected: `characterize: OK (ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792)`. The digest covers emitted bytes, not exit codes, so it must be unchanged. If it moved, something rewrote a file differently and that is a real defect.

- [ ] **Step 8: Commit**

```bash
git add src/rewrite.h cli/macho9.c tests/cli_test.sh tests/wrapper_test.sh \
        tests/known-callers.sh compat/ README.md
git commit -m "fix!: exit codes become 0 ok, 1 refused, 2 error"
```

---

### Task 1: The tokenizer

Shell word rules, and they must agree exactly with what `compat/translate.sh`'s `mt_quote` emits — the spec requires that the generator and the parser cannot drift.

`mt_quote`'s rule, read from the source: an empty string becomes `''`; a string containing any character outside `[A-Za-z0-9_@%+=:,./-]` is wrapped in single quotes with each `'` replaced by `'\''`; anything else is emitted bare. So `#`, space, `$`, backtick, `;` and `*` are all outside the safe set and always arrive quoted — which is what lets an unquoted `#` mean "comment" without ever eating a real path.

**Files:**
- Create: `src/script.h`, `src/script.c`
- Create: `tests/script_test.c`
- Modify: `CMakeLists.txt` — add `src/script.c` to `macho9core`, add the `script_test` target and test

**Interfaces:**
- Consumes: nothing.
- Produces:

```c
/* Splits ONE line into fields using shell word rules. Writes field pointers
 * into `argv` (at most `max`), NUL-terminating each in place inside `line`,
 * which is modified. Returns the field count, or -1 on a quoting error with
 * `err` set. A comment-only or blank line returns 0. */
int ms_split(char *line, char **argv, int max, char *err, size_t errsz);
```

- [ ] **Step 1: Write the failing test**

`tests/script_test.c`:

```c
/*
 * tests/script_test.c — hermetic tests for src/script.c.
 *
 * Ground truth is hand-written script text and the field vector it must
 * produce, so this is host-agnostic: no fixture file, no toolchain
 * dependence. The quoting cases are the ones mt_quote (compat/translate.sh)
 * actually emits, because the spec requires the generator and this parser
 * cannot drift.
 *
 * Build: clang -O2 -Wall -Isrc -o /tmp/scripttest tests/script_test.c \
 *   src/script.c && /tmp/scripttest
 */
#include "script.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg, ...) do { if (!(cond)) { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); fails++; } } while (0)

static void split_is(const char *in, int want_n, const char *w0,
                     const char *w1, const char *w2) {
    char buf[512]; char *av[8]; char err[128] = {0};
    snprintf(buf, sizeof buf, "%s", in);
    int n = ms_split(buf, av, 8, err, sizeof err);
    CHECK(n == want_n, "[%s] -> %d fields, wanted %d (err: %s)", in, n, want_n, err);
    if (n != want_n) return;
    if (w0) CHECK(strcmp(av[0], w0) == 0, "[%s] field0 = '%s', wanted '%s'", in, av[0], w0);
    if (w1) CHECK(strcmp(av[1], w1) == 0, "[%s] field1 = '%s', wanted '%s'", in, av[1], w1);
    if (w2) CHECK(strcmp(av[2], w2) == 0, "[%s] field2 = '%s', wanted '%s'", in, av[2], w2);
}

static void test_plain_fields(void) {
    split_is("dylib replace A B", 4, "dylib", "replace", "A");
    split_is("  load-command   delete   uuid  ", 3, "load-command", "delete", "uuid");
}

static void test_blank_and_comment(void) {
    split_is("", 0, NULL, NULL, NULL);
    split_is("   ", 0, NULL, NULL, NULL);
    split_is("# a whole-line comment", 0, NULL, NULL, NULL);
    split_is("   # indented comment", 0, NULL, NULL, NULL);
    split_is("dylib append /x # trailing comment", 3, "dylib", "append", "/x");
}

/* mt_quote wraps anything outside [A-Za-z0-9_@%+=:,./-] in single quotes,
 * so every one of these is a shape the generator really emits. */
static void test_mt_quote_shapes(void) {
    split_is("dylib append '/a path/with spaces.dylib'", 3,
             "dylib", "append", "/a path/with spaces.dylib");
    split_is("dylib append '/has#hash'", 3, "dylib", "append", "/has#hash");
    split_is("dylib append '/has$(cmd)'", 3, "dylib", "append", "/has$(cmd)");
    split_is("dylib append '/has;semi'", 3, "dylib", "append", "/has;semi");
    split_is("dylib append ''", 3, "dylib", "append", "");
    /* mt_quote's '\'' idiom for an embedded single quote */
    split_is("dylib append '/it'\\''s'", 3, "dylib", "append", "/it's");
}

static void test_double_quotes_and_backslash(void) {
    split_is("dylib append \"/a path\"", 3, "dylib", "append", "/a path");
    split_is("dylib append \"/esc\\\"q\"", 3, "dylib", "append", "/esc\"q");
    split_is("dylib append /lead\\ space", 3, "dylib", "append", "/lead space");
}

static void test_unterminated_quote_is_an_error(void) {
    char buf[64]; char *av[8]; char err[128] = {0};
    snprintf(buf, sizeof buf, "dylib append '/unterminated");
    int n = ms_split(buf, av, 8, err, sizeof err);
    CHECK(n == -1, "an unterminated quote is an error (got %d)", n);
    CHECK(err[0] != 0, "and says so");
}

static void test_too_many_fields_is_an_error(void) {
    char buf[64]; char *av[2]; char err[128] = {0};
    snprintf(buf, sizeof buf, "a b c d");
    int n = ms_split(buf, av, 2, err, sizeof err);
    CHECK(n == -1, "overflowing the field vector is an error, not truncation (got %d)", n);
}

int main(void) {
    test_plain_fields();
    test_blank_and_comment();
    test_mt_quote_shapes();
    test_double_quotes_and_backslash();
    test_unterminated_quote_is_an_error();
    test_too_many_fields_is_an_error();
    printf("script_test: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Run it and watch it fail to build**

```bash
clang -O2 -Wall -Isrc -o /tmp/scripttest tests/script_test.c src/script.c
```

Expected: failure — `src/script.h` and `src/script.c` do not exist.

- [ ] **Step 3: Write `src/script.h`**

```c
#ifndef MACHO9_SCRIPT_H
#define MACHO9_SCRIPT_H

#include <stddef.h>

/* Splits ONE line into fields using shell word rules, in place.
 *
 * The rules are chosen to agree exactly with compat/translate.sh's mt_quote,
 * which is the generator for these scripts: it emits a bare word only when
 * every character is in [A-Za-z0-9_@%+=:,./-], and otherwise single-quotes
 * the whole word with each ' written as '\''. So an unquoted '#' can mean
 * "comment" without ever eating a real path -- mt_quote never emits one.
 *
 *   - fields separate on unquoted space or tab
 *   - '...'  literal; no escapes inside (mt_quote's '\'' works because the
 *            quote closes, \' is a literal quote outside quotes, and the
 *            next ' reopens)
 *   - "..."  backslash escapes \" \\ \$ \` ; any other backslash is literal
 *   - \x     outside quotes: literal x
 *   - #      unquoted, at the start of a field: comment to end of line
 *
 * `line` is modified: each returned field is NUL-terminated in place.
 * Returns the field count, 0 for a blank or comment-only line, or -1 with
 * `err` set on a quoting error or on more than `max` fields. Overflow is an
 * error rather than truncation: silently dropping an operand is the
 * silent-success class this toolkit exists to eliminate. */
int ms_split(char *line, char **argv, int max, char *err, size_t errsz);

#endif
```

- [ ] **Step 4: Write `src/script.c`'s tokenizer**

```c
#include "script.h"
#include <stdio.h>
#include <string.h>

static int ms_err(char *err, size_t errsz, const char *msg) {
    if (err && errsz) snprintf(err, errsz, "%s", msg);
    return -1;
}

int ms_split(char *line, char **argv, int max, char *err, size_t errsz) {
    char *r = line;              /* read cursor */
    int   n = 0;
    for (;;) {
        while (*r == ' ' || *r == '\t') r++;
        if (*r == '\0' || *r == '#') break;   /* end of line, or a comment */
        if (n >= max) return ms_err(err, errsz, "too many fields on one line");

        char *w = r;             /* write cursor: always <= r, so in place */
        argv[n++] = w;
        while (*r && *r != ' ' && *r != '\t') {
            if (*r == '\'') {
                r++;
                while (*r != '\'') {
                    if (*r == '\0') return ms_err(err, errsz, "unterminated '");
                    *w++ = *r++;
                }
                r++;
            } else if (*r == '"') {
                r++;
                while (*r != '"') {
                    if (*r == '\0') return ms_err(err, errsz, "unterminated \"");
                    if (*r == '\\' && (r[1] == '"' || r[1] == '\\' ||
                                       r[1] == '$' || r[1] == '`')) r++;
                    *w++ = *r++;
                }
                r++;
            } else if (*r == '\\') {
                if (r[1] == '\0') return ms_err(err, errsz, "trailing backslash");
                r++;
                *w++ = *r++;
            } else {
                *w++ = *r++;
            }
        }
        /* r now points at the separator or the NUL. Capture it before the
         * terminator overwrites it -- w can equal r when nothing was
         * unquoted, and then *w = '\0' would clobber what we are about to
         * read. */
        char sep = *r;
        *w = '\0';
        if (sep == '\0') break;
        r++;
    }
    return n;
}
```

Note the `sep` capture in the final lines. When a field contains no quoting, `w` and `r` advance together and end up equal; writing the terminator before reading the separator would lose the rest of the line. Write the comment, because the next reader will want to delete those two lines.

- [ ] **Step 5: Run the test and watch it pass**

```bash
clang -O2 -Wall -Wextra -Isrc -o /tmp/scripttest tests/script_test.c src/script.c && /tmp/scripttest
```

Expected: `script_test: 0 failure(s)`, and no compiler warnings.

- [ ] **Step 6: Prove the test can fail**

Temporarily change `if (*r == '\0' || *r == '#') break;` to drop the `#` case, rebuild, and confirm the comment tests fail. Revert. A test suite that passes against a broken implementation is not a test suite; record in the task report which mutation you used and what it broke.

- [ ] **Step 7: Wire it into CMake**

In `CMakeLists.txt`, add `src/script.c` to `macho9core`'s sources, then, beside the other hermetic C tests:

```cmake
# Hermetic tests for src/script.c's tokenizer: hand-written script text and
# the field vector it must produce, so no fixture file is needed -- same
# reasoning as trie_test. The quoting cases are exactly the shapes
# compat/translate.sh's mt_quote emits, because the spec requires the
# generator and the parser cannot drift.
add_executable(script_test tests/script_test.c)
target_compile_options(script_test PRIVATE -O2 -Wall -Wextra)
target_link_libraries(script_test PRIVATE macho9core)
add_test(NAME script_test COMMAND script_test)
```

- [ ] **Step 8: Run the whole suite and commit**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
git add src/script.h src/script.c tests/script_test.c CMakeLists.txt
git commit -m "feat: a shell-word tokenizer for edit scripts"
```

---

### Task 2: The statement table, the parser, and the directives

The table is **data**, not a chain of `strcmp`s, because the spec requires `--capabilities` to be generated from it rather than maintained beside it — this repo has already had one defect from two such lists disagreeing.

**Files:**
- Modify: `src/script.h`, `src/script.c`
- Modify: `tests/script_test.c`

**Interfaces:**
- Consumes: `ms_split` from Task 1.
- Produces:

```c
enum { MS_LOAD_COMMAND, MS_SEGMENT, MS_VERSION_MIN, MS_SWIFT_ABI,
       MS_FIXUPS, MS_DYLIB, MS_RPATH };
enum { MS_DELETE, MS_RENAME, MS_SET, MS_REPLACE, MS_APPEND,
       MS_INSERT, MS_REEXPORT };

typedef struct { int kind, op; const char *a, *b; int line; } ms_stmt;

typedef struct {
    ms_stmt *stmts;
    int      n;
    int      allow_grow;
    int      fatal_warnings;
    char    *text;      /* owns every operand's storage */
} ms_script;

int  ms_parse(const char *buf, size_t len, ms_script *out, char *err, size_t errsz);
void ms_free(ms_script *s);
const char *ms_kind_name(int kind);
const char *ms_op_name(int op);
int  ms_table_row(int i, const char **kind, const char **op, int *nargs);
```

`ms_table_row` enumerates the statement table for `--capabilities`; it returns 0 when `i` is past the end.

- [ ] **Step 1: Write the failing tests**

Append to `tests/script_test.c`:

```c
static void test_parses_the_production_script(void) {
    static const char src[] =
        "# Claude Code -> 10.9\n"
        "fixups        set      classic\n"
        "version-min   set      10.9\n"
        "load-command  delete   uuid\n"
        "dylib         replace  /usr/lib/libSystem.B.dylib  @loader_path/../S.dylib\n";
    ms_script s; char err[256] = {0};
    int r = ms_parse(src, sizeof src - 1, &s, err, sizeof err);
    CHECK(r == 0, "the production script parses (got %d, err: %s)", r, err);
    if (r != 0) return;
    CHECK(s.n == 4, "four statements (got %d)", s.n);
    CHECK(s.stmts[0].kind == MS_FIXUPS && s.stmts[0].op == MS_SET, "stmt0 is fixups set");
    CHECK(s.stmts[3].kind == MS_DYLIB && s.stmts[3].op == MS_REPLACE, "stmt3 is dylib replace");
    CHECK(strcmp(s.stmts[3].b, "@loader_path/../S.dylib") == 0, "stmt3 operand b");
    CHECK(s.stmts[3].line == 5, "stmt3 remembers its source line (got %d)", s.stmts[3].line);
    ms_free(&s);
}

static void test_directives_set_flags_and_are_not_statements(void) {
    static const char src[] = "allow-grow\nfatal-warnings\nload-command delete uuid\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == 0, "parses (%s)", err);
    CHECK(s.allow_grow == 1, "allow-grow set the flag");
    CHECK(s.fatal_warnings == 1, "fatal-warnings set the flag");
    CHECK(s.n == 1, "directives are not statements (got n=%d)", s.n);
    ms_free(&s);
}

static void test_a_directive_after_an_operation_is_an_error(void) {
    static const char src[] = "load-command delete uuid\nallow-grow\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1,
          "a directive after an operation is refused");
    CHECK(strstr(err, "line 2") != NULL, "and names the line (got: %s)", err);
}

static void test_unknown_statement_and_wrong_arity(void) {
    ms_script s; char err[256] = {0};
    static const char bad1[] = "frobnicate all\n";
    CHECK(ms_parse(bad1, sizeof bad1 - 1, &s, err, sizeof err) == -1, "unknown kind refused");
    static const char bad2[] = "segment rename __ONLYONE\n";
    CHECK(ms_parse(bad2, sizeof bad2 - 1, &s, err, sizeof err) == -1, "wrong arity refused");
    static const char bad3[] = "load-command delete not-a-kind\n";
    CHECK(ms_parse(bad3, sizeof bad3 - 1, &s, err, sizeof err) == -1, "unknown KIND refused");
}

static void test_no_operation_cap(void) {
    /* The old CLI capped at MR_MAX_OPS (32). The spec is explicit that edit
     * sizes from the parsed script, because the dominant real workload --
     * repointing every framework in frameworks.json at a stub -- is 32
     * dylib replaces plus everything else. */
    char big[64 * 1024]; size_t len = 0;
    for (int i = 0; i < 200; i++)
        len += (size_t)snprintf(big + len, sizeof big - len,
                                "dylib replace /a/%d.dylib /b/%d.dylib\n", i, i);
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(big, len, &s, err, sizeof err) == 0, "200 statements parse (%s)", err);
    CHECK(s.n == 200, "all 200 kept (got %d)", s.n);
    ms_free(&s);
}
```

Add each new function to `main()`.

- [ ] **Step 2: Run and watch it fail**

```bash
clang -O2 -Wall -Isrc -o /tmp/scripttest tests/script_test.c src/script.c
```

Expected: failure — `ms_parse`, `ms_script` and the enums do not exist.

- [ ] **Step 3: Write the statement table**

In `src/script.c`. One row per statement, exactly the sixteen the spec lists:

```c
/* The statement table. Data, not a strcmp chain, because --capabilities is
 * GENERATED from these rows rather than maintained beside them -- this repo
 * has already had a defect from two such lists disagreeing (the DYLIB_OPS
 * table against the --capabilities text). Adding a statement here is the
 * whole of adding a statement. */
static const struct { const char *kind; int k; const char *op; int o; int nargs; }
MS_TABLE[] = {
    { "load-command", MS_LOAD_COMMAND, "delete",   MS_DELETE,   1 },
    { "segment",      MS_SEGMENT,      "rename",   MS_RENAME,   2 },
    { "version-min",  MS_VERSION_MIN,  "set",      MS_SET,      1 },
    { "swift-abi",    MS_SWIFT_ABI,    "set",      MS_SET,      1 },
    { "fixups",       MS_FIXUPS,       "set",      MS_SET,      1 },
    { "dylib",        MS_DYLIB,        "replace",  MS_REPLACE,  2 },
    { "dylib",        MS_DYLIB,        "append",   MS_APPEND,   1 },
    { "dylib",        MS_DYLIB,        "insert",   MS_INSERT,   1 },
    { "dylib",        MS_DYLIB,        "delete",   MS_DELETE,   1 },
    { "dylib",        MS_DYLIB,        "reexport", MS_REEXPORT, 1 },
    { "rpath",        MS_RPATH,        "replace",  MS_REPLACE,  2 },
    { "rpath",        MS_RPATH,        "delete",   MS_DELETE,   1 },
    { "rpath",        MS_RPATH,        "append",   MS_APPEND,   1 },
    { "rpath",        MS_RPATH,        "insert",   MS_INSERT,   1 },
};
static const int MS_TABLE_N = (int)(sizeof MS_TABLE / sizeof MS_TABLE[0]);

int ms_table_row(int i, const char **kind, const char **op, int *nargs) {
    if (i < 0 || i >= MS_TABLE_N) return 0;
    *kind = MS_TABLE[i].kind; *op = MS_TABLE[i].op; *nargs = MS_TABLE[i].nargs;
    return 1;
}
```

- [ ] **Step 4: Write `ms_parse`**

Two passes over a private copy of the text. The first counts non-blank, non-comment lines so the statement array is sized from the script — no cap. The second fills it.

Rules the parser enforces, each with its own error message naming the 1-based line:
- a line whose first field is `allow-grow` or `fatal-warnings` is a directive, takes no operands, and **must precede every operation** (the spec: directives describe the run, operations are steps in it);
- otherwise the first two fields must match a `MS_TABLE` row, and the remaining field count must equal that row's `nargs`;
- `load-command delete KIND` validates `KIND` against `LC_STRIP_KINDS` in `src/lc_kinds.c` — the same five names `lc_kind_name` knows — so an unknown kind is refused at parse time, before the file is opened;
- `version-min set` accepts only `10.9`, `swift-abi set` only `legacy`, `fixups set` only `classic`. Anything else is a refusal naming what was accepted. A wider vocabulary is a later decision, and refusing now is what keeps it one.

`ms_script.text` owns a NUL-separated copy of the script; every `ms_stmt.a`/`.b` points into it, so `ms_free` frees exactly two allocations.

- [ ] **Step 5: Run the tests and watch them pass**

```bash
clang -O2 -Wall -Wextra -Isrc -o /tmp/scripttest tests/script_test.c src/script.c && /tmp/scripttest
```

Expected: `script_test: 0 failure(s)`, no warnings.

- [ ] **Step 6: Generate `--capabilities` from the table**

In `cli/macho9.c`, replace the hand-maintained statement vocabulary with a loop over `ms_table_row`. Then add the assertion that proves it is generated rather than copied, in `tests/cli_test.sh`:

```sh
# Every row of the statement table must appear in --capabilities, and
# --capabilities must advertise nothing the table lacks. The point of
# generating one from the other is that this can never go stale; this
# assertion is what makes that claim testable rather than aspirational.
"$MACHO9" --capabilities >"$T/caps2.out" 2>&1
grep -q "statement dylib replace 2" "$T/caps2.out" \
    && ok "capabilities: statement table is advertised" \
    || bad "capabilities statements" "no 'statement dylib replace 2' line: $(cat "$T/caps2.out")"
```

- [ ] **Step 7: Run the whole suite and commit**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/characterize.sh /private/tmp/mm-build/schmonz/macho-tools/native check
git add src/script.h src/script.c tests/script_test.c tests/cli_test.sh cli/macho9.c
git commit -m "feat: the edit-script statement table, parser and directives"
```

---

### Task 3: The execution model

Read the image once, apply each statement in order against the in-memory buffer, verify, write once. Any statement failing means nothing is written — that is the property the whole design exists to buy.

**Files:**
- Create: `src/edit.h`, `src/edit.c`
- Modify: `CMakeLists.txt` — add `src/edit.c` to `macho9core`
- Modify: `tests/cli_test.sh` (the assertions land in Task 4, once the verb exists; this task's own test is the C-level one below)
- Create: `tests/edit_test.c`

**Interfaces:**
- Consumes: `ms_script`, `ms_stmt` from Task 2; `mr_apply_file`'s operation set and `mg_plausible` from the existing modules.
- Produces:

```c
typedef struct {
    int   verbose;        /* log each statement and its follow-ups to stderr */
    int   dry_run;        /* apply and verify, but do not write */
    FILE *log;            /* where the report goes; stderr in the CLI */
} me_opts;

/* Applies `s` to `path`, verifies, and writes once -- to `out` if non-NULL,
 * else back to `path`. Returns 0 on success, MR_REFUSED (1) when a statement
 * or the verify declined on purpose, or 2 for an operational failure (a
 * syscall, a malloc). On any non-zero return NOTHING has been written.
 *
 * NOT MR_ERROR: that is (-1), private to src/rewrite.c, and it is
 * mr_process_fat's per-slice status, not an exit code. */
int me_run(const char *path, const char *out, const ms_script *s,
           const me_opts *o);
```

- [ ] **Step 1: Write the failing test**

`tests/edit_test.c`, in the `linkedit_test.c` idiom — build a small image by hand with `mi_wrap` so no fixture file is needed:

```c
/*
 * tests/edit_test.c — hermetic tests for src/edit.c's me_run.
 *
 * The image is built here by hand (same reasoning as linkedit_test), so this
 * is host-agnostic. What is under test is the EXECUTION MODEL, not the
 * individual operations: statements apply in order, a failure part-way
 * writes nothing, and a dry run writes nothing while still verifying.
 */
```

Three tests, and the middle one is the reason this module exists:

1. `test_statements_apply_in_order` — a script that deletes `uuid` then renames a segment leaves both effects, and the log (into a `tmpfile()`) names them in script order.
2. `test_a_failure_part_way_writes_nothing` — a two-statement script whose second statement refuses. Assert `me_run` returns `MR_REFUSED` **and** that the file on disk is byte-identical to before, compared with a hash taken up front. This is the whole point of the design; it gets the most explicit assertion in the suite.
3. `test_dry_run_writes_nothing_but_still_verifies` — a script that would succeed, with `dry_run = 1`. Assert return 0 and the file unchanged.

- [ ] **Step 2: Run and watch it fail**

```bash
clang -O2 -Wall -Isrc -o /tmp/edittest tests/edit_test.c src/*.c && /tmp/edittest
```

Expected: failure — `src/edit.h` does not exist.

- [ ] **Step 3: Write `src/edit.c`'s lowering**

One `switch` on `ms_stmt.kind`, each arm building the single-operation `mr_ops` (or calling the dedicated `mg_`/`mseg_`/`mswift_` entry point) that the statement means. `ms_script.allow_grow` becomes `mr_ops.allow_grow`; `ms_script.fatal_warnings` becomes `mr_ops.fatal_unmatched`.

Sequential, not collapsed into one operation set, for the reason the spec gives: `fixups set classic` cannot batch with anything, because later statements must see the lowered image. Write that reason at the loop, because "why not batch these?" is the first question a reader will have.

- [ ] **Step 4: Write the write-once path**

Apply everything to the in-memory buffer; call `mg_plausible` once after the last statement; only then write. Writing is atomic — to a temp file in the destination's directory, then `rename(2)` — so a crash mid-write cannot leave a half-converted binary, which is the failure mode the three-tool `install.sh` sequence had.

**Verification is mandatory and has no flag.** Do not add a parameter to skip it. A `MACHO_NO_VERIFY` escape was deliberately removed from a shipped wrapper during the compat retirement, on the grounds that a disabled-safety-check-shaped hole in a shipped artifact is worse than the false positives it hides.

- [ ] **Step 5: Run the tests and watch them pass**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native -R edit_test
```

Expected: `edit_test` passes.

- [ ] **Step 6: Prove the nothing-is-written test can fail**

Temporarily move the write above the statement loop, rebuild, and confirm `test_a_failure_part_way_writes_nothing` fails. Revert. Record the mutation in the task report.

- [ ] **Step 7: Commit**

```bash
git add src/edit.h src/edit.c tests/edit_test.c CMakeLists.txt
git commit -m "feat: apply an edit script to one image, verify, write once"
```

---

### Task 4: The `edit` verb

**Files:**
- Modify: `cli/macho9.c`
- Modify: `tests/cli_test.sh`
- Modify: `README.md`

**Interfaces:**
- Consumes: `ms_parse`, `me_run`.
- Produces: `macho9 edit FILE SCRIPT [--output OUT] [--verbose]`, and `FILE -` reading the script from stdin.

- [ ] **Step 1: Write the failing tests**

In `tests/cli_test.sh`, the three shapes the spec names:

```sh
# edit: the production case -- what install.sh does with three tools and
# three full writes of a 208MB binary, in one write.
build_main "$T/edit_fixture"
cat >"$T/prod.edits" <<'EOF'
# a comment, and a blank line follow

load-command  delete   uuid
dylib         replace  LIBA_PLACEHOLDER  @loader_path/../S.dylib
EOF
sed -i.bak "s|LIBA_PLACEHOLDER|$T/liba.dylib|" "$T/prod.edits" && rm -f "$T/prod.edits.bak"
"$MACHO9" edit "$T/edit_fixture" "$T/prod.edits" >"$T/edit.out" 2>"$T/edit.err" \
    && edit_rc=0 || edit_rc=$?
[ "$edit_rc" -eq 0 ] && ok "edit: the production script succeeds" \
    || bad "edit" "expected 0, got $edit_rc: $(cat "$T/edit.err")"
otool -l "$T/edit_fixture" 2>/dev/null | grep -q LC_UUID \
    && bad "edit" "LC_UUID survived the edit script" \
    || ok "edit: applied the load-command delete"
otool -L "$T/edit_fixture" 2>/dev/null | grep -q "@loader_path/../S.dylib" \
    && ok "edit: applied the dylib replace" \
    || bad "edit" "the dylib replace did not land: $(otool -L "$T/edit_fixture")"

# edit --output leaves the input alone.
build_main "$T/edit_src"
before=$(sha "$T/edit_src")
"$MACHO9" edit "$T/edit_src" "$T/prod.edits" --output "$T/edit_dst" \
    >/dev/null 2>"$T/edit_out.err" || bad "edit --output" "$(cat "$T/edit_out.err")"
[ "$(sha "$T/edit_src")" = "$before" ] && ok "edit --output: input untouched" \
    || bad "edit --output" "the input file was modified"
[ -f "$T/edit_dst" ] && ok "edit --output: wrote the output" \
    || bad "edit --output" "no output file"

# edit FILE - reads the script from stdin, so a generated script needs no
# temp file.
build_main "$T/edit_stdin"
printf 'load-command delete uuid\n' | "$MACHO9" edit "$T/edit_stdin" - \
    >/dev/null 2>"$T/edit_stdin.err" || bad "edit -" "$(cat "$T/edit_stdin.err")"
otool -l "$T/edit_stdin" 2>/dev/null | grep -q LC_UUID \
    && bad "edit -" "LC_UUID survived the stdin script" \
    || ok "edit: reads a script from stdin"

# A parse error is reported BEFORE the file is opened for writing, and names
# the line. This is what makes a typo in statement 9 of 9 cost nothing.
build_main "$T/edit_bad"
bad_before=$(sha "$T/edit_bad")
printf 'load-command delete uuid\nfrobnicate everything\n' \
    >"$T/bad.edits"
"$MACHO9" edit "$T/edit_bad" "$T/bad.edits" >/dev/null 2>"$T/editbad.err" \
    && editbad_rc=0 || editbad_rc=$?
[ "$editbad_rc" -eq 2 ] && ok "edit: a parse error is an error (2), not a refusal" \
    || bad "edit parse error" "expected 2, got $editbad_rc"
grep -q "line 2" "$T/editbad.err" && ok "edit: names the offending line" \
    || bad "edit parse error" "no line number: $(cat "$T/editbad.err")"
[ "$(sha "$T/edit_bad")" = "$bad_before" ] \
    && ok "edit: a parse error left the file untouched" \
    || bad "edit parse error" "the file was modified despite a parse error"
```

- [ ] **Step 2: Run and watch them fail**

```bash
sh tests/cli_test.sh /private/tmp/mm-build/schmonz/macho-tools/native 2>&1 | grep "FAIL edit"
```

Expected: every `edit` assertion fails — the verb does not exist.

- [ ] **Step 3: Add the verb**

`cmd_edit(argc, argv)`: parse `FILE`, `SCRIPT`, and the two flags; read the script (from stdin when `SCRIPT` is `-`); `ms_parse`; on failure print the message with its line number and return `EX_FAIL` (2, an operational failure — an unparseable script is not a considered refusal); otherwise `me_run` and return what it returns.

Register it in the verb dispatch table and in the usage text.

- [ ] **Step 4: Run the tests and watch them pass**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/cli_test.sh /private/tmp/mm-build/schmonz/macho-tools/native 2>&1 | tail -1
```

Expected: `cli_test: 0 failure(s)`.

- [ ] **Step 5: Document it in `README.md`**

The three invocation shapes, the file format, and a worked example — use the spec's `claude.edits`, which is the real `install.sh` workload. State plainly that `edit` writes nothing unless every statement succeeded, because that is the reason to reach for it over three tool invocations.

- [ ] **Step 6: Run the whole suite and commit**

```bash
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/characterize.sh /private/tmp/mm-build/schmonz/macho-tools/native check
git add cli/macho9.c tests/cli_test.sh README.md
git commit -m "feat: macho9 edit FILE SCRIPT"
```

---

### Task 5: `--dry-run`

Skips the write and nothing else. Accurate by construction rather than a prediction, because it is the same run: everything is parsed, applied and verified exactly as it would be.

**Files:**
- Modify: `cli/macho9.c`, `tests/cli_test.sh`, `README.md`

**Interfaces:**
- Consumes: `me_opts.dry_run` from Task 3, which already exists and is already tested at the C level.

- [ ] **Step 1: Write the failing test**

```sh
# --dry-run applies and verifies everything and writes nothing. It is the
# same code path as a real run, so what it reports is what would happen --
# not a static prediction, which could never show the data-dependent
# follow-up work (ordinal renumbering and the like).
build_main "$T/edit_dry"
dry_before=$(sha "$T/edit_dry")
"$MACHO9" edit --dry-run "$T/edit_dry" "$T/prod.edits" \
    >"$T/dry.out" 2>"$T/dry.err" && dry_rc=0 || dry_rc=$?
[ "$dry_rc" -eq 0 ] && ok "edit --dry-run: succeeds" \
    || bad "edit --dry-run" "expected 0, got $dry_rc: $(cat "$T/dry.err")"
[ "$(sha "$T/edit_dry")" = "$dry_before" ] \
    && ok "edit --dry-run: wrote nothing" \
    || bad "edit --dry-run" "the file was modified by a dry run"
grep -q "NOT written" "$T/dry.err" && ok "edit --dry-run: says it did not write" \
    || bad "edit --dry-run" "no 'NOT written' in the report: $(cat "$T/dry.err")"

# And a dry run of a script that WOULD be refused still reports the refusal,
# with the same exit code as the real run -- otherwise a dry run could not
# be used to find out whether the real run will work, which is its purpose.
build_main "$T/edit_dryref"
printf 'dylib delete /definitely/not/linked.dylib\nfatal-warnings\n' >"$T/dryref.edits"
"$MACHO9" edit --dry-run "$T/edit_dryref" "$T/dryref.edits" \
    >/dev/null 2>"$T/dryref.err" && dryref_rc=0 || dryref_rc=$?
[ "$dryref_rc" -eq 2 ] \
    && ok "edit --dry-run: a directive after an operation is still an error" \
    || bad "edit --dry-run refusal" "expected 2, got $dryref_rc: $(cat "$T/dryref.err")"
```

- [ ] **Step 2: Run and watch it fail**

Expected: the `--dry-run` flag is unrecognised.

- [ ] **Step 3: Wire the flag**

Parse `--dry-run` in `cmd_edit` into `me_opts.dry_run`. That is the whole change at this layer — the behaviour already exists and is already tested from Task 3.

- [ ] **Step 4: Run the tests, document the flag, commit**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
git add cli/macho9.c tests/cli_test.sh README.md
git commit -m "feat: edit --dry-run"
```

---

### Task 6: Verbose output includes the follow-ups

The report must include the work an operation does on its own, because that is the part a user cannot see for themselves — the ordinal renumbering in both the `nlist` entries and the `SET_DYLIB_ORDINAL*` opcode streams. That is exactly where `docs/PROPOSAL.md`'s defect #2 lived: a `-delete` that left ordinals stale and produced `dyld: library ordinal (4) too big`.

**Files:**
- Modify: `src/edit.c`, `src/ordinals.c` (to report counts it already computes), `tests/cli_test.sh`

**Interfaces:**
- Consumes: `me_opts.verbose`, `me_opts.log`.

- [ ] **Step 1: Write the failing test**

```sh
# Verbose must report the FOLLOW-UP work, not just the statement. A dylib
# delete renumbers every surviving ordinal in the nlist entries AND in the
# SET_DYLIB_ORDINAL* opcodes; PROPOSAL defect #2 was exactly that work not
# happening, and it surfaced as "dyld: library ordinal (4) too big" at
# runtime rather than as anything the tool said. So the log is the only
# place a user can see it.
build_main_two_dylibs "$T/edit_verb"
printf 'dylib delete %s\n' "$T/libb.dylib" >"$T/verb.edits"
"$MACHO9" edit --verbose "$T/edit_verb" "$T/verb.edits" \
    >/dev/null 2>"$T/verb.err" || bad "edit --verbose" "$(cat "$T/verb.err")"
grep -q "dylib delete" "$T/verb.err" \
    && ok "edit --verbose: names the statement" \
    || bad "edit --verbose" "no statement line: $(cat "$T/verb.err")"
grep -q "renumbered" "$T/verb.err" \
    && ok "edit --verbose: reports the ordinal renumbering it did unasked" \
    || bad "edit --verbose" "no renumbering report: $(cat "$T/verb.err")"
grep -q "nlist" "$T/verb.err" \
    && ok "edit --verbose: counts the nlist entries it touched" \
    || bad "edit --verbose" "no nlist count: $(cat "$T/verb.err")"
grep -q "SET_DYLIB_ORDINAL" "$T/verb.err" \
    && ok "edit --verbose: counts the opcodes it rewrote" \
    || bad "edit --verbose" "no opcode count: $(cat "$T/verb.err")"
```

`build_main_two_dylibs` does not exist yet; add it next to `build_main`, linking both `liba.dylib` and a second `libb.dylib`, so there is a surviving ordinal to renumber. A one-dylib fixture would make the renumbering count zero and the assertion vacuous.

- [ ] **Step 2: Run and watch it fail**

Expected: the statement line may appear, but no renumbering, nlist or opcode counts.

- [ ] **Step 3: Break `mo_map_apply`'s single figure into the parts, without a second walk**

Measured before writing this step, so build on what is actually there:

- `mo_map_apply` (`src/ordinals.c:253`) already keeps a running `changed` and, at `:380-381`, prints `"  Renumbered library ordinals: %ld symbol entries + bind streams\n"` — **one combined figure**, not the per-stream breakdown the spec's example shows.
- It prints to **`stdout`**, via `printf`, and takes an `int verbose` parameter.

So two things change, and the second is a trap:

1. Split `changed` into four counters — `nlist`, `bind`, `weak`, `lazy` — incremented where each is already walked, and return them through an out-parameter struct. **Do not recount by walking again**: a second walk is a second implementation of the same predicate, and `PROPOSAL` defect #4 was two correct implementations of one rule meeting in a merge and silently composing.
2. **Leave the existing `printf` to stdout exactly as it is.** It is on the path the six compat wrappers run, and their stdout must stay byte-identical — that is a Global Constraint and `tests/known-callers.sh` enforces it. `edit`'s richer report is a *new* write to `me_opts.log` (stderr), not a relocation of this line. If you find yourself editing that `printf`, stop: you are about to break a wrapper.

- [ ] **Step 4: Print the breakdown to `me_opts.log`**

Match the spec's worked example exactly, indented under the statement:

```
  dylib delete @loader_path/libspare.dylib
      removed LC_LOAD_DYLIB (was ordinal 4)
      renumbered 3 surviving ordinals: 5->4, 6->5, 7->6
          12 nlist entries updated
          847 SET_DYLIB_ORDINAL opcodes updated (bind 811, weak 0, lazy 36)
```

- [ ] **Step 5: Run and watch it pass**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/cli_test.sh /private/tmp/mm-build/schmonz/macho-tools/native 2>&1 | tail -1
```

- [ ] **Step 6: Prove the counts are real**

Change the fixture to link a third dylib and confirm the reported counts move. A count that does not change when the input does is not a count; record what you observed in the task report.

- [ ] **Step 7: Run the whole suite and commit**

```bash
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/characterize.sh /private/tmp/mm-build/schmonz/macho-tools/native check
git add src/edit.c src/ordinals.c src/ordinals.h tests/cli_test.sh
git commit -m "feat: edit --verbose reports the follow-up work"
```

---

## Deferred, deliberately

Two pieces of the spec are **not** in this plan, and both are recorded here so nobody has to rediscover why.

**The relation table as data** (spec: "Relations — one place to record what points at what"). Declaring the five relations and deriving both "which operations carry follow-ups" and `mg_verify`/`mg_snapshot_take`/`mg_plausible` from them. It is the design centre and it widens into `src/grow.c`'s verification, which the script language does not otherwise touch. Its stated blocker is gone — the spec says a mandatory verify gate is only worth having once `mg_plausible` actually runs, and that shipped as Task 0 of `2026-09-10-report-what-macho9-did.md`. It earns its own plan.

**Lowering the existing verbs to one-line scripts** (spec: "What this does to the CLI"). The spec is explicit that this "is not a required part of the design and it does not have to happen at once", while also noting it costs more later. It is the payoff — one grammar, one parser, `--capabilities` generated rather than maintained — and Task 2 takes the first step by generating the statement vocabulary from the table.

## Self-review

**Spec coverage.** "What this design does" → Task 4. "File format" → Tasks 1–2. "Statements" → Task 2. "Directives" → Task 2. "Execution model" → Task 3. "Verification" → Task 3 (mandatory, no flag). "`--dry-run`" → Task 5. "Verbose output must include the follow-ups" → Task 6. "Exit codes — corrected while we still can" → Task 0. "Relations" and "What this does to the CLI" → deferred above, with reasons. "Out of scope", "binutils alignment", "Consumers" → no code.

**Placeholders.** None: every code step carries the code, every test step carries the assertions, and the two mutation steps name what to break and what must fail.

**Type consistency.** `ms_split`, `ms_parse`, `ms_free`, `ms_table_row`, `ms_script`, `ms_stmt`, `me_opts`, `me_run` are spelled identically in every task that names them. The `MS_*` enumerators in Task 2's interface block are the ones Task 2's table and Task 3's `switch` use. `MR_REFUSED`, `EX_REFUSED` and `EX_FAIL` take the Task 0 values everywhere after Task 0. `MR_ERROR` appears only in warnings not to touch it.

**Two assumptions checked rather than assumed, both of which were wrong in the first draft:**

- `MR_ERROR` is not an exit code. It is `(-1)`, private to `src/rewrite.c:578`, and it is `mr_process_fat`'s per-slice status. Task 0 and Task 3 now say so explicitly, because writing `me_run` to "return `MR_ERROR`" would have returned -1 as a process status.
- `mo_map_apply` does already count, but as one combined `changed` figure printed to **stdout** (`src/ordinals.c:380-381`), not the per-stream breakdown printed to stderr that the spec's example shows. Task 6 now says to split the counter and leave that `printf` alone, because it is on the compat wrappers' stdout path and moving it would break a Global Constraint.

**One gap genuinely carried.** Nothing in the tree currently separates bind from weak from lazy in the renumbering walk, so the spec's `(bind 811, weak 0, lazy 36)` breakdown needs three counters where there is one. That is new code, not plumbing, and Task 6's step 3 is sized for it.
