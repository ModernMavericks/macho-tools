# Edit on Fat Files Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `macho9 edit` applies a script to a fat (universal) file — every 64-bit slice by default, or the slices an `arch` directive names — keeping the container and passing untouched slices through byte-identical.

**Architecture:** `mr_process_fat`'s split-and-reassemble moves into `src/fat.c` as `mfat_rewrite`, driven by two callbacks (one per slice, one per placement), so the verbs and `edit` share one layout rule. A new `src/arch_names.c` maps lipo's names to cputypes. The parser gains an `arch NAME` directive, recorded as a bitmask over that table. `me_run` gains a fat path that runs the whole script per selected slice through `mfat_rewrite`, verifies each slice, and decides "matched nothing" when the last selected slice has run each statement.

**Tech Stack:** C99, stock 10.9 AppleClang 6.0, CMake + ctest. Hermetic C tests (`tests/fat_test.c` new, `tests/edit_test.c`, `tests/script_test.c`); CLI behaviour in `tests/cli_test.sh`.

**Spec:** `docs/superpowers/specs/2026-09-11-edit-on-fat-files-design.md`

## Global Constraints

- **`tests/EXPECTED` is never edited.** `sh tests/characterize.sh /private/tmp/mm-build/schmonz/macho-tools/native check` must print `characterize: OK (ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792)`.
- **The six `compat/` wrappers' stdout stays byte-identical,** including on fat inputs. The verb fat path's stdout — each slice's rewrite lines under the label `arch N (cputype 0x…)`, "not a 64-bit Mach-O; leaving this slice unchanged", "Nothing to change.", and "arch N: placed at OFF (SIZE bytes)" — does not change. `tests/known-callers.sh`, `tests/wrapper_test.sh` and `tests/change_dylib_test.sh` are the gates.
- **The container keeps every slice.** `edit` never drops or reorders one. A slice no statement touched is byte-identical; it may only move, when an earlier slice grew.
- **Any slice failing refuses the whole run, and nothing is written.**
- **`FAT_MAGIC_64` stays refused.**
- **Exit codes:** 0 ok, 1 refused (MR_REFUSED / EX_REFUSED), 2 error (MR_FAIL / EX_FAIL). A parse error is 2.
- **A thin file's behaviour is unchanged,** except that an `arch` directive can now refuse it.
- **The tools never move a byte of file data,** except grow's memmove and the export-trie append — and, as today, a fat container's slices being laid out again after one grew.
- **POSIX `/bin/sh` only** in test scripts. **Warning-free** under stock 10.9 AppleClang 6.0.
- **A comment or doc that claims more than the code does is a defect.** Committed text never names plan artifacts.
- **Stage explicit paths only.** Never `git add -A`, `git add .`, or `git commit -a`.

Build dir for every command below: `B=/private/tmp/mm-build/schmonz/macho-tools/native` (already configured).

---

### Task 1: One table of arch names

**Files:**
- Create: `src/arch_names.h`, `src/arch_names.c`
- Modify: `src/mach_compat.h` — cputype/subtype constants the 10.9 SDK may lack
- Create: `tests/fat_test.c`
- Modify: `CMakeLists.txt` — `src/arch_names.c` into `macho9core`; the `fat_test` target

**Interfaces:**
- Produces:

```c
int  ma_row(int r, const char **name, uint32_t *cputype, uint32_t *cpusubtype); /* 0 past the end */
int  ma_lookup(const char *name);                        /* row, or -1 */
int  ma_index(uint32_t cputype, uint32_t cpusubtype);    /* row, or -1; capability bits ignored */
void ma_describe(uint32_t cputype, uint32_t cpusubtype, char out[32]);  /* name, or "cputype 0x…" */
void ma_list(char *out, size_t outsz);                   /* "x86_64, x86_64h, arm64, arm64e, i386" */
```

Task 4 (the parser) uses `ma_lookup`/`ma_list`; Task 5 (`edit`) uses `ma_index`/`ma_describe`/`ma_row`.

- [ ] **Step 1: Write the failing test**

`tests/fat_test.c`:

```c
/*
 * tests/fat_test.c -- hermetic tests for src/arch_names.c and src/fat.c's
 * mfat_rewrite. Fat containers are built here by hand, big-endian as every
 * real one is, from slices that are plain bytes: nothing under test here
 * reads a slice's contents, so no Mach-O fixture is needed.
 */
#include "arch_names.h"
#include "fat.h"
#include "mach_compat.h"

#include <mach-o/fat.h>
#include <mach/machine.h>
#include <libkern/OSByteOrder.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg, ...) do { if (!(cond)) { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); fails++; } } while (0)

static void test_arch_names_round_trip(void) {
    int rows = 0;
    const char *name; uint32_t ct, cs;
    for (int r = 0; ma_row(r, &name, &ct, &cs); r++) {
        rows++;
        CHECK(ma_lookup(name) == r, "arch %s: its name finds its own row (got %d)", name, ma_lookup(name));
        CHECK(ma_index(ct, cs) == r, "arch %s: its cputype/subtype find its own row", name);
        CHECK(ma_index(ct, cs | 0x80000000u) == r,
              "arch %s: capability bits in the subtype's high byte are ignored", name);
        char d[32];
        ma_describe(ct, cs, d);
        CHECK(strcmp(d, name) == 0, "arch %s: described by its name (got '%s')", name, d);
    }
    CHECK(rows == 5, "the table has lipo's five names (got %d)", rows);
    CHECK(ma_lookup("amd64") == -1, "an unknown name is rejected");
    CHECK(ma_index(0x12345678u, 0) == -1, "an unknown cputype has no row");
    char d[32];
    ma_describe(0x12345678u, 0, d);
    CHECK(strcmp(d, "cputype 0x12345678") == 0, "an unknown cputype is described by number (got '%s')", d);
    char list[128];
    ma_list(list, sizeof list);
    CHECK(strcmp(list, "x86_64, x86_64h, arm64, arm64e, i386") == 0, "the name list (got '%s')", list);
}

int main(void) {
    test_arch_names_round_trip();
    printf("fat_test: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Register it, and watch it fail to build**

In `CMakeLists.txt`, beside `script_test`:

```cmake
# Hermetic tests for src/arch_names.c and src/fat.c's mfat_rewrite: fat
# containers built in the test from plain-byte slices, so no fixture file is
# needed. MallocScribble for the same reason as script_test: mfat_rewrite
# frees every split buffer on every path, and a use-after-free there reads
# garbage rather than crashing unless freed memory is scribbled.
add_executable(fat_test tests/fat_test.c)
target_compile_options(fat_test PRIVATE -O2 -Wall -Wextra)
target_link_libraries(fat_test PRIVATE macho9core)
add_test(NAME fat_test COMMAND fat_test)
set_tests_properties(fat_test PROPERTIES ENVIRONMENT MallocScribble=1)
```

```bash
cmake --build $B 2>&1 | grep -m2 "arch_names.h"
```

Expected: `arch_names.h` not found.

- [ ] **Step 3: Add the constants to `src/mach_compat.h`**

Beside the other `#ifndef` blocks, and add `#include <mach/machine.h>` at the top if it is not already included:

```c
/* cputype/subtype values for lipo's arch names (src/arch_names.c). arm64 and
 * the x86_64h/arm64e subtypes postdate or barely predate the 10.9 SDK;
 * defined here only where its headers lack them. */
#ifndef CPU_TYPE_ARM64
#define CPU_TYPE_ARM64 ((cpu_type_t)(CPU_TYPE_ARM | CPU_ARCH_ABI64))
#endif
#ifndef CPU_SUBTYPE_ARM64_ALL
#define CPU_SUBTYPE_ARM64_ALL ((cpu_subtype_t)0)
#endif
#ifndef CPU_SUBTYPE_ARM64E
#define CPU_SUBTYPE_ARM64E ((cpu_subtype_t)2)
#endif
#ifndef CPU_SUBTYPE_X86_64_H
#define CPU_SUBTYPE_X86_64_H ((cpu_subtype_t)8)
#endif
#ifndef CPU_SUBTYPE_MASK
#define CPU_SUBTYPE_MASK 0xff000000u
#endif
```

- [ ] **Step 4: Write `src/arch_names.h`**

```c
#ifndef MACHO9_ARCH_NAMES_H
#define MACHO9_ARCH_NAMES_H
/*
 * ma_ -- lipo's architecture names, and the cputype/cpusubtype each means.
 *
 * One table, so the `arch` directive, edit's messages, and anything else
 * that names a slice agree on what a name means. A subtype is compared with
 * its high byte masked off (CPU_SUBTYPE_MASK): that byte carries capability
 * bits (CPU_SUBTYPE_LIB64 on x86_64, pointer-auth ABI bits on arm64e) that
 * vary between otherwise identical slices.
 */
#include <stddef.h>
#include <stdint.h>

/* Row `r` of the table: sets *name, *cputype, *cpusubtype and returns 1, or
 * returns 0 when `r` is past the end. Rows are stable: the `arch` directive
 * records a set of names as a bitmask over them (src/script.h). */
int ma_row(int r, const char **name, uint32_t *cputype, uint32_t *cpusubtype);

/* The row a name means, or -1 for a name not in the table. */
int ma_lookup(const char *name);

/* The row a cputype/cpusubtype pair means, or -1. */
int ma_index(uint32_t cputype, uint32_t cpusubtype);

/* The name for a cputype/cpusubtype pair, or "cputype 0x…" for one the
 * table does not know. */
void ma_describe(uint32_t cputype, uint32_t cpusubtype, char out[32]);

/* Every name, comma-separated, for messages that list what was accepted. */
void ma_list(char *out, size_t outsz);

#endif /* MACHO9_ARCH_NAMES_H */
```

- [ ] **Step 5: Write `src/arch_names.c`**

```c
#include "arch_names.h"
#include "mach_compat.h"

#include <mach/machine.h>
#include <stdio.h>
#include <string.h>

static const struct { const char *name; uint32_t cputype, cpusubtype; } MA_TABLE[] = {
    { "x86_64",  (uint32_t)CPU_TYPE_X86_64, (uint32_t)CPU_SUBTYPE_X86_64_ALL },
    { "x86_64h", (uint32_t)CPU_TYPE_X86_64, (uint32_t)CPU_SUBTYPE_X86_64_H   },
    { "arm64",   (uint32_t)CPU_TYPE_ARM64,  (uint32_t)CPU_SUBTYPE_ARM64_ALL  },
    { "arm64e",  (uint32_t)CPU_TYPE_ARM64,  (uint32_t)CPU_SUBTYPE_ARM64E     },
    { "i386",    (uint32_t)CPU_TYPE_I386,   (uint32_t)CPU_SUBTYPE_I386_ALL   },
};
static const int MA_N = (int)(sizeof MA_TABLE / sizeof MA_TABLE[0]);

int ma_row(int r, const char **name, uint32_t *cputype, uint32_t *cpusubtype) {
    if (r < 0 || r >= MA_N) return 0;
    *name = MA_TABLE[r].name;
    *cputype = MA_TABLE[r].cputype;
    *cpusubtype = MA_TABLE[r].cpusubtype;
    return 1;
}

int ma_lookup(const char *name) {
    for (int r = 0; r < MA_N; r++)
        if (strcmp(name, MA_TABLE[r].name) == 0) return r;
    return -1;
}

int ma_index(uint32_t cputype, uint32_t cpusubtype) {
    uint32_t sub = cpusubtype & ~(uint32_t)CPU_SUBTYPE_MASK;
    for (int r = 0; r < MA_N; r++)
        if (MA_TABLE[r].cputype == cputype && MA_TABLE[r].cpusubtype == sub) return r;
    return -1;
}

void ma_describe(uint32_t cputype, uint32_t cpusubtype, char out[32]) {
    int r = ma_index(cputype, cpusubtype);
    if (r >= 0) snprintf(out, 32, "%s", MA_TABLE[r].name);
    else        snprintf(out, 32, "cputype 0x%x", cputype);
}

void ma_list(char *out, size_t outsz) {
    size_t o = 0;
    if (outsz) out[0] = '\0';
    for (int r = 0; r < MA_N && o < outsz; r++) {
        int w = snprintf(out + o, outsz - o, "%s%s", r ? ", " : "", MA_TABLE[r].name);
        if (w < 0) break;
        o += (size_t)w;
    }
}
```

Add `src/arch_names.c` to `macho9core`'s source list in `CMakeLists.txt`.

- [ ] **Step 6: Run the test and watch it pass**

```bash
cmake --build $B && ctest --test-dir $B -R fat_test --output-on-failure
```

Expected: `fat_test: 0 failure(s)`, no warnings.

- [ ] **Step 7: Prove it can fail**

Temporarily drop the `& ~(uint32_t)CPU_SUBTYPE_MASK` in `ma_index`, rebuild, and confirm the "capability bits … are ignored" checks fail. Revert. Record it in the task report.

- [ ] **Step 8: Commit**

```bash
ctest --test-dir $B
git add src/arch_names.h src/arch_names.c src/mach_compat.h tests/fat_test.c CMakeLists.txt
git commit -m "feat: one table of lipo's arch names"
```

---

### Task 2: The fat-fixture helpers become built programs

Both `change_dylib_test.sh` and, in Task 6, `cli_test.sh` need to build and inspect fat files. Today `change_dylib_test.sh` writes `makefat.c` and `fatcheck.c` as heredocs and compiles them itself. A second copy in `cli_test.sh` would be two implementations of one fixture format, so they move into committed sources that CMake builds beside `macho9`.

**Files:**
- Create: `tests/makefat.c`, `tests/fatcheck.c` — the heredoc bodies from `tests/change_dylib_test.sh:229-291` and `:294-382`, verbatim
- Modify: `tests/change_dylib_test.sh:222-383` — use `"$BIN/makefat"` and `"$BIN/fatcheck"`
- Modify: `CMakeLists.txt` — two helper executables

**Interfaces:**
- Produces: `$BIN/makefat OUT S0 CT0 CS0 AL0 S1 CT1 CS1 AL1` (two slices, each at its alignment, in the order given) and `$BIN/fatcheck archinfo|dump|dylibs FILE [IDX [OUT]]`, where `$BIN` is the directory holding `macho9` — the argument every CLI suite already receives.

- [ ] **Step 1: Move the sources**

Copy the C between `cat > "$T/makefat.c" <<'EOF'` and its `EOF` into `tests/makefat.c`, and the C between `cat > "$T/fatcheck.c" <<'EOF'` and its `EOF` into `tests/fatcheck.c`, byte for byte. Put the comment that precedes them in the script (`:222-228`, "makefat/fatcheck: build and inspect a fat (universal) Mach-O without depending on system lipo…") at the top of `tests/makefat.c` as a C comment, and a one-line pointer to it at the top of `tests/fatcheck.c`.

- [ ] **Step 2: Build them with CMake**

Beside the other test executables in `CMakeLists.txt`:

```cmake
# Fixture helpers, not tests: build and inspect a fat container without
# system lipo, whose accepted architectures are not the suites' to pin.
# Built beside macho9 so every CLI suite finds them in the directory it is
# already given.
add_executable(makefat tests/makefat.c)
add_executable(fatcheck tests/fatcheck.c)
target_compile_options(makefat PRIVATE -O2)
target_compile_options(fatcheck PRIVATE -O2)
```

- [ ] **Step 3: Point `change_dylib_test.sh` at them**

Delete both heredocs and both `"$CC" -O2 -o "$T/makefat" …` / `"$T/fatcheck"` compile lines. Replace every `"$T/makefat"` with `"$BIN/makefat"` and every `"$T/fatcheck"` with `"$BIN/fatcheck"` — check the variable name the script uses for its bindir argument (its first positional) and use that. Keep the explanatory comment in the script, shortened to a pointer to `tests/makefat.c`.

- [ ] **Step 4: Run the suite**

```bash
cmake --build $B && ctest --test-dir $B -R change_dylib_test --output-on-failure && ctest --test-dir $B
```

Expected: `change_dylib_test` passes with the same number of PASS lines as before the move (count them before and after; report both), and ctest is green.

- [ ] **Step 5: Commit**

```bash
git add tests/makefat.c tests/fatcheck.c tests/change_dylib_test.sh CMakeLists.txt
git commit -m "tests: makefat and fatcheck become built helpers, shared by the CLI suites"
```

---

### Task 3: `mfat_rewrite` — one split-and-reassemble

**Files:**
- Modify: `src/fat.h`, `src/fat.c` — `mfat_rewrite` and its two callback types; the header's scope comment
- Modify: `src/rewrite.c:997-1230` — `mr_process_fat` becomes a caller
- Modify: `tests/fat_test.c` — four `mfat_rewrite` tests

**Interfaces:**
- Consumes: `mfat_parse`, `mfat_get`, `mfat_arch`, `MFAT_IO_ERROR`, `MFAT_MALFORMED` (existing in `src/fat.h`).
- Produces:

```c
typedef int  (*mfat_slice_fn)(uint8_t **pbuf, size_t *psize, const mfat_arch *a,
                              uint32_t index, int *changed, void *ctx);
typedef void (*mfat_placed_fn)(const mfat_arch *a, uint32_t index,
                               uint64_t new_offset, uint64_t new_size, void *ctx);
int mfat_rewrite(uint8_t **pbuf, size_t *psize, uint32_t narch, int swapped,
                 mfat_slice_fn fn, mfat_placed_fn placed, void *ctx, int *modified);
```

A slice callback returns 0 (setting `*changed` if it changed the slice) or a **positive** failure code, which `mfat_rewrite` returns unchanged. `mfat_rewrite`'s own failures are negative: `MFAT_IO_ERROR` for an allocation, `MFAT_MALFORMED` when the new layout would overlap two slices. Task 5 uses all of this.

- [ ] **Step 1: Write the failing tests**

Add to `tests/fat_test.c`, before `main`:

```c
/* A fat container of n plain-byte slices -- slice i is filled with 'A'+i --
 * at the given offsets, big-endian, each aligned to 2^12. */
static uint8_t *build_fat(uint32_t n, const uint32_t *off, const uint32_t *sz, size_t *outlen) {
    size_t total = 0;
    for (uint32_t i = 0; i < n; i++)
        if (off[i] + sz[i] > total) total = off[i] + sz[i];
    uint8_t *buf = (uint8_t *)calloc(1, total);
    struct fat_header *fh = (struct fat_header *)buf;
    fh->magic = OSSwapHostToBigInt32(FAT_MAGIC);
    fh->nfat_arch = OSSwapHostToBigInt32(n);
    struct fat_arch *fa = (struct fat_arch *)(fh + 1);
    for (uint32_t i = 0; i < n; i++) {
        fa[i].cputype = (cpu_type_t)OSSwapHostToBigInt32((uint32_t)CPU_TYPE_X86_64);
        fa[i].cpusubtype = (cpu_subtype_t)OSSwapHostToBigInt32((uint32_t)CPU_SUBTYPE_X86_64_ALL);
        fa[i].offset = OSSwapHostToBigInt32(off[i]);
        fa[i].size = OSSwapHostToBigInt32(sz[i]);
        fa[i].align = OSSwapHostToBigInt32(12);
        memset(buf + off[i], 'A' + (int)i, sz[i]);
    }
    *outlen = total;
    return buf;
}

typedef struct {
    int      grow_index;   /* the slice to grow, or -1 */
    size_t   grow_to;
    int      fail_index;   /* the slice whose callback fails, or -1 */
    int      fail_code;
    uint64_t placed_off[4];
    int      placed_n;
} t_ctx;

static int t_slice(uint8_t **pbuf, size_t *psize, const mfat_arch *a,
                   uint32_t index, int *changed, void *ctx_) {
    t_ctx *c = (t_ctx *)ctx_;
    (void)a;
    if ((int)index == c->fail_index) return c->fail_code;
    if ((int)index == c->grow_index) {
        uint8_t *nb = (uint8_t *)realloc(*pbuf, c->grow_to);
        if (!nb) return 99;
        memset(nb + *psize, 'Z', c->grow_to - *psize);
        *pbuf = nb; *psize = c->grow_to; *changed = 1;
    }
    return 0;
}

static void t_placed(const mfat_arch *a, uint32_t index, uint64_t off, uint64_t size, void *ctx_) {
    t_ctx *c = (t_ctx *)ctx_;
    (void)a; (void)size;
    if (index < 4) c->placed_off[index] = off;
    c->placed_n++;
}

static int t_run(uint8_t **buf, size_t *len, t_ctx *c, int *modified) {
    uint32_t narch; int swap;
    if (mfat_parse(*buf, *len, &narch, &swap) != 0) return -100;
    return mfat_rewrite(buf, len, narch, swap, t_slice, t_placed, c, modified);
}

static void test_rewrite_nothing_changed_leaves_the_input(void) {
    uint32_t off[2] = { 0x1000, 0x2000 }, sz[2] = { 0x1000, 0x1000 };
    size_t len; uint8_t *buf = build_fat(2, off, sz, &len);
    uint8_t *orig = buf; size_t len0 = len;
    uint8_t *copy = (uint8_t *)malloc(len0); memcpy(copy, buf, len0);
    t_ctx c = { -1, 0, -1, 0, {0}, 0 };
    int modified = 1;
    int rc = t_run(&buf, &len, &c, &modified);
    CHECK(rc == 0, "rewrite, nothing changed: returns 0 (got %d)", rc);
    CHECK(modified == 0, "rewrite, nothing changed: modified is clear");
    CHECK(buf == orig && len == len0 && memcmp(copy, buf, len0) == 0,
          "rewrite, nothing changed: the input is untouched");
    CHECK(c.placed_n == 0, "rewrite, nothing changed: nothing is re-placed");
    free(copy); free(buf);
}

static void test_rewrite_a_grown_slice_moves_the_next(void) {
    uint32_t off[2] = { 0x1000, 0x2000 }, sz[2] = { 0x1000, 0x1000 };
    size_t len; uint8_t *buf = build_fat(2, off, sz, &len);
    t_ctx c = { 0, 0x1800, -1, 0, {0}, 0 };
    int modified = 0;
    int rc = t_run(&buf, &len, &c, &modified);
    CHECK(rc == 0 && modified, "rewrite, slice 0 grown: succeeds and is modified (rc %d)", rc);
    uint32_t narch; int swap;
    CHECK(mfat_parse(buf, len, &narch, &swap) == 0 && narch == 2, "rewrite: the result parses");
    mfat_arch a0, a1;
    mfat_get(buf, swap, 0, &a0);
    mfat_get(buf, swap, 1, &a1);
    CHECK(a0.offset == 0x1000 && a0.size == 0x1800, "rewrite: slice 0 keeps its offset and grew");
    CHECK(a1.offset == 0x3000 && a1.size == 0x1000,
          "rewrite: slice 1 moved to its next 2^12 boundary (got 0x%x)", a1.offset);
    int intact = 1;
    for (uint32_t i = 0; i < 0x1000; i++) if (buf[a1.offset + i] != 'B') intact = 0;
    CHECK(intact, "rewrite: slice 1's bytes are unchanged");
    CHECK(len == 0x4000, "rewrite: the file ends where slice 1 does (got 0x%zx)", len);
    CHECK(c.placed_n == 2 && c.placed_off[1] == 0x3000,
          "rewrite: the placement callback saw both slices, slice 1 at 0x3000");
    free(buf);
}

static void test_rewrite_refuses_an_overlapping_layout(void) {
    /* Non-ascending: slice 0 sits above slice 1. Growing slice 1 in place
     * (it keeps its offset, being the first to change) would run it into
     * slice 0. */
    uint32_t off[2] = { 0x3000, 0x1000 }, sz[2] = { 0x1000, 0x1000 };
    size_t len; uint8_t *buf = build_fat(2, off, sz, &len);
    uint8_t *orig = buf; size_t len0 = len;
    uint8_t *copy = (uint8_t *)malloc(len0); memcpy(copy, buf, len0);
    t_ctx c = { 1, 0x2800, -1, 0, {0}, 0 };
    int modified = 0;
    int rc = t_run(&buf, &len, &c, &modified);
    CHECK(rc == MFAT_MALFORMED, "rewrite: an overlapping layout is refused (got %d)", rc);
    CHECK(buf == orig && len == len0 && memcmp(copy, buf, len0) == 0,
          "rewrite: a refused layout leaves the input untouched");
    free(copy); free(buf);
}

static void test_rewrite_passes_a_callback_failure_through(void) {
    uint32_t off[2] = { 0x1000, 0x2000 }, sz[2] = { 0x1000, 0x1000 };
    size_t len; uint8_t *buf = build_fat(2, off, sz, &len);
    uint8_t *orig = buf; size_t len0 = len;
    uint8_t *copy = (uint8_t *)malloc(len0); memcpy(copy, buf, len0);
    t_ctx c = { 0, 0x1800, 1, 7, {0}, 0 };   /* slice 0 grows, then slice 1 fails */
    int modified = 0;
    int rc = t_run(&buf, &len, &c, &modified);
    CHECK(rc == 7, "rewrite: a callback's failure code is returned unchanged (got %d)", rc);
    CHECK(buf == orig && len == len0 && memcmp(copy, buf, len0) == 0,
          "rewrite: a failure on the second slice leaves the input untouched");
    free(copy); free(buf);
}
```

Add the four calls to `main`.

- [ ] **Step 2: Run and watch it fail to build**

```bash
cmake --build $B 2>&1 | grep -m2 "mfat_rewrite"
```

Expected: `mfat_rewrite` undeclared.

- [ ] **Step 3: Declare it in `src/fat.h`**

Replace the header's "Scope:" paragraph ("Scope: reading and validating the arch table only. Writing a fresh fat_header/fat_arch (change_dylib's -grow reassembly) is specific to that one caller and stays there.") with:

```c
 * Scope: reading and validating the arch table, and -- mfat_rewrite --
 * splitting a fat file into its slices, handing each to a caller's function,
 * and laying the results out again. That reassembly used to live inside
 * src/rewrite.c's mr_process_fat; it moved here when src/edit.c became a
 * second caller, so the layout rule has one implementation.
```

After `mfat_get`'s declaration:

```c
/* One slice, split out of the container as its own buffer. The function may
 * change it, grow it (reallocating *pbuf and updating *psize), or leave it
 * alone; it sets *changed when the slice is different. Returns 0, or a
 * POSITIVE failure code that mfat_rewrite returns unchanged -- positive so
 * it can never be mistaken for mfat_rewrite's own negative codes below. */
typedef int (*mfat_slice_fn)(uint8_t **pbuf, size_t *psize, const mfat_arch *a,
                             uint32_t index, int *changed, void *ctx);

/* Called once per slice, in arch-table order, after the new layout is fixed:
 * `a` is the slice as the input described it, new_offset/new_size where it
 * now sits. Not called when nothing changed (no new layout is made). */
typedef void (*mfat_placed_fn)(const mfat_arch *a, uint32_t index,
                               uint64_t new_offset, uint64_t new_size, void *ctx);

/* Split the fat file in *pbuf -- already validated by mfat_parse, which gave
 * `narch` and `swapped` -- into one buffer per slice, call `fn` on each in
 * arch-table order, and, if any changed, lay them out again: every slice
 * keeps its original offset until an earlier one's size changed; from then on
 * each packs after the previous at its own alignment. `placed` (may be NULL)
 * is then told where each landed, and *pbuf/*psize become the new file.
 *
 * Returns 0 with *modified set if any slice changed -- or clear, with *pbuf
 * untouched, if none did. Returns fn's positive code if it failed; or
 * MFAT_IO_ERROR if an allocation failed; or MFAT_MALFORMED if the new layout
 * would overlap two slices (possible when the arch table is not in ascending
 * offset order). On every non-zero return *pbuf and *psize are untouched,
 * every split buffer is freed, and the reason is on stderr. */
int mfat_rewrite(uint8_t **pbuf, size_t *psize, uint32_t narch, int swapped,
                 mfat_slice_fn fn, mfat_placed_fn placed, void *ctx, int *modified);
```

- [ ] **Step 4: Move the loop and the layout into `src/fat.c`**

Add `#include <stdio.h>`, `<stdlib.h>`, `<string.h>` and `<mach-o/fat.h>` if absent. Then add `mfat_rewrite`. This is `mr_process_fat`'s loop and reassembly, moved: the per-slice copies, the layout loop with its `shift`/`cursor`/`max_end` and its long comment about sizing from `max_end`, the pairwise overlap check with its comment and message, and the header write. Keep each comment and each stderr message's text as it is in `src/rewrite.c`, changing only what the move makes false (for example "mr_apply_file would then write" becomes "the caller would then write").

```c
int mfat_rewrite(uint8_t **pbuf, size_t *psize, uint32_t narch, int swapped,
                 mfat_slice_fn fn, mfat_placed_fn placed, void *ctx, int *modified) {
    *modified = 0;
    const uint8_t *buf = *pbuf;
    size_t n = narch ? narch : 1;
    uint8_t   **sbuf  = (uint8_t **)calloc(n, sizeof *sbuf);
    size_t     *ssize = (size_t *)calloc(n, sizeof *ssize);
    mfat_arch  *arch  = (mfat_arch *)calloc(n, sizeof *arch);
    uint64_t   *noff  = (uint64_t *)calloc(n, sizeof *noff);
    uint8_t    *newbuf = NULL;
    uint32_t    held = 0;   /* how many sbuf[] entries are allocated */
    int rc = 0;

    if (!sbuf || !ssize || !arch || !noff) {
        fprintf(stderr, "ERROR: out of memory\n");
        rc = MFAT_IO_ERROR;
        goto out;
    }
    for (uint32_t i = 0; i < narch; i++) {
        mfat_get(buf, swapped, i, &arch[i]);
        sbuf[i] = (uint8_t *)malloc(arch[i].size ? arch[i].size : 1);
        if (!sbuf[i]) { fprintf(stderr, "ERROR: out of memory\n"); rc = MFAT_IO_ERROR; goto out; }
        held = i + 1;
        memcpy(sbuf[i], buf + arch[i].offset, arch[i].size);
        ssize[i] = arch[i].size;
        int changed = 0;
        int frc = fn(&sbuf[i], &ssize[i], &arch[i], i, &changed, ctx);
        if (frc != 0) { rc = frc; goto out; }
        if (changed) *modified = 1;
    }
    if (!*modified) goto out;

    /* [the reassembly comment from rewrite.c, "Reassemble: each slice keeps
     *  its original offset until …" through "… regardless of table order."] */
    int shift = 0;
    uint64_t cursor = 0, max_end = 0;
    for (uint32_t j = 0; j < narch; j++) {
        uint64_t want;
        if (!shift) {
            want = arch[j].offset;
        } else {
            uint32_t shift_amt = arch[j].align > 31 ? 31 : arch[j].align;  /* hostile input guard */
            uint64_t a = (uint64_t)1 << shift_amt;
            want = (cursor + a - 1) & ~(a - 1);
        }
        noff[j] = want;
        cursor = want + ssize[j];
        if (cursor > max_end) max_end = cursor;
        if (ssize[j] != arch[j].size) shift = 1;
    }

    /* [the overlap comment from rewrite.c, "Refuse rather than guess: …"] */
    for (uint32_t a = 0; a < narch; a++) {
        uint64_t a0 = noff[a], a1 = a0 + ssize[a];
        for (uint32_t b = a + 1; b < narch; b++) {
            uint64_t b0 = noff[b], b1 = b0 + ssize[b];
            if (a0 < b1 && b0 < a1) {
                fprintf(stderr, "ERROR: reassembly would place arch %u [%llu,%llu) and "
                                "arch %u [%llu,%llu) at overlapping offsets; refusing "
                                "rather than guess a different layout\n",
                        a, (unsigned long long)a0, (unsigned long long)a1,
                        b, (unsigned long long)b0, (unsigned long long)b1);
                rc = MFAT_MALFORMED;
                goto out;
            }
        }
    }

    newbuf = (uint8_t *)calloc(1, (size_t)max_end);
    if (!newbuf) {
        fprintf(stderr, "ERROR: out of memory reassembling the fat file\n");
        rc = MFAT_IO_ERROR;
        goto out;
    }
    struct fat_header *nfh = (struct fat_header *)newbuf;
    nfh->magic = swapped ? mfat_swap32(FAT_MAGIC) : FAT_MAGIC;
    nfh->nfat_arch = swapped ? mfat_swap32(narch) : narch;
    struct fat_arch *nar = (struct fat_arch *)(newbuf + sizeof(struct fat_header));
    for (uint32_t j = 0; j < narch; j++) {
        uint32_t o = (uint32_t)noff[j], s = (uint32_t)ssize[j];
        nar[j].cputype    = (cpu_type_t)(swapped ? mfat_swap32(arch[j].cputype) : arch[j].cputype);
        nar[j].cpusubtype = (cpu_subtype_t)(swapped ? mfat_swap32(arch[j].cpusubtype) : arch[j].cpusubtype);
        nar[j].offset = swapped ? mfat_swap32(o) : o;
        nar[j].size   = swapped ? mfat_swap32(s) : s;
        nar[j].align  = swapped ? mfat_swap32(arch[j].align) : arch[j].align;
        memcpy(newbuf + noff[j], sbuf[j], ssize[j]);
        if (placed) placed(&arch[j], j, noff[j], ssize[j], ctx);
    }
    free(*pbuf);
    *pbuf = newbuf;
    *psize = (size_t)max_end;   /* NOT cursor -- see the comment above the layout loop */

out:
    for (uint32_t j = 0; j < held; j++) free(sbuf[j]);
    free(sbuf); free(ssize); free(arch); free(noff);
    return rc;
}
```

Replace the two bracketed comment placeholders with the actual comment text from `src/rewrite.c` before committing — they are pointers to text being moved, not text to keep.

- [ ] **Step 5: `mr_process_fat` becomes a caller**

In `src/rewrite.c`, replace `mr_process_fat` — keep its preceding comment block — with:

```c
/* The verb path's slice callback: the thin rewrite, under the label and with
 * the stdout lines `macho9 dylib`/`change_dylib` have always printed for a
 * fat file. */
typedef struct {
    const mr_ops *ops;
    int *hit_dylib, *hit_rpath, *hit_strip;
} mr_fat_ctx;

static int mr_fat_slice(uint8_t **pbuf, size_t *psize, const mfat_arch *a,
                        uint32_t index, int *changed, void *ctx_) {
    mr_fat_ctx *c = (mr_fat_ctx *)ctx_;
    char label[64];
    snprintf(label, sizeof label, "arch %u (cputype 0x%x)", index, a->cputype);
    int mod = 0;
    int rc = mr_process_thin(pbuf, psize, label, c->ops, &mod,
                             c->hit_dylib, c->hit_rpath, c->hit_strip);
    if (rc == MR_SKIP) {
        printf("%s: not a 64-bit Mach-O; leaving this slice unchanged\n", label);
        return 0;
    }
    if (rc == MR_ERROR) {
        fprintf(stderr, "ERROR: %s: refusing the whole fat file -- a partial "
                        "rewrite would leave its slices inconsistent\n", label);
        /* MR_REFUSED even when the slice's MR_ERROR came from an allocation
         * failure inside mg_grow_header or mg_plausible: the same deliberate
         * fold as the thin path's, whose comment at its own
         * MR_ERROR->MR_REFUSED translation (mr_apply_image) says why. */
        return MR_REFUSED;
    }
    *changed = mod;
    return 0;
}

static void mr_fat_placed(const mfat_arch *a, uint32_t index,
                          uint64_t off, uint64_t size, void *ctx) {
    (void)a; (void)ctx;
    printf("arch %u: placed at %llu (%llu bytes)\n", index,
           (unsigned long long)off, (unsigned long long)size);
}

static int mr_process_fat(uint8_t **pbuf, size_t *pfsize,
                          const mr_ops *ops, int *out_modified,
                          int *hit_dylib, int *hit_rpath, int *hit_strip) {
    *out_modified = 0;
    /* [keep the existing comment block about mfat_parse being the one place
     *  both this rewriter and fix_macho validate a fat file's arch table,
     *  and the one about this function's return values] */
    uint32_t narch; int swap;
    int fp_rc = mfat_parse(*pbuf, *pfsize, &narch, &swap);
    if (fp_rc == MFAT_IO_ERROR) {
        fprintf(stderr, "ERROR: out of memory validating the fat arch table\n");
        return MR_FAIL;
    }
    if (fp_rc != 0) {
        fprintf(stderr, "ERROR: malformed fat file (bad magic, arch table past the end, "
                        "a slice overlapping the header, or two slices overlapping "
                        "each other)\n");
        return MR_REFUSED;
    }
    mr_fat_ctx ctx = { ops, hit_dylib, hit_rpath, hit_strip };
    int rc = mfat_rewrite(pbuf, pfsize, narch, swap, mr_fat_slice, mr_fat_placed,
                          &ctx, out_modified);
    if (rc == MFAT_IO_ERROR) return MR_FAIL;
    if (rc == MFAT_MALFORMED) return MR_REFUSED;
    if (rc != 0) return rc;
    if (!*out_modified) printf("Nothing to change.\n");
    return 0;
}
```

Replace the bracketed comment placeholder with the comments it names, from the old function. `mr_swap32` may become unused in `src/rewrite.c`; if the compiler says so, delete it.

- [ ] **Step 6: Run the tests and watch them pass**

```bash
cmake --build $B && ctest --test-dir $B -R "fat_test|change_dylib_test|wrapper_test|known_callers" --output-on-failure
```

Expected: all pass, no warnings.

- [ ] **Step 7: Compare old and new on fat inputs**

```bash
BASE=$(git rev-parse HEAD)
S=${TMPDIR:-/tmp}/fatcmp.$$; mkdir -p "$S"
git worktree add "$S/old" "$BASE"
cmake -S "$S/old" -B "$S/old-build" >/dev/null && cmake --build "$S/old-build" >/dev/null
printf '\316\372\355\376\007\000\000\000\003\000\000\000' >"$S/stub32"   # MH_MAGIC, CPU_TYPE_I386
dd if=/dev/zero bs=1 count=4084 2>/dev/null >>"$S/stub32"
"$B/makefat" "$S/fat" tests/fixture.macho 0x1000007 3 12 tests/fixture.macho 0x1000007 3 12
"$B/makefat" "$S/fat32" tests/fixture.macho 0x1000007 3 12 "$S/stub32" 7 3 12
long="@loader_path/$(printf '%3000s' '' | tr ' ' y).dylib"
cmp_run() {   # cmp_run NAME FIXTURE ARGS... (FILE is the literal word f)
    name=$1; fx=$2; shift 2
    for side in old new; do
        if [ $side = old ]; then d="$S/old-build"; else d="$B"; fi
        cp "$fx" "$S/f"
        ( cd "$S" && "$d/$@" ) >"$S/$side.out" 2>"$S/$side.err"; echo $? >"$S/$side.rc"
        shasum -a 256 <"$S/f" >"$S/$side.sha"
    done
    for k in out err rc sha; do cmp -s "$S/old.$k" "$S/new.$k" || echo "DIFF $name: $k"; done
}
cmp_run dylib-append       "$S/fat"   macho9 dylib f -append @loader_path/x.dylib
cmp_run dylib-grow         "$S/fat"   macho9 dylib f --allow-grow -append "$long"
cmp_run dylib-nogrow       "$S/fat"   macho9 dylib f -append "$long"
cmp_run lc-delete          "$S/fat"   macho9 lc f -delete uuid
cmp_run nothing            "$S/fat"   macho9 dylib f -delete @loader_path/none.dylib
cmp_run with-32bit         "$S/fat32" macho9 lc f -delete uuid
cmp_run cd-grow            "$S/fat"   change_dylib f -grow -add "$long"
git worktree remove --force "$S/old"
```

Expected: no `DIFF` line — stdout, stderr, exit code and resulting bytes all identical. (`dylib-nogrow`'s stderr is the same on both sides here: the no-room message changed in the allow-grow plan, not in this one. If that plan has already landed, both sides carry the new wording.) Show the output in the task report.

- [ ] **Step 8: Commit**

```bash
ctest --test-dir $B && sh tests/characterize.sh $B check
git add src/fat.h src/fat.c src/rewrite.c tests/fat_test.c
git commit -m "refactor: mfat_rewrite owns splitting and reassembling a fat file"
```

---

### Task 4: The `arch` directive

**Files:**
- Modify: `src/script.h` — `ms_script.arch_mask`
- Modify: `src/script.c` — parse the directive
- Modify: `tests/script_test.c`

**Interfaces:**
- Consumes: `ma_lookup`, `ma_list` from Task 1.
- Produces: `unsigned ms_script.arch_mask` — bit `r` set when an `arch` directive named row `r` of the arch table; 0 when the script has none. Task 5 reads it.

- [ ] **Step 1: Write the failing tests**

Append to `tests/script_test.c` and add each to `main`:

```c
static void test_arch_directive_names_rows(void) {
    static const char src[] = "arch x86_64\narch arm64\narch x86_64\nload-command delete uuid\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == 0, "arch: parses (%s)", err);
    CHECK(s.arch_mask == ((1u << ma_lookup("x86_64")) | (1u << ma_lookup("arm64"))),
          "arch: the mask names x86_64 and arm64, a repeat harmlessly (got 0x%x)", s.arch_mask);
    CHECK(s.n == 1, "arch: directives are not statements (got %d)", s.n);
    ms_free(&s);
}

static void test_no_arch_directive_is_an_empty_mask(void) {
    static const char src[] = "load-command delete uuid\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == 0, "no arch: parses (%s)", err);
    CHECK(s.arch_mask == 0, "no arch: the mask is empty (got 0x%x)", s.arch_mask);
    ms_free(&s);
}

static void test_arch_directive_errors(void) {
    ms_script s; char err[256];
    static const char bad1[] = "arch amd64\n";
    err[0] = 0;
    CHECK(ms_parse(bad1, sizeof bad1 - 1, &s, err, sizeof err) == -1, "arch amd64 is refused");
    CHECK(strstr(err, "line 1") && strstr(err, "amd64") && strstr(err, "x86_64, x86_64h"),
          "and names the line, the name, and what is accepted (got: %s)", err);
    static const char bad2[] = "arch\n";
    CHECK(ms_parse(bad2, sizeof bad2 - 1, &s, err, sizeof err) == -1, "arch with no name is refused");
    static const char bad3[] = "arch x86_64 arm64\n";
    CHECK(ms_parse(bad3, sizeof bad3 - 1, &s, err, sizeof err) == -1, "arch with two names is refused");
    static const char bad4[] = "load-command delete uuid\narch x86_64\n";
    CHECK(ms_parse(bad4, sizeof bad4 - 1, &s, err, sizeof err) == -1,
          "arch after an operation is refused");
    CHECK(strstr(err, "line 2") != NULL, "and names line 2 (got: %s)", err);
}
```

Add `#include "arch_names.h"` to `tests/script_test.c`.

- [ ] **Step 2: Run and watch it fail**

```bash
cmake --build $B 2>&1 | grep -m2 "arch_mask"
```

Expected: `ms_script` has no member `arch_mask`.

- [ ] **Step 3: Add the field**

In `src/script.h`'s `ms_script`, after `fatal_warnings`:

```c
    unsigned arch_mask;   /* bit r set when an `arch` directive named row r of
                           * src/arch_names.h's table; 0 when the script names
                           * no arch, which means every 64-bit slice */
```

Where the header documents the directives, add `arch NAME` alongside the other two: one operand, checked against the arch table, repeatable, and like every directive it must precede every operation.

- [ ] **Step 4: Parse it**

In `src/script.c`, add `#include "arch_names.h"`, declare `unsigned arch_mask = 0;` beside `allow_grow`/`fatal_warnings`, and immediately before the existing `allow-grow`/`fatal-warnings` directive block:

```c
        if (strcmp(fields[0], "arch") == 0) {
            if (n != 2)
                return ms_failf(stmts, text, out, err, errsz, lineno,
                    "directive 'arch' takes exactly one operand, an arch name");
            if (seen_operation)
                return ms_failf(stmts, text, out, err, errsz, lineno,
                    "directive 'arch' must precede every operation");
            int row = ma_lookup(fields[1]);
            if (row < 0) {
                char names[128];
                ma_list(names, sizeof names);
                return ms_failf(stmts, text, out, err, errsz, lineno,
                    "unknown arch '%s' (expected one of: %s)", fields[1], names);
            }
            arch_mask |= 1u << row;
            continue;
        }
```

and, where `out->allow_grow` and `out->fatal_warnings` are set at the end, `out->arch_mask = arch_mask;`.

- [ ] **Step 5: Run the tests and watch them pass**

```bash
cmake --build $B && ctest --test-dir $B -R script_test --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
ctest --test-dir $B
git add src/script.h src/script.c tests/script_test.c
git commit -m "feat: the arch directive names the slices a script applies to"
```

---

### Task 5: `edit` runs a script on every selected slice

**Files:**
- Modify: `src/edit.c` — the fat path, the shared statement loop, the verdict across slices, the shared write
- Modify: `tests/edit_test.c` — a `NO_UUID` builder flag, a fat builder, eight tests; the old "thin only" test loses its fat half

**Interfaces:**
- Consumes: `mfat_rewrite` and its callback types (Task 3); `ma_index`, `ma_describe`, `ma_row` (Task 1); `ms_script.arch_mask` (Task 4).
- Produces: `me_run` accepts `FAT_MAGIC`/`FAT_CIGAM` containers. No public signature changes.

- [ ] **Step 1: Write the failing tests**

In `tests/edit_test.c`:

1. Beside `IMPLAUSIBLE` and `DYLD_INFO`, add `#define NO_UUID 4   /* leave out LC_UUID */`, and in `build_image` wrap the LC_UUID block in `if (!(flags & NO_UUID)) { … }`.
2. After `build_image`, a fat builder and a slice reader:

```c
/* A fat container of n slices, each at a 0x1000-aligned offset in the order
 * given, big-endian as every real fat file is. ct/cs label the fat_arch
 * entry; edit names a slice by its fat_arch entry, so an x86_64 image can
 * stand in for arm64 without its own header saying so. */
static uint8_t *build_fat(int n, uint8_t *const *slice, const size_t *len,
                          const uint32_t *ct, const uint32_t *cs, size_t *outlen) {
    size_t off[8], total = 0x1000;
    for (int i = 0; i < n; i++) { off[i] = total; total += (len[i] + 0xfff) & ~(size_t)0xfff; }
    uint8_t *buf = (uint8_t *)calloc(1, total);
    struct fat_header *fh = (struct fat_header *)buf;
    fh->magic = OSSwapHostToBigInt32(FAT_MAGIC);
    fh->nfat_arch = OSSwapHostToBigInt32((uint32_t)n);
    struct fat_arch *fa = (struct fat_arch *)(fh + 1);
    for (int i = 0; i < n; i++) {
        fa[i].cputype = (cpu_type_t)OSSwapHostToBigInt32(ct[i]);
        fa[i].cpusubtype = (cpu_subtype_t)OSSwapHostToBigInt32(cs[i]);
        fa[i].offset = OSSwapHostToBigInt32((uint32_t)off[i]);
        fa[i].size = OSSwapHostToBigInt32((uint32_t)len[i]);
        fa[i].align = OSSwapHostToBigInt32(12);
        memcpy(buf + off[i], slice[i], len[i]);
    }
    *outlen = total;
    return buf;
}

/* A 32-bit slice: just enough header to be one. */
static uint8_t *build_i386_stub(size_t *len) {
    *len = 0x1000;
    uint8_t *b = (uint8_t *)calloc(1, *len);
    struct mach_header *h = (struct mach_header *)b;
    h->magic = MH_MAGIC; h->cputype = CPU_TYPE_I386; h->cpusubtype = CPU_SUBTYPE_I386_ALL;
    h->filetype = MH_DYLIB;
    return b;
}

/* Copy slice `idx` of the fat file at `path` into its own file at `out`. */
static void slice_to_file(const char *path, int idx, const char *out) {
    size_t len;
    uint8_t *b = read_file(path, &len);
    const struct fat_arch *fa = (const struct fat_arch *)(b + sizeof(struct fat_header));
    uint32_t off = OSSwapBigToHostInt32(fa[idx].offset), sz = OSSwapBigToHostInt32(fa[idx].size);
    write_file(out, b + off, sz, 0644);
    free(b);
}

/* The standard two- or three-slice fixture: x86_64, arm64 (the same image,
 * relabelled), and optionally an i386 stub. flags0/flags1 go to build_image. */
static void write_fat(const char *path, int flags0, int flags1, int with_i386) {
    uint8_t *s[3]; size_t l[3];
    uint32_t ct[3] = { (uint32_t)CPU_TYPE_X86_64, (uint32_t)CPU_TYPE_ARM64, (uint32_t)CPU_TYPE_I386 };
    uint32_t cs[3] = { (uint32_t)CPU_SUBTYPE_X86_64_ALL, (uint32_t)CPU_SUBTYPE_ARM64_ALL,
                       (uint32_t)CPU_SUBTYPE_I386_ALL };
    s[0] = build_image(flags0); l[0] = IMG_SIZE;
    s[1] = build_image(flags1); l[1] = IMG_SIZE;
    int n = 2;
    if (with_i386) { s[2] = build_i386_stub(&l[2]); n = 3; }
    size_t flen;
    uint8_t *fat = build_fat(n, s, l, ct, cs, &flen);
    write_file(path, fat, flen, 0755);
    for (int i = 0; i < n; i++) free(s[i]);
    free(fat);
}
```

3. Change `test_only_a_thin_image_is_accepted`: rename it `test_what_edit_accepts`; replace its one-slice fat case (the block from "A one-slice fat container" through the `strstr(g_log, "fat")` check) with a `FAT_MAGIC_64` case — a header with magic `OSSwapHostToBigInt32(FAT_MAGIC_64)` and `nfat_arch` 0, written to a file; `run` returns `MR_REFUSED`, `check_untouched` holds, and the log contains `64-bit fat`. Keep its non-Mach-O and absent cases unchanged. Update `main`.
4. Add these tests and call them from `main`:

```c
static void test_fat_every_64bit_slice_by_default(void) {
    fresh_dir();
    char path[512], s0[512], s1[512], s2[512];
    in_dir(path, sizeof path, "fat"); in_dir(s0, sizeof s0, "s0");
    in_dir(s1, sizeof s1, "s1"); in_dir(s2, sizeof s2, "s2");
    write_fat(path, 0, 0, 1);
    size_t stub_len; uint8_t *stub = build_i386_stub(&stub_len);
    int rc = run(path, NULL, "load-command delete uuid\n", 0, 0);
    CHECK(rc == 0, "fat, no arch: succeeds (got %d; log: %s)", rc, g_log);
    slice_to_file(path, 0, s0); slice_to_file(path, 1, s1); slice_to_file(path, 2, s2);
    CHECK(count_lc(s0, LC_UUID, NULL) == 0, "fat, no arch: the x86_64 slice lost LC_UUID");
    CHECK(count_lc(s1, LC_UUID, NULL) == 0, "fat, no arch: the arm64 slice lost LC_UUID");
    size_t l2; uint8_t *b2 = read_file(s2, &l2);
    CHECK(l2 == stub_len && memcmp(b2, stub, l2) == 0, "fat, no arch: the i386 slice is byte-identical");
    free(b2); free(stub);
}

static void test_fat_arch_selects_named_slices(void) {
    fresh_dir();
    char path[512], s0[512], s1[512], orig1[512];
    in_dir(path, sizeof path, "fat"); in_dir(s0, sizeof s0, "s0");
    in_dir(s1, sizeof s1, "s1"); in_dir(orig1, sizeof orig1, "orig1");
    write_fat(path, 0, 0, 0);
    slice_to_file(path, 1, orig1);
    int rc = run(path, NULL, "arch x86_64\nload-command delete uuid\n", 0, 0);
    CHECK(rc == 0, "fat, arch x86_64: succeeds (got %d; log: %s)", rc, g_log);
    slice_to_file(path, 0, s0); slice_to_file(path, 1, s1);
    CHECK(count_lc(s0, LC_UUID, NULL) == 0, "fat, arch x86_64: the named slice was edited");
    size_t la, lb; uint8_t *a = read_file(orig1, &la), *b = read_file(s1, &lb);
    CHECK(la == lb && memcmp(a, b, la) == 0, "fat, arch x86_64: the arm64 slice is byte-identical");
    free(a); free(b);
}

static void test_arch_on_a_thin_file(void) {
    fresh_dir();
    char path[512];
    in_dir(path, sizeof path, "thin");
    uint8_t *img = build_image(0);
    write_file(path, img, IMG_SIZE, 0755);
    free(img);
    int rc = run(path, NULL, "arch x86_64\nload-command delete uuid\n", 0, 0);
    CHECK(rc == 0, "thin, arch x86_64: runs on an x86_64 image (got %d; log: %s)", rc, g_log);
    img = build_image(0);
    write_file(path, img, IMG_SIZE, 0755);
    free(img);
    snap before = take(path);
    rc = run(path, NULL, "arch arm64\nload-command delete uuid\n", 0, 0);
    CHECK(rc == MR_REFUSED, "thin, arch arm64: an x86_64 image is refused (got %d)", rc);
    check_untouched("thin, arch arm64", path, &before);
    CHECK(strstr(g_log, "x86_64") != NULL, "thin, arch arm64: the refusal names the image's arch (log: %s)", g_log);
}

static void test_fat_missing_or_32bit_arch_is_refused(void) {
    fresh_dir();
    char path[512];
    in_dir(path, sizeof path, "fat");
    write_fat(path, 0, 0, 1);
    snap before = take(path);
    int rc = run(path, NULL, "arch arm64e\nload-command delete uuid\n", 0, 0);
    CHECK(rc == MR_REFUSED, "fat, arch arm64e: a slice the file lacks is refused (got %d)", rc);
    check_untouched("fat, missing arch", path, &before);
    CHECK(strstr(g_log, "x86_64, arm64, i386") != NULL,
          "fat, missing arch: the refusal lists the file's slices (log: %s)", g_log);
    rc = run(path, NULL, "arch i386\nload-command delete uuid\n", 0, 0);
    CHECK(rc == MR_REFUSED, "fat, arch i386: naming a 32-bit slice is refused (got %d)", rc);
    check_untouched("fat, 32-bit arch", path, &before);
    CHECK(strstr(g_log, "32-bit") != NULL, "fat, arch i386: the refusal says 32-bit (log: %s)", g_log);
}

static void test_fat_fatal_warnings_counts_a_match_in_any_slice(void) {
    fresh_dir();
    char path[512];
    in_dir(path, sizeof path, "fat");
    write_fat(path, 0, NO_UUID, 0);   /* only the x86_64 slice has LC_UUID */
    int rc = run(path, NULL, "fatal-warnings\nload-command delete uuid\n", 0, 0);
    CHECK(rc == 0, "fat, fatal-warnings: a match in one slice is not a miss (got %d; log: %s)", rc, g_log);
    write_fat(path, NO_UUID, NO_UUID, 0);   /* neither has it */
    snap before = take(path);
    rc = run(path, NULL, "fatal-warnings\nload-command delete uuid\n", 0, 0);
    CHECK(rc == MR_REFUSED, "fat, fatal-warnings: matching in no slice refuses (got %d)", rc);
    check_untouched("fat, miss everywhere", path, &before);
    CHECK(strstr(g_log, "matched nothing in any selected slice") != NULL,
          "fat, fatal-warnings: the refusal says it matched in no slice (log: %s)", g_log);
}

static void test_fat_a_refusal_in_the_second_slice_writes_nothing(void) {
    fresh_dir();
    char path[512];
    in_dir(path, sizeof path, "fat");
    write_fat(path, 0, IMPLAUSIBLE, 0);   /* slice 1 fails its final verification */
    snap before = take(path);
    int rc = run(path, NULL, "load-command delete uuid\n", 0, 0);
    CHECK(rc == MR_REFUSED, "fat: the second slice's verification refuses the run (got %d)", rc);
    check_untouched("fat, second slice refused", path, &before);
    CHECK(strstr(g_log, "slice arm64") != NULL, "fat: the refusal names the slice (log: %s)", g_log);
}

static void test_fat_verbose_accounts_for_every_slice(void) {
    fresh_dir();
    char path[512];
    in_dir(path, sizeof path, "fat");
    write_fat(path, 0, 0, 1);
    int rc = run(path, NULL, "arch x86_64\nload-command delete uuid\n", 1, 0);
    CHECK(rc == 0, "fat, verbose: succeeds (got %d)", rc);
    CHECK(strstr(g_log, "slice x86_64:\n") != NULL, "fat, verbose: the edited slice's header (log: %s)", g_log);
    CHECK(strstr(g_log, "slice x86_64: verified") != NULL, "fat, verbose: the edited slice verified");
    CHECK(strstr(g_log, "slice arm64: not selected by arch; passed through unchanged") != NULL,
          "fat, verbose: the unselected slice is accounted for (log: %s)", g_log);
    CHECK(strstr(g_log, "slice i386: 32-bit; passed through unchanged") != NULL,
          "fat, verbose: the 32-bit slice is accounted for (log: %s)", g_log);
}

static void test_fat_dry_run_writes_nothing(void) {
    fresh_dir();
    char path[512];
    in_dir(path, sizeof path, "fat");
    write_fat(path, 0, 0, 0);
    snap before = take(path);
    int rc = run(path, NULL, "load-command delete uuid\n", 0, 1);
    CHECK(rc == 0, "fat, dry run: succeeds (got %d)", rc);
    check_untouched("fat, dry run", path, &before);
    CHECK(strstr(g_log, "NOT written") != NULL, "fat, dry run: says it did not write (log: %s)", g_log);
}
```

`count_lc(path, cmd, name)`'s third argument is the segment/section name filter the existing helper takes; pass `NULL` as its other callers do for a plain command count — check its definition (`tests/edit_test.c:273`) and adapt the call if its contract differs.

- [ ] **Step 2: Run and watch them fail**

```bash
cmake --build $B && ctest --test-dir $B -R edit_test --output-on-failure 2>&1 | grep FAIL | head
```

Expected: every fat test fails (a fat input is refused today).

- [ ] **Step 3: The verdict across slices, and the shared statement loop**

In `src/edit.c`:

1. Add `#include "fat.h"` and `#include "arch_names.h"`.
2. After `me_view`, add:

```c
/* What one statement's "matched nothing" verdict needs across the slices of
 * a run: counts that every slice running the statement ADDS to (mr_hits'
 * own contract, rewrite.h), and whether this is the last selected slice --
 * the one that decides. On a thin file the one image is the last, so the
 * verdict comes right after the statement, as it always has. */
typedef struct {
    mr_hits *hits;      /* this statement's counts, summed across slices */
    int     *renamed;   /* this statement's segment-rename count, likewise */
    int      decide;    /* nonzero in the last selected slice */
    int      missed;    /* set when the verdict refused: it matched nothing */
} me_verdict;
```

3. `me_rewrite` accumulates, and decides only when told to:

```c
static int me_rewrite(uint8_t **pbuf, size_t *psize, const char *path,
                      const mr_ops *ops, me_verdict *v) {
    int modified = 0;
    int rc = mr_apply_image(pbuf, psize, path, ops, &modified, v->hits);
    if (rc != 0 || !v->decide) return rc;
    /* The verdict's "matched nothing" report goes to stderr; flushed first
     * for the reason me_say flushes. */
    fflush(stdout);
    rc = mr_unmatched_verdict(ops, v->hits);
    if (rc != 0) v->missed = 1;
    return rc;
}
```

Rewrite its preceding comment to match: the counts are per statement and summed across slices; the verdict is taken in the last selected slice.

4. `me_apply` gains a last parameter `me_verdict *v`; every `me_rewrite(pbuf, psize, path, &ops)` call becomes `me_rewrite(pbuf, psize, path, &ops, v)`. The `MS_SEGMENT` arm's tail becomes:

```c
        int renamed = 0;
        ops.segment_rename_old = st->a;
        ops.segment_rename_new = st->b;
        ops.segment_renamed = &renamed;
        int rc = me_rewrite(pbuf, psize, path, &ops, v);
        if (rc != 0) return rc;
        *v->renamed += renamed;
        if (!v->decide || *v->renamed > 0) return 0;
        me_say(stderr, "macho9: segment %s matched nothing\n", st->a);
        if (s->fatal_warnings) { v->missed = 1; return MR_REFUSED; }
        return 0;
```

and its comment says the count is summed across slices and judged in the last one.

5. Replace the statement loop in `me_run` (from `for (int i = 0; i < s->n; i++) {` through its closing brace) with a call to this new function, placed before `me_run`:

```c
/* Every statement, in order, against one image -- a thin file's, or one fat
 * slice's. `slice` is NULL for a thin file, else the slice's arch name, for
 * the refusal line. hits/renamed hold one entry per statement, shared by
 * every slice of the run; `decide` says whether this is the last selected
 * slice. Returns 0, or the first failing statement's code after printing the
 * refusal line. */
static int me_statements(uint8_t **pbuf, size_t *psize, const char *path, const char *out,
                         const ms_script *s, FILE *log, int verbose,
                         mr_hits *hits, int *renamed, int decide, const char *slice) {
    for (int i = 0; i < s->n; i++) {
        const ms_stmt *stmt = &s->stmts[i];
        if (verbose) me_log_stmt(log, stmt);
        me_verdict v = { &hits[i], &renamed[i], decide, 0 };
        int rc = me_apply(pbuf, psize, path, s, stmt, log, verbose, &v);
        if (rc != 0) {
            if (rc != MR_REFUSED) rc = MR_FAIL;
            me_say(log, "macho9 edit: %s at statement %d of %d (line %d)",
                   rc == MR_REFUSED ? "refused" : "failed", i + 1, s->n, stmt->line);
            if (slice && v.missed) me_say(log, ": it matched nothing in any selected slice");
            else if (slice)        me_say(log, " in slice %s", slice);
            me_say(log, "; ");
            me_say_left(log, path, out);
            return rc;
        }
    }
    return 0;
}
```

In `me_run`'s thin path, allocate the per-statement arrays before the call and free them on every exit:

```c
    size_t nst = s->n ? (size_t)s->n : 1;
    mr_hits *hits = (mr_hits *)calloc(nst, sizeof *hits);
    int *renamed = (int *)calloc(nst, sizeof *renamed);
    if (!hits || !renamed) {
        free(hits); free(renamed); free(buf);
        me_say(log, "macho9 edit: out of memory\n");
        return MR_FAIL;
    }
    int rc = me_statements(&buf, &size, path, out, s, log, verbose, hits, renamed, 1, NULL);
    free(hits); free(renamed);
    if (rc != 0) { free(buf); return rc; }
```

A thin run's refusal line is unchanged: `slice` is NULL.

- [ ] **Step 4: One write path for both**

Move `me_run`'s tail — from `me_commas(bytes, size);` through the end — into a function both paths call:

```c
/* The last step of a run that verified: report, and write once unless this
 * is a dry run. Takes ownership of buf. */
static int me_write_once(uint8_t *buf, size_t size, mode_t mode, const char *path,
                         const char *out, FILE *log, int verbose, int dry_run) {
    const char *dest = out ? out : path;
    char bytes[32];
    me_commas(bytes, size);
    if (dry_run) {
        me_say(log, "%s: NOT written (--dry-run) -- would be %s bytes\n", dest, bytes);
        free(buf);
        return 0;
    }
    if (wa_write_atomic(dest, mode, buf, size) != 0) {
        /* [keep the existing comment] */
        if (out)
            me_say(log, "macho9 edit: writing %s failed; %s left unmodified\n", out, path);
        else
            me_say(log, "macho9 edit: %s left unmodified (write failed)\n", path);
        free(buf);
        return MR_FAIL;
    }
    if (verbose) me_say(log, "%s: written (%s bytes)\n", dest, bytes);
    free(buf);
    return 0;
}
```

The thin path ends `return me_write_once(buf, size, st.st_mode, path, out, log, verbose, dry_run);` after its existing verification. Remove the now-unused `dest`/`bytes` locals from `me_run` if nothing else uses them.

- [ ] **Step 5: The fat path**

Before `me_run`:

```c
/* The first four bytes of `path`, or 0 if they cannot be read. */
static uint32_t me_magic(const char *path) {
    uint32_t magic = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    if (read(fd, &magic, sizeof magic) != (ssize_t)sizeof magic) magic = 0;
    close(fd);
    return magic;
}

/* Everything the fat path's slice callbacks need. */
typedef struct {
    const ms_script *s;
    const char *path, *out;
    FILE *log;
    int verbose;
    const unsigned char *selected;   /* per slice: does the script apply to it? */
    uint32_t last;                   /* the last selected slice, in arch-table order */
    mr_hits *hits;
    int *renamed;
} me_fat_ctx;

static int me_fat_slice(uint8_t **pbuf, size_t *psize, const mfat_arch *a,
                        uint32_t index, int *changed, void *ctx_) {
    me_fat_ctx *c = (me_fat_ctx *)ctx_;
    char name[32];
    ma_describe(a->cputype, a->cpusubtype, name);
    if (!c->selected[index]) {
        if (c->verbose)
            me_say(c->log, "slice %s: %s; passed through unchanged\n", name,
                   (a->cputype & CPU_ARCH_ABI64) ? "not selected by arch" : "32-bit");
        return 0;
    }
    if (c->verbose) me_say(c->log, "slice %s:\n", name);
    int rc = me_statements(pbuf, psize, c->path, c->out, c->s, c->log, c->verbose,
                           c->hits, c->renamed, index == c->last, name);
    if (rc != 0) return rc;
    /* Each slice's own final verification: always, and never subject to
     * MACHO_NO_VERIFY, exactly as a thin file's. */
    if (mg_plausible(*pbuf, *psize) != 0) {
        me_say(c->log, "macho9 edit: refused at verification of slice %s; ", name);
        me_say_left(c->log, c->path, c->out);
        return MR_REFUSED;
    }
    if (c->verbose) me_say(c->log, "slice %s: verified\n", name);
    *changed = 1;
    return 0;
}

/* After the new layout is fixed: a slice that moved says so. Its bytes are
 * unchanged if nothing selected it, but where it lives is not. */
static void me_fat_placed(const mfat_arch *a, uint32_t index,
                          uint64_t off, uint64_t size, void *ctx_) {
    me_fat_ctx *c = (me_fat_ctx *)ctx_;
    (void)index; (void)size;
    if (!c->verbose || off == a->offset) return;
    char name[32];
    ma_describe(a->cputype, a->cpusubtype, name);
    me_say(c->log, "slice %s: moved from offset 0x%llx to 0x%llx\n", name,
           (unsigned long long)a->offset, (unsigned long long)off);
}

static int me_run_fat(const char *path, const char *out, const ms_script *s,
                      FILE *log, int verbose, int dry_run) {
    /* Read the whole container once. */
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        if (fd >= 0) close(fd);
        me_say(log, "macho9 edit: %s: cannot open or read\n", path);
        return MR_FAIL;
    }
    size_t size = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(size ? size : 1);
    if (!buf || read(fd, buf, size) != (ssize_t)size) {
        close(fd); free(buf);
        me_say(log, "macho9 edit: %s: cannot open or read\n", path);
        return MR_FAIL;
    }
    close(fd);

    uint32_t narch; int swap;
    int prc = mfat_parse(buf, size, &narch, &swap);
    if (prc != 0) {
        free(buf);
        if (prc == MFAT_IO_ERROR) {
            me_say(log, "macho9 edit: out of memory reading %s's arch table\n", path);
            return MR_FAIL;
        }
        me_say(log, "macho9 edit: %s: malformed fat file; ", path);
        me_say_left(log, path, out);
        return MR_REFUSED;
    }

    /* Which slices the script applies to, and whether it names any the file
     * lacks or cannot edit -- all decided before any slice is touched. */
    unsigned char *selected = (unsigned char *)calloc(narch ? narch : 1, 1);
    size_t nst = s->n ? (size_t)s->n : 1;
    mr_hits *hits = (mr_hits *)calloc(nst, sizeof *hits);
    int *renamed = (int *)calloc(nst, sizeof *renamed);
    if (!selected || !hits || !renamed) {
        free(selected); free(hits); free(renamed); free(buf);
        me_say(log, "macho9 edit: out of memory\n");
        return MR_FAIL;
    }
    char have[256] = "";
    int nselected = 0; uint32_t last = 0;
    for (uint32_t j = 0; j < narch; j++) {
        mfat_arch a; mfat_get(buf, swap, j, &a);
        char name[32]; ma_describe(a.cputype, a.cpusubtype, name);
        size_t hl = strlen(have);
        snprintf(have + hl, sizeof have - hl, "%s%s", j ? ", " : "", name);
        int row = ma_index(a.cputype, a.cpusubtype);
        int is64 = (a.cputype & CPU_ARCH_ABI64) != 0;
        selected[j] = s->arch_mask ? (row >= 0 && (s->arch_mask & (1u << row)))
                                   : is64;
        if (selected[j]) { nselected++; last = j; }
    }
    int rc = 0;
    const char *rname; uint32_t rct, rcs;
    for (int r = 0; rc == 0 && ma_row(r, &rname, &rct, &rcs); r++) {
        if (!(s->arch_mask & (1u << r))) continue;
        int found = 0, found64 = 0;
        for (uint32_t j = 0; j < narch; j++) {
            mfat_arch a; mfat_get(buf, swap, j, &a);
            if (ma_index(a.cputype, a.cpusubtype) == r) {
                found = 1; found64 = (a.cputype & CPU_ARCH_ABI64) != 0;
            }
        }
        if (!found) {
            me_say(log, "macho9 edit: %s has no %s slice (it has: %s); ", path, rname, have);
            rc = MR_REFUSED;
        } else if (!found64) {
            me_say(log, "macho9 edit: %s's %s slice is 32-bit, and statements apply only to "
                        "64-bit slices; ", path, rname);
            rc = MR_REFUSED;
        }
    }
    if (rc == 0 && nselected == 0) {
        me_say(log, "macho9 edit: %s has no 64-bit slice to edit (it has: %s); ", path, have);
        rc = MR_REFUSED;
    }
    if (rc != 0) {
        me_say_left(log, path, out);
        free(selected); free(hits); free(renamed); free(buf);
        return rc;
    }

    me_fat_ctx ctx = { s, path, out, log, verbose, selected, last, hits, renamed };
    int modified = 0;
    rc = mfat_rewrite(&buf, &size, narch, swap, me_fat_slice, me_fat_placed, &ctx, &modified);
    free(selected); free(hits); free(renamed);
    if (rc != 0) {
        if (rc == MFAT_IO_ERROR || rc == MFAT_MALFORMED) {
            me_say(log, "macho9 edit: could not lay out %s's slices again; ", path);
            me_say_left(log, path, out);
            rc = (rc == MFAT_IO_ERROR) ? MR_FAIL : MR_REFUSED;
        }
        /* A slice's own failure already printed its refusal line. */
        free(buf);
        return rc;
    }
    /* The container itself, as it will be written. */
    if (mfat_parse(buf, size, &narch, &swap) != 0) {
        me_say(log, "macho9 edit: the reassembled %s fails validation; ", path);
        me_say_left(log, path, out);
        free(buf);
        return MR_REFUSED;
    }
    if (verbose) me_say(log, "%s: verified\n", path);
    return me_write_once(buf, size, st.st_mode, path, out, log, verbose, dry_run);
}
```

`me_say_left` prints the "… left unmodified" tail; every refusal above calls it after a message ending in `"; "`, the shape the thin path's refusals already have.

- [ ] **Step 6: `me_run` dispatches, and a thin file answers to `arch`**

At the top of `me_run`, after the locals, before the thin path reads the image:

```c
    uint32_t magic = me_magic(path);
    if (magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64) {
        me_say(log, "macho9 edit: %s is a 64-bit fat container (fat_arch_64), which this "
                    "tool does not read; ", path);
        me_say_left(log, path, out);
        return MR_REFUSED;
    }
    if (magic == FAT_MAGIC || magic == FAT_CIGAM)
        return me_run_fat(path, out, s, log, verbose, dry_run);
```

`me_refuse_input` loses its fat branch (that case never reaches it now) and keeps only "not a readable 64-bit Mach-O"; update its comment.

In the thin path, right after `mi_open` succeeds and before `mi_release`, the `arch` check:

```c
    if (s->arch_mask) {
        int row = ma_index((uint32_t)im.hdr->cputype, (uint32_t)im.hdr->cpusubtype);
        if (row < 0 || !(s->arch_mask & (1u << row))) {
            char name[32];
            ma_describe((uint32_t)im.hdr->cputype, (uint32_t)im.hdr->cpusubtype, name);
            me_say(log, "macho9 edit: %s is %s, which the script's arch directives do not "
                        "name; ", path, name);
            me_say_left(log, path, out);
            mi_close(&im);
            return MR_REFUSED;
        }
    }
```

- [ ] **Step 7: Run the tests and watch them pass**

```bash
cmake --build $B && ctest --test-dir $B -R edit_test --output-on-failure && ctest --test-dir $B
```

Expected: all pass, including every existing thin `edit_test` case unchanged; no warnings.

- [ ] **Step 8: Prove three tests can fail**

One at a time, each reverted before the next:

1. Make every slice selected (`selected[j] = is64;` regardless of `arch_mask`): `test_fat_arch_selects_named_slices` fails.
2. Decide per slice instead of across them (pass `1` for `decide` in `me_fat_slice`): `test_fat_fatal_warnings_counts_a_match_in_any_slice`'s first check fails.
3. Write before the second slice is processed (in `me_fat_slice`, when `index == 0`, call `wa_write_atomic(c->path, 0755, *pbuf, *psize)`): `test_fat_a_refusal_in_the_second_slice_writes_nothing`'s `check_untouched` fails.

Record each mutation and its output in the task report.

- [ ] **Step 9: Commit**

```bash
sh tests/characterize.sh $B check
git add src/edit.c tests/edit_test.c
git commit -m "feat: edit runs a script on every selected slice of a fat file"
```

---

### Task 6: End to end, and the docs

**Files:**
- Modify: `tests/cli_test.sh` — a fat file through the CLI, a slice that grows and one that moves, the parse error
- Modify: `src/edit.h` — replace the thin-only statements; document `arch`, the slice table, the verdict across slices, and the per-slice log lines
- Modify: `README.md` — the `edit` section's directives table, Limits, and verbose description

**Interfaces:**
- Consumes: everything above; `$BIN/makefat` and `$BIN/fatcheck` from Task 2.

- [ ] **Step 1: Write the failing test**

At the end of `tests/cli_test.sh`, before `reached_end=1`:

```sh
# edit on a fat file, end to end: two build_main executables in one
# container, the second labelled arm64 in its fat_arch entry (edit names a
# slice by that entry). allow-grow on the x86_64 slice alone grows it by a
# page, so the arm64 slice after it has to move -- the one consequence a
# passed-through slice can have, and --verbose must say so. The appended
# path is sized from the slice's own pad, not hard-coded, because each
# host's linker leaves a different pad.
build_main "$T/fat_s0"
build_main "$T/fat_s1"
"$BIN/makefat" "$T/fat_edit" "$T/fat_s0" 0x1000007 3 12 "$T/fat_s1" 0x100000c 0 12
fat_pad=$("$MACHO9" info "$T/fat_s0" | sed -n 's/^header pad: \([0-9][0-9]*\) bytes available.*/\1/p')
[ -n "$fat_pad" ] || { bad "edit fat: fixture setup" "no header pad reported"; fat_pad=0; }
fat_path="/$(printf "%${fat_pad}s" '' | tr ' ' f)"
printf 'arch x86_64\nallow-grow\ndylib append %s\n' "$fat_path" >"$T/fat.edits"
rc=0
"$MACHO9" edit --verbose "$T/fat_edit" "$T/fat.edits" >/dev/null 2>"$T/fat.err" || rc=$?
[ "$rc" -eq 0 ] && ok "edit: a fat file's x86_64 slice is edited, growing it" \
    || bad "edit fat" "expected 0, got $rc: $(cut -c1-200 "$T/fat.err")"
grep -q "slice arm64: not selected by arch; passed through unchanged" "$T/fat.err" \
    && ok "edit: --verbose accounts for the unselected arm64 slice" \
    || bad "edit fat" "no pass-through line: $(cut -c1-300 "$T/fat.err")"
grep -q "slice arm64: moved from offset" "$T/fat.err" \
    && ok "edit: --verbose says the arm64 slice moved when the x86_64 slice grew" \
    || bad "edit fat" "no moved line: $(cut -c1-300 "$T/fat.err")"
"$BIN/fatcheck" dump "$T/fat_edit" 0 "$T/fat_out0"
"$BIN/fatcheck" dump "$T/fat_edit" 1 "$T/fat_out1"
"$MACHO9" info "$T/fat_out0" | grep -qF "path=$fat_path" \
    && ok "edit: the x86_64 slice carries the appended dylib" \
    || bad "edit fat" "the appended dylib is not in slice 0"
"$MACHO9" verify "$T/fat_out0" >/dev/null 2>"$T/fat_v.err" \
    && ok "edit: the grown x86_64 slice passes macho9 verify" \
    || bad "edit fat" "verify refused slice 0: $(cat "$T/fat_v.err")"
cmp -s "$T/fat_out1" "$T/fat_s1" \
    && ok "edit: the arm64 slice is byte-identical, though it moved" \
    || bad "edit fat" "the arm64 slice changed"

printf 'arch amd64\nload-command delete uuid\n' >"$T/fat_bad.edits"
rc=0
"$MACHO9" edit "$T/fat_edit" "$T/fat_bad.edits" >/dev/null 2>"$T/fat_bad.err" || rc=$?
[ "$rc" -eq 2 ] && ok "edit: an unknown arch name is a parse error (2)" \
    || bad "edit fat" "arch amd64: expected 2, got $rc: $(cat "$T/fat_bad.err")"
```

- [ ] **Step 2: Run it**

```bash
cmake --build $B && sh tests/cli_test.sh $B 2>&1 | grep -E "edit fat|edit: .*(fat|slice|arch)"
```

Expected: every line PASSes — Task 5 built the behaviour; this is the CLI and real-linker confirmation. A FAIL here is a real defect in Tasks 3-5; fix it there, not in the test.

- [ ] **Step 3: Correct `src/edit.h`**

Every statement that `edit` takes a thin image only — the REPORT section, the refusal descriptions, and any "fat is refused" line — is now false. Replace them with a FAT FILES section:

```c
 * FAT FILES. A fat (universal) container is edited slice by slice and kept
 * whole: no slice is dropped or reordered. With no `arch` directive the
 * script applies to every 64-bit slice; 32-bit slices pass through. With
 * `arch` directives it applies to exactly the named slices, and naming one
 * the file lacks, or a 32-bit one, is refused before any slice is touched.
 * On a thin file an `arch` directive must name the image's own arch.
 *
 * Each selected slice runs every statement, in order, then its own final
 * verification; any failure refuses the whole run and writes nothing. A
 * statement that can match nothing (see fatal-warnings) is judged across
 * the selected slices: it has matched if it matched in any of them, and the
 * verdict is taken when the last selected slice has run it.
 *
 * Under --verbose each slice is accounted for: "slice NAME:" before an
 * edited slice's statements and "slice NAME: verified" after; "slice NAME:
 * not selected by arch; passed through unchanged" or "slice NAME: 32-bit;
 * passed through unchanged" for the rest; and, after the slices are laid out
 * again, "slice NAME: moved from offset 0x… to 0x…" for any slice an earlier
 * slice's growth moved. A 64-bit fat container (fat_arch_64) is refused.
```

and add `arch NAME` to the DIRECTIVES section beside the other two.

- [ ] **Step 4: Correct the README**

1. The directives block gains:

```
arch NAME           apply the script only to the slice named NAME (lipo's
                    names: x86_64, x86_64h, arm64, arm64e, i386); repeatable.
                    Without it, every 64-bit slice of a fat file is edited
```

and the sentence before the block says there are three directives, not two.

2. Limits: replace the "**Input must be a thin 64-bit Mach-O.**" bullet with:

```markdown
- **A fat (universal) file is edited slice by slice, and kept whole.** With
  no `arch` directive every 64-bit slice is edited and 32-bit slices pass
  through; with `arch` directives, exactly the named slices. Naming a slice
  the file lacks, or a 32-bit one, is refused. `edit` never drops a slice —
  thin a file with `lipo` if you want one. A 64-bit fat container
  (`fat_arch_64`) is refused.
```

3. Where the README says `fatal-warnings` refuses a statement that matched nothing, add: "on a fat file, a statement has matched if it matched in any selected slice."
4. Where the README describes what `--verbose` reports, add the per-slice lines from Step 3.

- [ ] **Step 5: Run everything**

```bash
ctest --test-dir $B && sh tests/characterize.sh $B check
```

Expected: all pass; characterize OK.

- [ ] **Step 6: Commit**

```bash
git add tests/cli_test.sh src/edit.h README.md
git commit -m "docs: edit on fat files, end to end and in the README"
```

---

## Self-review

**Spec coverage.** "One split-and-reassemble" → Task 3 (moved, not copied; the verb's labels and stdout stay in its own callbacks; a slice failing refuses the whole file; `FAT_MAGIC_64` refused — Task 5 Step 6 for `edit`). "One table of arch names" → Task 1. "The `arch` directive" → Task 4 (parse, one operand, repeatable, precedes operations, parse error 2 for an unknown name — Task 6's CLI check). "Which slices a run touches" → Task 5 Steps 5-6 and its tests. "Execution" → Task 5 (slice by slice, per-slice verification, container re-parsed before writing, refusal names the slice). "fatal-warnings across slices" → Task 5 Step 3 (`me_verdict`, decided in the last selected slice). "Reporting" → Task 5 (`me_fat_slice`, `me_fat_placed`) and Task 6's CLI check of the moved line. "Testing" → Tasks 1, 3, 5, 6; fixtures hand-built (Task 2 shares `makefat`/`fatcheck` rather than copying them); the three named mutations → Task 5 Step 8. Documentation → Task 6.

**Two refinements of the spec, recorded there too:** `mfat_rewrite` hands every slice to one callback (which may leave it alone) instead of taking a separate `select` predicate, because `edit` must log the slices it passes through; and the "moved from offset" note is its own line after reassembly, because a slice's new offset is not known until every slice has run.

**Placeholders.** Two deliberate bracketed pointers in Task 3 Steps 4-5 name existing comments to move verbatim; each step says to replace them before committing. No other placeholders.

**Type consistency.** `mfat_slice_fn`, `mfat_placed_fn` and `mfat_rewrite` are spelled identically in Tasks 3 and 5; `ms_script.arch_mask` in Tasks 4 and 5; `ma_row`/`ma_lookup`/`ma_index`/`ma_describe`/`ma_list` in Tasks 1, 4 and 5; `me_verdict`/`me_statements`/`me_write_once`/`me_run_fat` only within Task 5.
