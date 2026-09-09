/*
 * image_test.c — hermetic tests for src/image.c, the open/validate/iterate layer.
 *
 * What this pins: every one of the seven rewriters currently opens a Mach-O by
 * hand (fstat, malloc, read, check MH_MAGIC_64) and walks its load commands with
 * its own `for (i = 0; i < hdr->ncmds; i++)` — all seven files do, and
 * macho_grow.h does it eleven times. Those copies agree today by coincidence,
 * not by construction. This tests the one implementation they are converging on.
 *
 * Ground truth is tests/fixture.macho, a real 10.9-built executable committed to
 * the repo: 8528 bytes, 16 load commands, __PAGEZERO then __TEXT at 0x100000000.
 * Reading a real binary is the point — a synthetic header would not catch a walk
 * that mis-strides on a load command this test never thought to build.
 *
 * Build (standalone):
 *   clang -O2 -I src -o /tmp/imgtest tests/image_test.c src/image.c && /tmp/imgtest
 */
#include "image.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg, ...) do { if (!(cond)) { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); fails++; } } while (0)

#define FIXTURE "tests/fixture.macho"

/* ---- open + validate ---- */

static void test_open_accepts_a_real_macho(void) {
    mi_image im;
    int rc = mi_open(FIXTURE, &im);
    CHECK(rc == 0, "mi_open(fixture) == 0 (got %d)", rc);
    if (rc != 0) return;
    CHECK(im.hdr != NULL,                "  hdr is set");
    CHECK(im.buf != NULL,                "  buf is set");
    CHECK(im.size == 8528,               "  size == 8528 (got %lu)", (unsigned long)im.size);
    CHECK(im.hdr->magic == MH_MAGIC_64,  "  magic is MH_MAGIC_64");
    CHECK(im.hdr->ncmds == 16,           "  ncmds == 16 (got %u)", im.hdr->ncmds);
    mi_close(&im);
}

static void test_open_refuses_a_missing_file(void) {
    mi_image im;
    CHECK(mi_open("tests/no-such-file.macho", &im) != 0, "mi_open(missing) refuses");
}

static void test_open_refuses_a_non_macho(void) {
    /* tests/EXPECTED is a committed text file — the cheapest honest non-Mach-O.
     * A tool that accepts this would go on to read a header out of ASCII. */
    mi_image im;
    CHECK(mi_open("tests/EXPECTED", &im) != 0, "mi_open(text file) refuses");
}

/* ---- iterate ---- */

struct lc_count { uint32_t seen; uint32_t segments; };

static void count_cb(const struct load_command *lc, void *ctx) {
    struct lc_count *c = (struct lc_count *)ctx;
    c->seen++;
    if (lc->cmd == LC_SEGMENT_64) c->segments++;
}

static void test_each_lc_visits_every_command(void) {
    mi_image im;
    if (mi_open(FIXTURE, &im) != 0) { CHECK(0, "each_lc: fixture would not open"); return; }
    struct lc_count c = { 0, 0 };
    mi_each_lc(&im, count_cb, &c);
    CHECK(c.seen == im.hdr->ncmds, "mi_each_lc visits all %u commands (got %u)",
          im.hdr->ncmds, c.seen);
    CHECK(c.segments >= 2, "  and sees at least __PAGEZERO and __TEXT (got %u)", c.segments);
    mi_close(&im);
}

/* ---- find ---- */

static void test_find_segment(void) {
    mi_image im;
    if (mi_open(FIXTURE, &im) != 0) { CHECK(0, "find_segment: fixture would not open"); return; }
    struct segment_command_64 *text = mi_find_segment(&im, "__TEXT");
    CHECK(text != NULL, "mi_find_segment(__TEXT) found");
    if (text) CHECK(text->vmaddr == 0x100000000ULL,
                    "  __TEXT vmaddr == 0x100000000 (got 0x%llx)",
                    (unsigned long long)text->vmaddr);
    CHECK(mi_find_segment(&im, "__PAGEZERO") != NULL, "mi_find_segment(__PAGEZERO) found");
    CHECK(mi_find_segment(&im, "__NOPE") == NULL,     "mi_find_segment(__NOPE) is NULL");
    mi_close(&im);
}

static void test_find_section(void) {
    mi_image im;
    if (mi_open(FIXTURE, &im) != 0) { CHECK(0, "find_section: fixture would not open"); return; }
    struct section_64 *text = mi_find_section(&im, "__TEXT", "__text");
    CHECK(text != NULL, "mi_find_section(__TEXT,__text) found");
    if (text) CHECK(text->size > 0, "  and it is non-empty");
    CHECK(mi_find_section(&im, "__TEXT", "__nope")  == NULL, "wrong section name is NULL");
    CHECK(mi_find_section(&im, "__NOPE", "__text")  == NULL, "wrong segment name is NULL");
    mi_close(&im);
}

/* ---- text base ---- */

static void test_text_base_is_the_segment_mapping_the_header(void) {
    /* Not simply "__TEXT.vmaddr": it is the segment whose file range covers
     * offset 0, which is what every base-relative fixup in macho_grow.h means
     * by the image base. On this fixture they coincide, which is why the
     * assertion can be exact. */
    mi_image im;
    if (mi_open(FIXTURE, &im) != 0) { CHECK(0, "text_base: fixture would not open"); return; }
    CHECK(mi_text_base(&im) == 0x100000000ULL,
          "mi_text_base == 0x100000000 (got 0x%llx)",
          (unsigned long long)mi_text_base(&im));
    mi_close(&im);
}

int main(void) {
    test_open_accepts_a_real_macho();
    test_open_refuses_a_missing_file();
    test_open_refuses_a_non_macho();
    test_each_lc_visits_every_command();
    test_find_segment();
    test_find_section();
    test_text_base_is_the_segment_mapping_the_header();

    if (fails == 0) { printf("image_test: all cases pass\n"); return 0; }
    printf("image_test: %d FAILED\n", fails);
    return 1;
}
