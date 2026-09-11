# Never Write the Input Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every rewriting verb becomes `macho9 VERB FILE OUT …` and never writes FILE; one writer produces OUT atomically with FILE's metadata; the compat wrappers provide their historical in-place behaviour with a temp file and `mv`, refuse a hard-linked FILE, and run multi-command invocations as one edit script.

**Architecture:** A new `wa_write_new(in, out, buf, size)` replaces `wa_write_atomic`. Each file-level operation gains an output and writes through it; their in-place descriptor writes and race guards go. `compat/macho9-compat.sh` gains three functions — resolve FILE to its real target, prepare a temp beside it (refusing hard links), and install the temp with `mv` only if it differs — and `compat/translate.sh` names the output explicitly, emitting one `macho9 edit` script whenever an invocation needs more than one command. `--dry-run` goes.

**Tech Stack:** C99, stock 10.9 AppleClang 6.0, POSIX `/bin/sh`, CMake + ctest.

**Spec:** `docs/superpowers/specs/2026-09-11-never-write-the-input-design.md`

**Runs after** `docs/superpowers/plans/2026-09-11-allow-grow-everywhere.md` and `docs/superpowers/plans/2026-09-11-edit-on-fat-files.md`. It builds on names those plans introduce: `mv_add_version_min(const char *path, int allow_grow)`, `mv_add_version_min_image(uint8_t **, size_t *, int, int *)`, `me_write_once(...)`, `me_run_fat(...)`, `tests/makefat.c`/`tests/fatcheck.c` as built helpers, and `edit`'s fat support. Before Task 1, confirm with `git log` that both have landed; if either has not, stop.

## Global Constraints

- **`tests/EXPECTED` is never edited.** `sh tests/characterize.sh /private/tmp/mm-build/schmonz/macho-tools/native check` must print `characterize: OK (ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792)`. It drives `patch_macho`, `add_version_min` and `change_dylib`, so it guards the wrappers' output bytes through every change here.
- **The wrappers' stdout stays byte-identical to the C tools.** `tests/known-callers.sh` and `tests/wrapper_test.sh` gate it. `known-callers.sh`'s converted-bytes sha256s (`INSTALLSH_SHA`, `ONGOING_SHA`) are never edited: if a change moves one, that is a defect in the change.
- **`macho9` never writes FILE,** on any path, in any task after the one that converts that verb.
- **Exit codes:** 0 ok, 1 refused, 2 error. A missing OUT, or an OUT that is FILE, is 2. A wrapper's hard-link refusal is 1 (the historical "failed").
- **The tools never move a byte of file data,** except grow's memmove, the export-trie append, and a fat container's slices being laid out again.
- **POSIX `/bin/sh` only** in `compat/*.sh` and tests. No `[[`, `local`, `+=`, arrays, `<<<`, `$'...'`, `function`, `source`, `shopt`. 10.9's `readlink` has no `-f`; BSD `stat -f`, `cmp`, `mv` are available.
- **Warning-free** under stock 10.9 AppleClang 6.0.
- **A comment or doc that claims more than the code does is a defect.** Committed text never names plan artifacts.
- **Stage explicit paths only.** Never `git add -A`, `git add .`, or `git commit -a`.

Build dir for every command below: `B=/private/tmp/mm-build/schmonz/macho-tools/native` (already configured).

**Each task keeps the whole suite green.** Tasks 3–7 convert one verb family at a time together with the wrappers that call it, so no wrapper is ever broken between tasks. A task's report lists every test line it changed, and why the meaning moved.

---

### Task 1: `wa_write_new`

**Files:**
- Modify: `src/atomic_write.h`, `src/atomic_write.c` — add `wa_write_new` (keep `wa_write_atomic` until Task 8)
- Create: `tests/atomic_write_test.c`
- Modify: `CMakeLists.txt` — the `atomic_write_test` target

**Interfaces:**
- Produces:

```c
#define WA_IS_INPUT 1   /* `out` is `in`; nothing written */
#define WA_FAILED   2   /* an I/O or allocation failure; the reason is on stderr */
int wa_write_new(const char *in, const char *out, const uint8_t *buf, size_t size);
int wa_is_input(const char *in, const char *out);   /* 1 if out names in's file, else 0 */
```

Tasks 3–7 call both: `wa_is_input` before any work, `wa_write_new` at the end.

- [ ] **Step 1: Write the failing test**

`tests/atomic_write_test.c`:

```c
/*
 * tests/atomic_write_test.c -- hermetic tests for wa_write_new: it never
 * writes its input, it writes its output whole or not at all, and the output
 * carries the input's metadata. Files live in a fresh directory under
 * $TMPDIR; nothing here needs a Mach-O.
 */
#include "atomic_write.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(cond, msg, ...) do { if (!(cond)) { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); fails++; } } while (0)

static char g_dir[512];

static void path_in(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", g_dir, name);
}
static void put(const char *path, const char *text, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd >= 0) { if (write(fd, text, strlen(text)) < 0) { /* checked by readers */ } close(fd); }
    chmod(path, mode);
}
static int is(const char *path, const char *text) {
    char buf[256] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    return n == (ssize_t)strlen(text) && memcmp(buf, text, (size_t)n) == 0;
}
static int entries(void) {
    int n = 0;
    DIR *d = opendir(g_dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    if (d) closedir(d);
    return n;
}

static void test_refuses_the_input_itself(void) {
    char in[600], ln[600], hl[600];
    path_in(in, sizeof in, "in"); path_in(ln, sizeof ln, "sym"); path_in(hl, sizeof hl, "hard");
    put(in, "ORIGINAL", 0644);
    CHECK(symlink(in, ln) == 0, "setup: symlink");
    CHECK(link(in, hl) == 0, "setup: hard link");
    const uint8_t nb[] = "NEW";
    CHECK(wa_is_input(in, in) && wa_is_input(in, ln) && wa_is_input(in, hl),
          "wa_is_input: the same path, a symlink, and a hard link are all the input");
    CHECK(wa_write_new(in, in, nb, 3) == WA_IS_INPUT, "the same path is refused");
    CHECK(wa_write_new(in, ln, nb, 3) == WA_IS_INPUT, "a symlink to the input is refused");
    CHECK(wa_write_new(in, hl, nb, 3) == WA_IS_INPUT, "a hard link to the input is refused");
    CHECK(is(in, "ORIGINAL"), "the input is untouched after all three refusals");
    unlink(ln); unlink(hl); unlink(in);
}

static void test_new_output_carries_the_inputs_metadata(void) {
    char in[600], out[600];
    path_in(in, sizeof in, "in"); path_in(out, sizeof out, "out");
    put(in, "ORIGINAL", 0751);
    CHECK(setxattr(in, "com.example.tag", "keep", 4, 0, 0) == 0, "setup: xattr");
    const uint8_t nb[] = "NEWCONTENT";
    CHECK(wa_write_new(in, out, nb, 10) == 0, "a new output is written");
    CHECK(is(out, "NEWCONTENT"), "the output has the new content");
    CHECK(is(in, "ORIGINAL"), "the input is untouched");
    struct stat st;
    CHECK(stat(out, &st) == 0 && (st.st_mode & 07777) == 0751,
          "the output has the input's mode (got %o)", (unsigned)(st.st_mode & 07777));
    char v[8] = {0};
    CHECK(getxattr(out, "com.example.tag", v, sizeof v, 0, 0) == 4 && memcmp(v, "keep", 4) == 0,
          "the output has the input's extended attribute");
    unlink(in); unlink(out);
}

static void test_existing_output_is_replaced(void) {
    char in[600], out[600];
    path_in(in, sizeof in, "in"); path_in(out, sizeof out, "out");
    put(in, "ORIGINAL", 0644);
    put(out, "STALE", 0644);
    struct stat before; stat(out, &before);
    const uint8_t nb[] = "FRESH";
    CHECK(wa_write_new(in, out, nb, 5) == 0, "an existing output is replaced");
    struct stat after; stat(out, &after);
    CHECK(is(out, "FRESH") && after.st_ino != before.st_ino,
          "... by a new file, not written through the old one");
    unlink(in); unlink(out);
}

static void test_a_failed_write_leaves_the_output_as_it_was(void) {
    char in[600], out[600];
    path_in(in, sizeof in, "in"); path_in(out, sizeof out, "out");
    put(in, "ORIGINAL", 0644);
    put(out, "STALE", 0644);
    int before = entries();
    /* A file-size limit below the new content makes write() fail partway
     * with EFBIG; SIGXFSZ, which would otherwise kill the process, is
     * ignored so the failure comes back as an error. */
    signal(SIGXFSZ, SIG_IGN);
    struct rlimit old, lim;
    getrlimit(RLIMIT_FSIZE, &old);
    lim = old; lim.rlim_cur = 4;
    setrlimit(RLIMIT_FSIZE, &lim);
    const uint8_t nb[] = "FAR-TOO-LONG-FOR-THE-LIMIT";
    int rc = wa_write_new(in, out, nb, sizeof nb - 1);
    setrlimit(RLIMIT_FSIZE, &old);
    CHECK(rc == WA_FAILED, "a write that fails partway reports failure (got %d)", rc);
    CHECK(is(out, "STALE"), "the output is as it was");
    CHECK(entries() == before, "no temp file is left behind (%d entries, was %d)", entries(), before);
    unlink(in); unlink(out);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/atomic_write_test.%d", tmp ? tmp : "/tmp", (int)getpid());
    mkdir(g_dir, 0755);
    test_refuses_the_input_itself();
    test_new_output_carries_the_inputs_metadata();
    test_existing_output_is_replaced();
    test_a_failed_write_leaves_the_output_as_it_was();
    rmdir(g_dir);
    printf("atomic_write_test: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
```

Register it in `CMakeLists.txt` beside the other hermetic C tests:

```cmake
# Hermetic tests for src/atomic_write.c's wa_write_new, in a scratch
# directory under $TMPDIR: it never writes its input, it writes its output
# whole or not at all, and the output carries the input's metadata.
add_executable(atomic_write_test tests/atomic_write_test.c)
target_compile_options(atomic_write_test PRIVATE -O2 -Wall -Wextra)
target_link_libraries(atomic_write_test PRIVATE macho9core)
add_test(NAME atomic_write_test COMMAND atomic_write_test)
```

- [ ] **Step 2: Run and watch it fail to build**

```bash
cmake --build $B 2>&1 | grep -m2 "wa_write_new\|wa_is_input"
```

- [ ] **Step 3: Declare it in `src/atomic_write.h`**

After `wa_write_atomic`'s declaration:

```c
/* wa_write_new's two failure codes. */
#define WA_IS_INPUT 1   /* `out` is `in`; nothing written */
#define WA_FAILED   2   /* an I/O or allocation failure; the reason is on stderr */

/* 1 if `out` names the same file as `in` -- the same path once both are
 * resolved, or, when `out` exists, the same device and inode, which catches a
 * symlink to `in` and a hard link to it -- else 0. For a caller that wants to
 * refuse before doing any work; wa_write_new checks again at the write. */
int wa_is_input(const char *in, const char *out);

/* Write `size` bytes of `buf` as the file `out`, never touching `in`.
 * Returns WA_IS_INPUT, writing nothing, when wa_is_input(in, out). Otherwise
 * writes a temp file in `out`'s resolved directory -- a symlink at `out` is
 * followed to its target, not replaced -- gives it `in`'s mode, `in`'s owner
 * (best-effort: needs privilege) and every extended attribute `in` carries,
 * fsyncs it, and renames it onto `out`. So `out` is either what it was or the
 * whole new content, never a partial file. Returns 0, WA_IS_INPUT, or
 * WA_FAILED with the reason on stderr; on any non-zero return `out` is as it
 * was and no temp file remains. */
int wa_write_new(const char *in, const char *out, const uint8_t *buf, size_t size);
```

- [ ] **Step 4: Define them in `src/atomic_write.c`**

After `wa_write_atomic`:

```c
int wa_is_input(const char *in, const char *out) {
    char rin[PATH_MAX], rout[PATH_MAX];
    if (realpath(in, rin) && realpath(out, rout) && strcmp(rin, rout) == 0) return 1;
    struct stat si, so;
    if (stat(in, &si) == 0 && stat(out, &so) == 0 &&
        si.st_dev == so.st_dev && si.st_ino == so.st_ino) return 1;
    return 0;
}

int wa_write_new(const char *in, const char *out, const uint8_t *buf, size_t size) {
    if (wa_is_input(in, out)) {
        fprintf(stderr, "%s: the output is the input; refusing to write it\n", out);
        return WA_IS_INPUT;
    }
    /* An existing `out` that is a symlink is followed, so the link keeps
     * pointing where it did and its target gets the new content. */
    char real[PATH_MAX];
    const char *target = (realpath(out, real) != NULL) ? real : out;

    struct stat ist;
    int have_in = (stat(in, &ist) == 0);

    size_t tlen = strlen(target) + 8;
    char *tmpl = (char *)malloc(tlen);
    if (!tmpl) { fprintf(stderr, "out of memory\n"); return WA_FAILED; }
    snprintf(tmpl, tlen, "%s.XXXXXX", target);
    int tfd = mkstemp(tmpl);
    if (tfd < 0) { perror("mkstemp"); free(tmpl); return WA_FAILED; }

    if (have_in) {
        fchmod(tfd, ist.st_mode & 07777);
        fchown(tfd, ist.st_uid, ist.st_gid);   /* best-effort: needs privilege */
    }
    if (wa_copy_xattrs(in, tfd) != 0)
        fprintf(stderr, "warning: %s: could not copy all extended attributes "
                        "(e.g. com.apple.quarantine) from %s\n", target, in);

    size_t off = 0;
    int failed = 0;
    while (off < size) {
        ssize_t n = write(tfd, buf + off, size - off);
        if (n < 0) { perror("write"); failed = 1; break; }
        off += (size_t)n;
    }
    if (!failed && fsync(tfd) != 0) { perror("fsync"); failed = 1; }
    close(tfd);
    if (failed || rename(tmpl, target) != 0) {
        if (!failed) perror("rename");
        unlink(tmpl);
        free(tmpl);
        return WA_FAILED;
    }
    free(tmpl);
    return 0;
}
```

- [ ] **Step 5: Run the test and watch it pass**

```bash
cmake --build $B && ctest --test-dir $B -R atomic_write_test --output-on-failure
```

- [ ] **Step 6: Prove two tests can fail**

One at a time, each reverted: make `wa_is_input` return 0 always — the refusal checks fail; drop the `fchmod`/`wa_copy_xattrs` calls — the metadata checks fail. Record both.

- [ ] **Step 7: Commit**

```bash
ctest --test-dir $B
git add src/atomic_write.h src/atomic_write.c tests/atomic_write_test.c CMakeLists.txt
git commit -m "feat: wa_write_new writes an output whole, with the input's metadata, never the input"
```

---

### Task 2: Multi-command wrapper invocations become one edit script

Before any verb changes shape, the wrappers stop running several `macho9` commands against a copy. Every invocation that translates to more than one command becomes one `macho9 edit FILE -` with the statements on stdin. `edit` still edits in place in this task; Task 7 gives it an output.

**Files:**
- Modify: `compat/translate.sh` — `mt_tr_change_dylib` and `mt_tr_fix_macho` emit an edit script when they would emit more than one command
- Modify: `compat/macho9-compat.sh` — `mw_run` evaluates the whole translation; `mw_translate` counts commands, not lines; `mw_run_atomic` is deleted
- Modify: `compat/change_dylib.sh`, `compat/fix_macho.sh` — call `mw_run`
- Modify: `tests/translate_test.sh`, `tests/known-callers.sh` (the teaching assertion only), `tests/wrapper_test.sh` (assertions about the removed copy-aside path)

**Interfaces:**
- Produces: a translation is now at most one command per binary. For `change_dylib`/`fix_macho` needing more than one family (or more than one segment rename), that command is:

```
macho9 edit FILE - <<'MACHO9_EDIT'
allow-grow
load-command delete uuid
dylib replace /old /new
MACHO9_EDIT
```

(`allow-grow` only when `-grow` was given; each operand quoted by `mt_quote`, which is exactly the parser's word rules.)

- [ ] **Step 1: Record the baseline**

```bash
ctest --test-dir $B -R "known_callers|wrapper_test|translate_test|characterize|change_dylib_test" 2>&1 | tail -3
sh tests/known-callers.sh $B 2>&1 | grep -c '^ok\|^PASS'
```

Save both outputs for the report.

- [ ] **Step 2: Decide the statement order, and the one refusal**

A verb applies all of one family's operations as a batch against the original image; an edit script applies statements in sequence. They agree when each family's statements are emitted in this order, which the translation must use:

1. `load-command delete` (every `-strip-lc`) — first, as today, because deleting commands hands header pad back;
2. per family (dylib, then rpath): every `delete` and `reexport`, then every `replace`, then every `append`, then every `insert` **in reverse flag order** — script inserts each go to the front, so `-insert A -insert B` (A ordinal 1, B ordinal 2 as a batch) must be emitted `insert B` then `insert A`;
3. segment renames, in flag order.

With that order a sequence matches the batch except for one shape: a `-change` whose NEW path is another `-change`'s OLD path (a chain, or a swap). Sequentially the second replace would also rewrite what the first produced. Refuse it, in the translation, only on the edit-script path: `mt_die "-change $a $b and -change $b $c chain: run them as separate invocations"`. A single-family invocation keeps using the verb, whose batch handles it.

Write this reasoning as the comment above the emission code in `compat/translate.sh`.

- [ ] **Step 3: Emit the edit script**

In `mt_tr_change_dylib`, collect each operation as a statement string as well as the verb flags it builds today — e.g. alongside `mt_dy="$mt_dy$(mt_qargs -replace "$2" "$3")"`, append a line to a per-kind list:

```sh
mt_st_dyrepl="$mt_st_dyrepl$(printf 'dylib replace%s' "$(mt_qargs "$2" "$3")")
"
```

with separate lists `mt_st_lc`, `mt_st_dydel` (delete and reexport), `mt_st_dyrepl`, `mt_st_dyapp`, `mt_st_dyins`, and the four rpath counterparts. For inserts, prepend instead of append, so the list comes out reversed:

```sh
mt_st_dyins="$(printf 'dylib insert%s' "$(mt_qargs "$2")")
$mt_st_dyins"
```

At the end, count the families present (`lc`, `dylib`, `rpath`). If exactly one, print the verb line exactly as today. If more than one, check the chain rule over the collected replace pairs (per family), then print:

```sh
    printf '%s edit%s - <<'"'"'MACHO9_EDIT'"'"'\n' "$mt_pre" "$(mt_qargs "$mt_file")"
    [ -n "$mt_grow" ] && printf 'allow-grow\n'
    printf '%s%s%s%s%s%s%s%s%s' "$mt_st_lc" "$mt_st_dydel" "$mt_st_dyrepl" "$mt_st_dyapp" \
        "$mt_st_dyins" "$mt_st_rpdel" "$mt_st_rprepl" "$mt_st_rpapp" "$mt_st_rpins"
    printf 'MACHO9_EDIT\n'
```

`mt_tr_fix_macho` does the same for its three kinds: `-strip_build_version` is `load-command delete build-version`, `-change` is `dylib replace`, `-rename_seg OLD NEW` is `segment rename OLD NEW`; more than one command's worth — which now includes two or more renames — becomes one edit script.

- [ ] **Step 4: Run the whole translation, count commands**

In `compat/macho9-compat.sh`:

- `mw_run` evaluates the translation as one script, so a heredoc reaches `macho9`'s stdin:

```sh
mw_run() {
    eval "$MW_CMDS" </dev/null
}
```

  (A translation is now at most one command; `retag_swift_classes.sh` still runs its per-binary lines itself.) Update `mw_run`'s comment: it no longer loops, and why.

- `mw_translate` counts commands, not lines, since a heredoc spans several:

```sh
    MW_NCMDS=$(printf '%s\n' "$MW_CMDS" | awk -v p="$(mt_pre_word) " 'index($0, p) == 1 { n++ } END { print n + 0 }')
```

- Delete `mw_run_atomic`, its long comment, and `MW_TMPFILE`'s uses that only it made (keep `MW_TMPFILE` in `mw_cleanup`: Task 3 uses it again). `compat/change_dylib.sh` and `compat/fix_macho.sh` call `mw_run` where they called `mw_run_atomic`.

- [ ] **Step 5: Update the tests whose meaning moved**

- `tests/translate_test.sh`: every multi-family expectation becomes the edit form. Single-family expectations are unchanged. Add: `change_dylib f -insert A -insert B -strip-lc uuid` emits `dylib insert B` before `dylib insert A`; a chained `-change a b -change b c` with `-strip-lc uuid` is refused; the same chain without `-strip-lc` (one family) is not.
- `tests/known-callers.sh`: the install.sh teaching assertion (`grep -q 'macho9 lc ' … && grep -q 'macho9 dylib '`) becomes `grep -q 'macho9 edit ' "$T/e3" && grep -q 'load-command delete uuid' "$T/e3" && grep -q 'dylib replace' "$T/e3"`. **Its sha256 assertions do not change.**
- `tests/wrapper_test.sh`: assertions that describe `mw_run_atomic`'s copy (a `.NAME.macho9-compat.PID` file, stdout naming the copy) are about a path that no longer exists: delete them, and in their place assert that a multi-family `change_dylib` leaves no stray file beside FILE and that its stdout names FILE, not a copy.

Add one wrapper test: `change_dylib f -insert /A -insert /B -strip-lc uuid` leaves `/A` at ordinal 1 and `/B` at ordinal 2 (`macho9 info`).

- [ ] **Step 6: Run everything**

```bash
cmake --build $B && ctest --test-dir $B && sh tests/characterize.sh $B check
```

Expected: all pass; characterize OK; `known-callers.sh`'s two converted-bytes sha256 assertions pass unchanged. **If a sha256 moved, stop**: the edit-script path produced different bytes from lc-then-dylib, which is a real finding to report, not an expectation to update.

- [ ] **Step 7: Commit**

```bash
git add compat/translate.sh compat/macho9-compat.sh compat/change_dylib.sh compat/fix_macho.sh \
        tests/translate_test.sh tests/known-callers.sh tests/wrapper_test.sh
git commit -m "feat: multi-command wrapper invocations run as one edit script"
```

---

### Task 3: The wrappers' install path, and `macho9 minos FILE OUT`

The smallest verb first, to prove the machinery every later task reuses.

**Files:**
- Modify: `compat/macho9-compat.sh` — `mw_resolve`, `mw_prepare`, `mw_retranslate`, `mw_run_to_tmp`, `mw_finish`
- Modify: `compat/translate.sh` — output naming (`MT_OUT`), for `add_version_min` first
- Modify: `compat/add_version_min.sh`
- Modify: `src/version_min.h`, `src/version_min.c` — `mv_add_version_min(path, out, allow_grow)`
- Modify: `cli/macho9.c` — `minos FILE OUT 10.9 [--allow-grow]`
- Modify: every test that runs `macho9 minos` or calls `mv_add_version_min`; `tests/wrapper_test.sh` for the new wrapper behaviours

**Interfaces:**
- Consumes: `wa_is_input`, `wa_write_new` (Task 1).
- Produces:

```c
int mv_add_version_min(const char *path, const char *out, int allow_grow);
```

and, in `compat/macho9-compat.sh`, the functions below. `MT_OUT` is the translation's output: when set, `mt_translate` names it as each command's OUT; when unset (the teaching form), it names `FILE.new` and ends with `mv -f FILE.new FILE`, so what a user is shown is a complete, pasteable equivalent.

- [ ] **Step 1: Write the failing tests**

In `tests/cli_test.sh`, beside the existing `minos` tests:

```sh
# minos never writes its input: FILE OUT, and an OUT that is FILE is refused.
build_main "$T/mo_in"; "$T/strip_version_min" "$T/mo_in" >/dev/null
mo_before=$(sha "$T/mo_in"); mo_ino=$(stat -f %i "$T/mo_in")
"$MACHO9" minos "$T/mo_in" "$T/mo_out" 10.9 >"$T/mo.out" 2>"$T/mo.err" \
    && ok "minos FILE OUT: succeeds" || bad "minos FILE OUT" "$(cat "$T/mo.err")"
[ "$(sha "$T/mo_in")" = "$mo_before" ] && [ "$(stat -f %i "$T/mo_in")" = "$mo_ino" ] \
    && ok "minos FILE OUT: FILE is untouched" || bad "minos FILE OUT" "FILE changed"
"$MACHO9" info "$T/mo_out" | grep -q LC_VERSION_MIN_MACOSX \
    && ok "minos FILE OUT: OUT has the command" || bad "minos FILE OUT" "OUT lacks it"
grep -q "^Wrote $T/mo_out (" "$T/mo.out" \
    && ok "minos FILE OUT: says what it wrote" || bad "minos FILE OUT" "no Wrote line: $(cat "$T/mo.out")"
rc=0; "$MACHO9" minos "$T/mo_in" "$T/mo_in" 10.9 >/dev/null 2>"$T/mo_same.err" || rc=$?
[ "$rc" -eq 2 ] && [ "$(sha "$T/mo_in")" = "$mo_before" ] \
    && ok "minos: OUT that is FILE is refused (2), FILE untouched" || bad "minos OUT=FILE" "rc $rc"
grep -q "never writes its input" "$T/mo_same.err" \
    && ok "minos: ... refused up front, before any work" \
    || bad "minos OUT=FILE" "not the up-front refusal: $(cat "$T/mo_same.err")"
ln -s "$T/mo_in" "$T/mo_link"
rc=0; "$MACHO9" minos "$T/mo_in" "$T/mo_link" 10.9 >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] && ok "minos: OUT that is a symlink to FILE is refused (2)" || bad "minos OUT=link" "rc $rc"
rc=0; "$MACHO9" minos "$T/mo_in" 10.9 >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 2 ] && ok "minos: a missing OUT is a usage error (2)" || bad "minos no OUT" "rc $rc"
```

In `tests/wrapper_test.sh`, a block for the wrapper install path, using `add_version_min`:

```sh
# The wrappers keep editing FILE "in place" -- by writing a temp beside the
# real target and mv-ing it over. A symlinked FILE updates its target and
# stays a symlink; a hard-linked FILE is refused; a refusal leaves no temp
# behind; mode and xattrs survive.
cp "$FIXTURE" "$T/w_real"; strip_vm "$T/w_real"      # see the note below
ln -s w_real "$T/w_link"
( cd "$T" && "$BIN/add_version_min" w_link ) >/dev/null 2>"$T/w.err" \
    && ok "wrapper: a symlinked FILE is edited" || bad "wrapper symlink" "$(cat "$T/w.err")"
[ -L "$T/w_link" ] && "$BIN/macho9" info "$T/w_real" | grep -q LC_VERSION_MIN_MACOSX \
    && ok "wrapper: ... through the link, which is still a link" || bad "wrapper symlink" "link replaced or target unchanged"

cp "$FIXTURE" "$T/w_h1"; strip_vm "$T/w_h1"; ln "$T/w_h1" "$T/w_h2"
h_before=$(shasum -a 256 < "$T/w_h1")
rc=0; "$BIN/add_version_min" "$T/w_h1" >/dev/null 2>"$T/wh.err" || rc=$?
[ "$rc" -eq 1 ] && [ "$(shasum -a 256 < "$T/w_h1")" = "$h_before" ] \
    && ok "wrapper: a hard-linked FILE is refused (1), untouched" || bad "wrapper hard link" "rc $rc"
grep -q "hard link" "$T/wh.err" && ok "wrapper: ... and says why" || bad "wrapper hard link" "$(cat "$T/wh.err")"
ls -a "$T" | grep -q 'macho9-compat' && bad "wrapper" "a temp file was left behind" \
    || ok "wrapper: no temp file left behind"

cp "$FIXTURE" "$T/w_meta"; strip_vm "$T/w_meta"; chmod 0751 "$T/w_meta"
xattr -w com.apple.quarantine "0081;00000000;test;" "$T/w_meta"
"$BIN/add_version_min" "$T/w_meta" >/dev/null 2>&1
[ "$(stat -f %Lp "$T/w_meta")" = 751 ] && xattr -p com.apple.quarantine "$T/w_meta" >/dev/null 2>&1 \
    && ok "wrapper: mode and quarantine survive" || bad "wrapper metadata" "mode $(stat -f %Lp "$T/w_meta")"
```

`$FIXTURE` and the removal of any existing `LC_VERSION_MIN_MACOSX` (`strip_vm` above) must use whatever `wrapper_test.sh` already has for a writable thin fixture and for stripping the command — read it before writing this block, and use those names. If it has no strip helper, build one the way `tests/cli_test.sh`'s `strip_version_min` does.

- [ ] **Step 2: Run and watch them fail**

```bash
cmake --build $B && sh tests/cli_test.sh $B 2>&1 | grep "minos" | grep FAIL; sh tests/wrapper_test.sh $B 2>&1 | grep "wrapper" | grep FAIL
```

- [ ] **Step 3: `mv_add_version_min` writes OUT**

In `src/version_min.c`: open `path` `O_RDONLY` only to report an unreadable file early (keep the `open`/`fstat` messages); delete the race guard (the `stat` comparison and its comment — there is no second open to race any more: the tool now only reads `path`, through `mi_open`); after `mv_add_version_min_image` succeeds, write with `wa_write_new(path, out, buf, fsize)` — even when `added` is 0, since OUT must exist after a 0 exit — then print `Added …` (only when `added`) followed by `Wrote OUT (N bytes)`:

```c
    int wr = wa_write_new(path, out, buf, fsize);
    if (wr == WA_IS_INPUT) { free(buf); return MR_FAIL; }   /* checked earlier; a path changed */
    if (wr != 0) { free(buf); return MR_FAIL; }
    if (added)
        printf("Added LC_VERSION_MIN_MACOSX 10.9 (ncmds=%u, sizeofcmds=%u)\n",
               hdr->ncmds, hdr->sizeofcmds);
    printf("Wrote %s (%zu bytes)\n", out, fsize);
```

Update `src/version_min.h`'s comment: it reads `path`, writes `out`, and never writes `path`.

- [ ] **Step 4: `macho9 minos FILE OUT 10.9 [--allow-grow]`**

`cmd_minos(path, out, version, allow_grow)` refuses first, before any work, when `wa_is_input(path, out)`:

```c
    if (wa_is_input(path, out)) {
        fprintf(stderr, "macho9 minos: %s is %s; macho9 never writes its input\n", out, path);
        return EX_FAIL;
    }
```

The dispatch accepts exactly `minos FILE OUT 10.9` or `minos FILE OUT 10.9 --allow-grow` (argc 5 or 6); anything else prints `usage: %s minos FILE OUT 10.9 [--allow-grow]` and returns `EX_FAIL`. Update the file-header grammar and the usage text.

- [ ] **Step 5: The wrapper functions**

Add to `compat/macho9-compat.sh`, after `mw_require_writable`:

```sh
# mw_resolve PATH -- print the file PATH finally names once every symlink in
# its last component is followed. 10.9's readlink has no -f, so this follows
# one level at a time. The install lands on that file, so a FILE that is a
# symlink stays one.
mw_resolve() {
    mw_p=$1
    mw_hops=0
    while [ -L "$mw_p" ]; do
        mw_hops=$((mw_hops + 1))
        [ "$mw_hops" -le 32 ] || { printf '%s: too many levels of symbolic links\n' "$1" >&2; return 1; }
        mw_l=$(readlink "$mw_p") || return 1
        case $mw_l in
            /*) mw_p=$mw_l ;;
            *)  case $mw_p in */*) mw_p=${mw_p%/*}/$mw_l ;; *) mw_p=$mw_l ;; esac ;;
        esac
    done
    printf '%s\n' "$mw_p"
}

# mw_prepare FILE [new-ok] -- set MW_TARGET to the file FILE really is and
# MW_TMPFILE to a fresh name beside it, for macho9 to write. Refuses what
# cannot be replaced safely: a FILE this user could not have written (the C
# tools opened it read-write, and mv would otherwise replace it anyway), and
# a FILE with other hard links, which mv would leave on the old content.
# With `new-ok`, a FILE that does not exist yet is fine (patch_macho's OUT).
mw_prepare() {
    if [ "${2:-}" = new-ok ] && [ ! -e "$1" ] && [ ! -L "$1" ]; then
        MW_TARGET=$1
    else
        mw_require_writable "$1" || return 1
        MW_TARGET=$(mw_resolve "$1") || return 1
        mw_links=$(stat -f %l "$MW_TARGET" 2>/dev/null) || mw_links=1
        if [ "$mw_links" -gt 1 ]; then
            printf '%s: %s has %d hard links; replacing it would leave the others with the old content. Break the link first, or run macho9 with an explicit output.\n' \
                "$MW_TOOL" "$1" "$mw_links" >&2
            return 1
        fi
    fi
    case $MW_TARGET in
        */*) mw_dirpart=${MW_TARGET%/*}; mw_basepart=${MW_TARGET##*/} ;;
        *)   mw_dirpart=.;               mw_basepart=$MW_TARGET ;;
    esac
    [ -n "$mw_dirpart" ] || mw_dirpart=/
    MW_TMPFILE="$mw_dirpart/.$mw_basepart.macho9-compat.$$"
    rm -f -- "$MW_TMPFILE"
    return 0
}

# mw_retranslate TOOL ARG... -- translate again, this time writing MW_TMPFILE.
# The same argv translated a moment ago, with only the output named; a
# difference means the translation depends on something it must not.
mw_retranslate() {
    MT_PROG0=$0
    MW_CMDS=$(MT_OUT=$MW_TMPFILE mt_translate "$@")
    mw_trc=$?
    unset MT_PROG0
    if [ "$mw_trc" -ne 0 ]; then
        printf '%s: internal error: the translation is not stable under a change of output\n' "$MW_TOOL" >&2
        return 1
    fi
    return 0
}

# mw_run_to_tmp -- run the translation (which writes MW_TMPFILE) with its
# stdout captured, then pass every line through except a final "Wrote ..."
# naming the temp file, which no C tool ever printed. Returns macho9's status.
mw_run_to_tmp() {
    mw_run >"$MW_T/out"
    mw_rc=$?
    if [ "$(sed -n '$p' "$MW_T/out" | cut -c1-6)" = 'Wrote ' ]; then
        sed '$d' "$MW_T/out"
    else
        cat "$MW_T/out"
    fi
    return "$mw_rc"
}

# mw_finish -- after macho9 wrote MW_TMPFILE: if it differs from MW_TARGET,
# mv it over (atomic: same directory), else discard it -- the C tools wrote
# nothing when nothing changed. Sets MW_CHANGED. Returns 1 only if the mv
# failed, leaving MW_TARGET as it was.
mw_finish() {
    MW_CHANGED=0
    if [ -e "$MW_TARGET" ] && cmp -s -- "$MW_TMPFILE" "$MW_TARGET"; then
        rm -f -- "$MW_TMPFILE"; MW_TMPFILE=''
        return 0
    fi
    if ! mv -f -- "$MW_TMPFILE" "$MW_TARGET"; then
        printf '%s: %s: the rewrite succeeded but installing it failed\n' "$MW_TOOL" "$MW_TARGET" >&2
        rm -f -- "$MW_TMPFILE"; MW_TMPFILE=''
        return 1
    fi
    MW_TMPFILE=''
    MW_CHANGED=1
    return 0
}
```

`mw_cleanup` already removes `MW_TMPFILE` on every exit; keep it.

- [ ] **Step 6: Name the output in the translation**

In `compat/translate.sh`, add:

```sh
# The output a translated command writes. A wrapper sets MT_OUT to its temp
# file. Without it -- the teaching form, printed on stderr and by
# `sh translate.sh` -- the output is FILE.new, and the caller appends
# mt_install_line so the equivalent shown is complete: macho9 never writes
# its input, so replacing FILE is a second step.
mt_out_for() {
    if [ -n "${MT_OUT:-}" ]; then printf '%s' "$MT_OUT"; else printf '%s.new' "$1"; fi
}
mt_install_line() {
    [ -n "${MT_OUT:-}" ] || printf 'mv -f%s\n' "$(mt_qargs "$1.new" "$1")"
}
```

`mt_tr_add_version_min` becomes:

```sh
mt_tr_add_version_min() {
    [ $# -eq 1 ] || { printf 'Usage: %s binary\n' "$MT_PROG" >&2; return 1; }
    printf '%s minos%s 10.9\n' "$(mt_pre_word)" "$(mt_qargs "$1" "$(mt_out_for "$1")")"
    mt_install_line "$1"
}
```

`mw_translate`'s command count (Task 2) counts lines beginning with the `macho9` word; extend the awk to count lines beginning `mv -f` too, so the teaching message's "command"/"commands" agrees.

- [ ] **Step 7: `add_version_min.sh` installs**

Replace its `mw_run` / `exit $?` with:

```sh
mw_prepare "$1" || exit 1
mw_retranslate add_version_min "$@" || exit 1
mw_run_to_tmp
mw_rc=$?
[ "$mw_rc" -eq 0 ] || exit "$mw_rc"
mw_finish || exit 1
exit 0
```

Its stdout is `macho9 minos`'s minus the final `Wrote` line: `Added LC_VERSION_MIN_MACOSX 10.9 (…)` or `LC_VERSION_MIN_MACOSX already present; nothing to do.` — the C tool's lines.

- [ ] **Step 8: Convert the remaining callers**

Every `macho9 minos FILE 10.9` in the suites becomes `macho9 minos FILE OUT 10.9`, reading OUT where they read FILE afterwards; every C call of `mv_add_version_min(p, g)` becomes `mv_add_version_min(p, out, g)`. `tests/translate_test.sh`'s `add_version_min` expectation gains `f.new` and the `mv -f f.new f` line.

- [ ] **Step 9: Run everything, prove it**

```bash
cmake --build $B && ctest --test-dir $B && sh tests/characterize.sh $B check
```

Then, one at a time, reverted:
1. Remove `cmd_minos`'s `wa_is_input` refusal. `wa_write_new` still refuses at the write, so the exit code and FILE survive — but its message is "the output is the input", so `minos: ... refused up front, before any work` fails. That is the assertion that proves the check runs before the work.
2. Remove `mw_prepare`'s link-count check: the hard-link wrapper test fails.

Record both.

- [ ] **Step 10: Commit**

```bash
git status --short     # every path below, and nothing else, should be listed
git add compat/macho9-compat.sh compat/translate.sh compat/add_version_min.sh \
        src/version_min.h src/version_min.c cli/macho9.c \
        <each changed test file, by name>
git commit -m "feat!: macho9 minos FILE OUT; the wrappers install through a temp and mv"
```

---

### Task 4: `macho9 retag-swift FILE OUT`

**Files:**
- Modify: `src/swift_retag.h`, `src/swift_retag.c` — `mswift_retag_file(path, out)`; the in-place write and `MSWIFT_RACED` race guard go
- Modify: `cli/macho9.c` — `retag-swift FILE OUT`
- Modify: `compat/translate.sh` (`mt_tr_retag_swift_classes`), `compat/retag_swift_classes.sh`
- Modify: every test running `macho9 retag-swift`

**Interfaces:** `int mswift_retag_file(const char *path, const char *out);` — returns the retag count (≥ 0) having written OUT, or `MSWIFT_ERROR`/`MSWIFT_NOT_MACHO` as today. `MSWIFT_RACED` is deleted: there is no second open to race.

- [ ] **Step 1: Write the failing tests**

In `tests/cli_test.sh`, the same five assertions as Task 3 Step 1 for `retag-swift FILE OUT` (FILE untouched; OUT written; `Wrote OUT`; OUT=FILE is 2; missing OUT is 2), on the fixture the existing `retag-swift` tests use.

- [ ] **Step 2: Run and watch them fail**

- [ ] **Step 3: Convert**

`mswift_retag_file(path, out)` opens `path` read-only (for early open/fstat messages), `mi_open`s it, retags in memory, and writes `wa_write_new(path, out, buf, fsize)` whether or not anything changed; `cmd_retag_swift(path, out)` refuses `wa_is_input` first (2) and prints `PATH: retagged N class record(s)` as today, then `Wrote OUT (N bytes)`. Delete `MSWIFT_RACED`, its comment in `swift_retag.h` (and the stale "only MSWIFT_ERROR is a failure of the tool itself" sentence becomes true again — keep it, now accurately), and `cmd_retag_swift`'s branch for it.

`mt_tr_retag_swift_classes` emits, per binary, `macho9 retag-swift FILE OUT` with `mt_out_for`/`mt_install_line`. `retag_swift_classes.sh` loops per binary as it does, but for each: `mw_prepare "$mw_f"`, re-translate that one binary (`MT_OUT=$MW_TMPFILE mt_translate retag_swift_classes "$mw_f"`), run it with stdout captured as today, then `mw_finish` when it succeeded. Its per-binary and `total:` lines are computed from `macho9`'s `retagged N` line exactly as now; a binary with N = 0 is not rewritten (`mw_finish`'s `cmp` discards the identical temp).

- [ ] **Step 4: Convert the remaining callers; run everything; commit**

```bash
cmake --build $B && ctest --test-dir $B && sh tests/characterize.sh $B check
git add src/swift_retag.h src/swift_retag.c cli/macho9.c compat/translate.sh compat/retag_swift_classes.sh <each changed test file>
git commit -m "feat!: macho9 retag-swift FILE OUT"
```

---

### Task 5: `dylib`, `rpath`, `lc`, `segment` take FILE OUT

All four go through `mr_apply_file`, so they move together, with `change_dylib`, `fix_macho` and `rename_segment`'s single-family paths.

**Files:**
- Modify: `src/rewrite.h`, `src/rewrite.c` — `mr_apply_file(path, out, ops)`
- Modify: `cli/macho9.c` — `cmd_dylib_or_rpath`, `cmd_lc`, `cmd_segment` take OUT as the second positional
- Modify: `compat/translate.sh` (`mt_tr_change_dylib`'s and `mt_tr_fix_macho`'s single-family lines, `mt_tr_rename_segment`), `compat/change_dylib.sh`, `compat/fix_macho.sh`, `compat/rename_segment.sh`
- Modify: every test running these four verbs

**Interfaces:** `int mr_apply_file(const char *path, const char *out, const mr_ops *ops);`

- [ ] **Step 1: Write the failing tests**

In `tests/cli_test.sh`, the same five assertions as Task 3 Step 1 for each of `dylib FILE OUT -append /x`, `rpath FILE OUT -append /x`, `lc FILE OUT -delete uuid` and `segment FILE OUT __DATA __DATX`. Plus: `dylib FILE OUT -delete /not/linked` (nothing to change) exits 0 and OUT is byte-identical to FILE — a 0 exit must leave OUT there.

- [ ] **Step 2: Run and watch them fail**

- [ ] **Step 3: `mr_apply_file` writes OUT**

Open `path` `O_RDONLY` (it only reads now; keep the too-small and magic checks and their messages). Refuse `wa_is_input(path, out)` first with `MR_FAIL` and `"%s is %s; macho9 never writes its input"`. At the end, replace the `if (rc == 0 && modified) { wa_write_atomic … printf("Updated …") }` block with an unconditional write when `rc == 0`:

```c
    if (rc == 0) {
        int wr = wa_write_new(path, out, buf, fsize);
        if (wr != 0) {
            fprintf(stderr, "ERROR: %s not written\n", out);
            rc = MR_FAIL;
        } else {
            printf("Wrote %s (%zu bytes)\n", out, fsize);
        }
    }
```

Keep the `processed_ok` logic and the miss report after the write exactly as they are, and correct their comments where they name "Updated". The per-slice `label` stays `path` — FILE — so every progress line is unchanged.

- [ ] **Step 4: The verbs**

`cmd_dylib_or_rpath`, `cmd_lc` and `cmd_segment` take OUT as the positional right after FILE (flags keep their placement rules); a missing OUT is `EX_FAIL` with the verb's usage, which names `FILE OUT`. Update the dispatch argc checks, the usage text and the file-header grammar.

- [ ] **Step 5: The wrappers**

Single-family lines in `mt_tr_change_dylib` and `mt_tr_fix_macho` gain `"$(mt_out_for "$mt_file")"` after the file, and each translator ends with `mt_install_line "$mt_file"` (the edit-script branch from Task 2 is Task 7's). `mt_tr_rename_segment` the same.

`change_dylib.sh` and `fix_macho.sh` become:

```sh
mw_prepare "$1" || exit 1
mw_retranslate change_dylib "$@" || exit 1     # fix_macho for fix_macho.sh
mw_run_to_tmp
mw_rc=$?
[ "$mw_rc" -eq 0 ] || exit "$mw_rc"            # fix_macho.sh: exit 1, as it maps today
mw_finish || exit 1
[ "$MW_CHANGED" -eq 1 ] && printf 'Updated %s (%s bytes)\n' "$1" "$(wc -c < "$MW_TARGET" | tr -d ' ')"
exit 0
```

— the C tools printed `Updated FILE (N bytes)` naming the FILE they were given, only when they changed it. **Before relying on that, run the pre-change `tests/wrapper_test.sh` and `tests/known-callers.sh` and check each wrapper's actual final stdout line on a changed and an unchanged run**; if `fix_macho`'s differs from `change_dylib`'s, print what it printed. An invocation Task 2 turned into an edit script has no `Wrote` line to drop and still gets the `Updated` line here.

`rename_segment.sh` keeps its flow — `mw_require_writable`, the `macho9 info` pre-check, capturing all output, parsing `renamed=N`, `exit 2` for 0 — with `mw_prepare`/`mw_retranslate` before the run and `mw_finish` after it, only when N > 0 (N = 0 removes the temp without installing).

- [ ] **Step 6: Convert the remaining callers; run everything**

Every `macho9 dylib|rpath|lc|segment FILE …` in the suites gains OUT; C callers of `mr_apply_file` gain `out`.

```bash
cmake --build $B && ctest --test-dir $B && sh tests/characterize.sh $B check
```

`known-callers.sh`'s sha256s must pass unchanged.

- [ ] **Step 7: Prove the zero-exit-leaves-OUT test can fail**

Temporarily write only when `modified`, confirm "nothing to change … OUT is byte-identical to FILE" fails. Revert. Record it.

- [ ] **Step 8: Commit**

```bash
git add src/rewrite.h src/rewrite.c cli/macho9.c compat/translate.sh compat/change_dylib.sh \
        compat/fix_macho.sh compat/rename_segment.sh <each changed test file>
git commit -m "feat!: dylib, rpath, lc and segment take FILE OUT"
```

---

### Task 6: `declassify` refuses OUT = FILE; `grow FILE OUT N`

**Files:**
- Modify: `cli/macho9.c` — `cmd_declassify` (its shape is already FILE OUT), `cmd_grow`
- Modify: `compat/translate.sh` (`mt_tr_patch_macho`), `compat/patch_macho.sh`
- Modify: every test running `macho9 grow`; tests of `declassify` with IN = OUT

- [ ] **Step 1: Write the failing tests**

`declassify f f` is 2 and `f` is untouched; `declassify f g` writes `g` with `f`'s mode (not a fixed 0755: `wa_write_new` copies the input's). `grow FILE OUT 4096`: FILE untouched, OUT grown, the five assertions of Task 3 Step 1. Through the wrapper: `patch_macho f f` still converts `f` (the C tool allowed IN = OUT), and `patch_macho f g` still writes `g`.

- [ ] **Step 2: Run and watch them fail**

- [ ] **Step 3: Convert**

`cmd_declassify` refuses `wa_is_input(in, out)` first (2), writes `wa_write_new(in, out, buf, len)`, and keeps its `Wrote OUT (N bytes)` line. Remove the usage text's "IN is only read, so IN and OUT may match". `cmd_grow(path, out, n)` refuses `wa_is_input`, reads `path` (the `O_RDWR` open becomes `O_RDONLY`), writes OUT, and prints `Grew %s: header pad enlarged, file now %zu bytes` naming OUT. Dispatch: `grow FILE OUT N` (argc 5).

`mt_tr_patch_macho` emits `macho9 declassify IN OUT`, where OUT is `mt_out_for "$2"` when MT_OUT is set, and — in the teaching form — the user's own OUT when it differs from IN as a string, or `IN.new` plus `mt_install_line` when they are equal. `patch_macho.sh` always installs onto the user's OUT: `mw_prepare "$mw_out" new-ok`, re-translate with `MT_OUT`, run with stdout captured (it already drops macho9's final `Wrote` line and prints its own), then `mw_finish` — replacing today's `cat "$MW_T/converted" > "$mw_out"`, the last non-atomic write in the wrappers. When OUT did not exist, keep the C tool's creation mode: `chmod` the temp to `0755 & ~umask` (the computation the script already has) before `mw_finish`; when it did exist, `chmod` the temp to OUT's current mode (`stat -f %Lp`), which is what `cat >` preserved. Its final `Wrote OUT (N bytes)` line (when not "Already patched") is unchanged.

- [ ] **Step 4: Convert the remaining callers; run everything; commit**

```bash
cmake --build $B && ctest --test-dir $B && sh tests/characterize.sh $B check
git add cli/macho9.c compat/translate.sh compat/patch_macho.sh <each changed test file>
git commit -m "feat!: declassify refuses its input as output; grow FILE OUT N"
```

---

### Task 7: `macho9 edit FILE OUT SCRIPT`; `--output` and `--dry-run` go

**Files:**
- Modify: `cli/macho9.c` — `cmd_edit`: positionals FILE OUT SCRIPT; `--output` and `--dry-run` removed; usage; `--capabilities`
- Modify: `src/edit.h`, `src/edit.c` — `me_run` requires `out`; `me_opts.dry_run` removed; `me_write_once` writes through `wa_write_new`
- Modify: `compat/translate.sh` — the edit-script branch names OUT; `compat/change_dylib.sh`/`fix_macho.sh` need nothing more
- Modify: `tests/edit_test.c`, `tests/cli_test.sh`, `README.md`

- [ ] **Step 1: Write the failing tests**

In `tests/cli_test.sh`: `edit FILE OUT SCRIPT` leaves FILE's hash and inode unchanged and writes OUT; `edit FILE FILE SCRIPT` is 2; `edit FILE SCRIPT` (two positionals) is 2; `edit --dry-run …` and `edit … --output X` are 2 (unknown flags); `--capabilities` has `verb edit flags=verbose`. In `tests/edit_test.c`, a test that `me_run(path, path, …)` returns `MR_FAIL` and writes nothing.

- [ ] **Step 2: Run and watch them fail**

- [ ] **Step 3: Convert**

`cmd_edit` takes exactly three positionals (FILE, OUT, SCRIPT — SCRIPT may be `-`) with `--verbose` anywhere; refuses `wa_is_input(FILE, OUT)` before reading the script (2, "macho9 never writes its input"). `me_run(path, out, s, o)` requires a non-NULL `out`, checks `wa_is_input` first (returning `MR_FAIL`), and `me_write_once` becomes:

```c
static int me_write_once(uint8_t *buf, size_t size, const char *path, const char *out,
                         FILE *log, int verbose) {
    char bytes[32];
    me_commas(bytes, size);
    int wr = wa_write_new(path, out, buf, size);
    free(buf);
    if (wr != 0) {
        me_say(log, "macho9 edit: writing %s failed; %s left unmodified\n", out, path);
        return MR_FAIL;
    }
    if (verbose) me_say(log, "%s: written (%s bytes)\n", out, bytes);
    return 0;
}
```

`me_write_once` loses its `mode` parameter (`wa_write_new` takes the mode from the input) and its `dry_run` parameter; update both of its callers — the thin path at the end of `me_run` and `me_run_fat` — to `me_write_once(buf, size, path, out, log, verbose)`. Remove `me_opts.dry_run`, every branch on it, the dry-run paragraphs in `src/edit.h` and the README, and `me_say_left`'s in-place wording where it no longer applies (a refused run now leaves FILE unmodified *and* OUT not written, always — say both). Delete `edit_test.c`'s dry-run tests and `cli_test.sh`'s `--dry-run` block; a scratch OUT replaces them, and the README says so in one sentence where `--dry-run` was documented.

The edit-script branch in `compat/translate.sh` becomes `macho9 edit FILE OUT - <<'MACHO9_EDIT'` with `mt_out_for`, followed by `mt_install_line`.

- [ ] **Step 4: Convert the remaining callers; run everything; commit**

Every `me_run(p, NULL, …)` in `tests/edit_test.c` gets an OUT beside `p` and reads the result from OUT; every `macho9 edit FILE SCRIPT [--output OUT]` in the suites becomes `macho9 edit FILE OUT SCRIPT`.

```bash
cmake --build $B && ctest --test-dir $B && sh tests/characterize.sh $B check
git add cli/macho9.c src/edit.h src/edit.c compat/translate.sh README.md <each changed test file>
git commit -m "feat!: macho9 edit FILE OUT SCRIPT; --output and --dry-run are gone"
```

---

### Task 8: `wa_write_atomic` goes; the docs say what is true

**Files:**
- Modify: `src/atomic_write.h`, `src/atomic_write.c` — delete `wa_write_atomic` and `wa_write_in_place`; the file header describes `wa_write_new`
- Modify: `cli/macho9.c` — `--capabilities` gains the `FILE OUT` line; the file-header comments
- Modify: `README.md`, `compat/README.md`, `tests/README.md`
- Modify: `docs/superpowers/specs/2026-09-10-edit-scripts-design.md` — the amendment
- Modify: `docs/superpowers/QUEUE.md`

- [ ] **Step 1: Nothing calls the old writer**

```bash
git grep -n "wa_write_atomic\|wa_write_in_place" -- src cli tests compat
```

Expected: only their own definitions and comments. Delete both functions and every comment that describes the hard-link fallback; `src/atomic_write.h`'s header comment describes `wa_write_new` and says why there is no hard-link case (the output is always a new file).

- [ ] **Step 2: `--capabilities` says the shape**

After the `exitcodes` line:

```c
    /* Every rewriting verb reads FILE and writes OUT, the positional right
     * after it, and refuses an OUT that is FILE: macho9 never writes its
     * input. A wrapper checks for this line rather than assume the shape. */
    printf("output positional=2 never-writes-input\n");
```

Document the new line type in `print_capabilities`' contract comment. Add a `cli_test.sh` assertion that the line is present.

- [ ] **Step 3: The docs**

- `README.md`: every rewriting verb's synopsis shows `FILE OUT`; a short section "macho9 never writes its input" says what OUT is, that OUT = FILE (by path, symlink or hard link) is refused, that OUT gets FILE's mode, owner (with privilege) and xattrs, and that the compat wrappers keep editing in place by writing a temp beside FILE and moving it over — refusing a FILE with other hard links.
- `compat/README.md` and `tests/README.md`: correct every statement about in-place writes, `mw_run_atomic`, and exit codes that this plan changed.
- The edit-scripts spec: under "binutils alignment", move "`objcopy`'s `infile [outfile]` shape, where omitting the output modifies in place via temp-and-rename" from **Matched** to **Knowingly divergent**, saying the output is always named and never the input; mark its `--output` and `--dry-run` passages superseded by `2026-09-11-never-write-the-input-design.md`.
- `docs/superpowers/QUEUE.md`: item 9 done.

- [ ] **Step 4: Run everything; commit**

```bash
cmake --build $B && ctest --test-dir $B && sh tests/characterize.sh $B check
git add src/atomic_write.h src/atomic_write.c cli/macho9.c README.md compat/README.md tests/README.md \
        tests/cli_test.sh docs/superpowers/specs/2026-09-10-edit-scripts-design.md docs/superpowers/QUEUE.md
git commit -m "docs: macho9 never writes its input; the old in-place writer is gone"
```

---

## Self-review

**Spec coverage.** Grammar → Tasks 3–7 (one family each) and Task 8's capabilities line. `wa_write_new` → Task 1 (refuses the input by path, symlink and hard link; atomic; mode, owner, xattrs). "Who calls it" → Tasks 3 (minos), 4 (retag-swift), 5 (`mr_apply_file`: dylib, rpath, lc, segment), 6 (declassify, grow), 7 (`me_run`). "The input is checked before any work" → each verb's `wa_is_input` refusal. "Success lines tell the truth" → `Wrote OUT` in Tasks 3–6; a zero exit always leaves OUT (Task 5's test). The wrappers' five steps → Task 3 (`mw_resolve`, `mw_prepare` with the hard-link refusal and the writability check, `mw_retranslate`, `mw_run_to_tmp`, `mw_finish`), applied in Tasks 4–6. Multi-command as one edit script → Task 2, output named in Task 7. `--dry-run` removed → Task 7. Testing → Task 1 (the four `wa_write_new` cases, including `RLIMIT_FSIZE`), Tasks 3–7 (CLI), Task 3 (symlink, hard link, no temp left, metadata), Task 2 (insert order through the edit path), `characterize.sh` and `known-callers.sh`'s sha256s throughout. Mutations → Task 1 Step 6 (the input check, the metadata copy), Task 3 Step 9 (the hard-link refusal). Edit-scripts spec amendment → Task 8.

**One refinement of the spec, which planning found:** a verb applies a family's operations as a batch and an edit script applies them in sequence, so Task 2 fixes the statement order that makes the two agree and refuses the one shape no order can express (a `-change` chain), on the edit-script path only. Recorded in the spec.

**Placeholders.** Task 3 Step 1's wrapper test names two helpers (`$FIXTURE`, `strip_vm`) that must be matched to `tests/wrapper_test.sh`'s existing ones, and says so. Task 5 Step 5 requires checking each wrapper's historical final line against the existing gates before relying on "Updated FILE (N bytes)". Both are verifications of the tree, not missing design.

**Type consistency.** `wa_write_new`, `wa_is_input`, `WA_IS_INPUT`, `WA_FAILED`; `mv_add_version_min(path, out, allow_grow)`, `mswift_retag_file(path, out)`, `mr_apply_file(path, out, ops)`, `me_run(path, out, s, o)`, `me_write_once(buf, size, path, out, log, verbose)`; `mw_resolve`, `mw_prepare`, `mw_retranslate`, `mw_run_to_tmp`, `mw_finish`, `MW_TARGET`, `MW_TMPFILE`, `MW_CHANGED`; `MT_OUT`, `mt_out_for`, `mt_install_line` — each spelled the same everywhere it appears.
