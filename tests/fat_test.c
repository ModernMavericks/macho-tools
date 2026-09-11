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

int main(void) {
    test_arch_names_round_trip();
    test_rewrite_nothing_changed_leaves_the_input();
    test_rewrite_a_grown_slice_moves_the_next();
    test_rewrite_refuses_an_overlapping_layout();
    test_rewrite_passes_a_callback_failure_through();
    printf("fat_test: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
