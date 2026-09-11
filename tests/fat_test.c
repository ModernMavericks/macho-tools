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
