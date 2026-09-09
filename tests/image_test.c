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
#include <stdlib.h>

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

static void test_open_reports_its_capacity(void) {
    /* size is the FILE's size; cap is how much buffer there is. With no slack
     * asked for they are equal, and a caller that grows in place can tell the
     * difference rather than guessing. */
    mi_image im;
    if (mi_open(FIXTURE, &im) != 0) { CHECK(0, "capacity: fixture would not open"); return; }
    CHECK(im.size == 8528, "mi_open size == 8528 (got %lu)", (unsigned long)im.size);
    CHECK(im.cap == im.size, "mi_open cap == size (got %lu vs %lu)",
          (unsigned long)im.cap, (unsigned long)im.size);
    mi_close(&im);
}

static void test_open_slack_allocates_real_headroom(void) {
    /* patch_macho appends rebase/bind streams into the tail of its buffer, so it
     * over-allocates. That need is why mi_open alone could not serve it. The
     * headroom must be genuinely writable, not merely promised -- hence the
     * write to the last byte and the read back. */
    const size_t slack = 2u * 1024 * 1024;
    mi_image im;
    int rc = mi_open_slack(FIXTURE, slack, &im);
    CHECK(rc == 0, "mi_open_slack(fixture, 2MB) == 0 (got %d)", rc);
    if (rc != 0) return;
    CHECK(im.size == 8528, "  size is still the FILE size, 8528 (got %lu)",
          (unsigned long)im.size);
    CHECK(im.cap >= im.size + slack, "  cap >= size + slack (got %lu, want >= %lu)",
          (unsigned long)im.cap, (unsigned long)(im.size + slack));
    CHECK(im.hdr->magic == MH_MAGIC_64, "  and it is still a valid image");
    im.buf[im.cap - 1] = 0xA5;
    CHECK(im.buf[im.cap - 1] == 0xA5, "  the last slack byte is writable");
    mi_close(&im);
}

static void test_release_hands_the_buffer_to_the_caller(void) {
    /* change_dylib grows the header via mg_grow_header(&buf, &fsize, n), which
     * REALLOCS. An mi_image that still pointed at the old allocation would be a
     * dangling pointer waiting for mi_close. mi_release makes the hand-off
     * explicit: the caller owns the buffer, and the image is emptied so a later
     * mi_close is a no-op rather than a double free. */
    mi_image im;
    if (mi_open(FIXTURE, &im) != 0) { CHECK(0, "release: fixture would not open"); return; }
    uint8_t *owned = mi_release(&im);
    CHECK(owned != NULL,   "mi_release returns the buffer");
    CHECK(im.buf == NULL,  "  and the image no longer points at it");
    CHECK(im.size == 0,    "  size is cleared");
    CHECK(im.hdr == NULL,  "  hdr is cleared");
    CHECK(((struct mach_header_64 *)owned)->magic == MH_MAGIC_64,
          "  the handed-over buffer is still the image");
    mi_close(&im);   /* must be a safe no-op now */
    free(owned);
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

/* ---- wrap ---- */

static void test_wrap_accepts_a_caller_owned_buffer(void) {
    /* macho_grow_test.c builds Mach-O images SYNTHETICALLY IN MEMORY and never
     * from a file, so mi_open can't serve it. mi_wrap is how such a buffer gets
     * the same validated view, without mi_open's open()/read()/malloc. */
    mi_image src;
    if (mi_open(FIXTURE, &src) != 0) { CHECK(0, "wrap: fixture would not open"); return; }
    size_t size = src.size;
    uint8_t *owned = mi_release(&src);   /* caller now owns this buffer */

    mi_image im;
    int rc = mi_wrap(owned, size, &im);
    CHECK(rc == 0, "mi_wrap(valid buffer) == 0 (got %d)", rc);
    if (rc == 0) {
        CHECK(im.buf == owned,              "  buf IS the caller's buffer, not a copy");
        CHECK(im.size == size,              "  size matches");
        CHECK(im.cap == size,               "  cap == size (no slack)");
        CHECK(im.hdr->magic == MH_MAGIC_64, "  hdr is set");
        CHECK(mi_find_segment(&im, "__TEXT") != NULL, "  and it's iterable, e.g. finds __TEXT");
    }
    mi_close(&im);   /* must NOT free `owned` -- the caller still owns it */
    CHECK(((struct mach_header_64 *)owned)->magic == MH_MAGIC_64,
          "mi_close on a wrapped image left the caller's buffer intact");
    free(owned);
}

static void test_wrap_refuses_bad_magic(void) {
    uint8_t junk[sizeof(struct mach_header_64)];
    memset(junk, 0, sizeof junk);
    mi_image im;
    CHECK(mi_wrap(junk, sizeof junk, &im) != 0, "mi_wrap(bad magic) refuses");
}

/* Named separately from the generic bad-magic case above: a 32-bit Mach-O is
 * not "junk", it is a real, well-formed format this module deliberately does
 * not support (see image.h's file header -- "32-bit and fat are known gaps,
 * filed as Task 5"). Pinning it by name keeps that refusal from being an
 * accident of the generic magic check ever regressing into something looser.
 *
 * Reviewed and found tautological in its first form: a buffer sized to
 * sizeof(struct mach_header) (28 bytes) is caught by mi_validate's `size <
 * sizeof(mach_header_64)` (32 bytes) check BEFORE the magic check ever runs,
 * so the test passed even under a mutation that let 32-bit magic through the
 * check it claims to pin -- it was testing the size guard, not the magic
 * one. Fixed by padding the buffer to sizeof(struct mach_header_64): now the
 * ONLY thing wrong with it is the magic, so weakening the magic check (e.g.
 * `hdr->magic != MH_MAGIC_64` -> `hdr->magic != MH_MAGIC_64 && hdr->magic !=
 * MH_MAGIC`) makes mi_wrap accept it and this test fails. Confirmed by hand:
 * that exact mutation flips this CHECK from pass to fail. */
static void test_wrap_refuses_32bit_mach_header(void) {
    uint8_t buf[sizeof(struct mach_header_64)];
    memset(buf, 0, sizeof buf);
    struct mach_header *h = (struct mach_header *)buf;
    h->magic = MH_MAGIC;
    h->filetype = MH_EXECUTE;
    mi_image im;
    CHECK(mi_wrap(buf, sizeof buf, &im) != 0, "mi_wrap(32-bit MH_MAGIC) refuses");
}

static void test_wrap_refuses_load_commands_past_the_end(void) {
    /* Same validation mi_open does: a cmdsize/sizeofcmds that strides past the
     * buffer must be caught here, not walked off the end of by some later
     * caller that trusts ncmds. */
    mi_image src;
    if (mi_open(FIXTURE, &src) != 0) { CHECK(0, "wrap: fixture would not open"); return; }
    uint8_t *buf = (uint8_t *)malloc(src.size);
    memcpy(buf, src.buf, src.size);
    size_t size = src.size;
    mi_close(&src);

    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    hdr->sizeofcmds = 0xFFFFFFFFu;   /* claims far more than the buffer holds */

    mi_image im;
    CHECK(mi_wrap(buf, size, &im) != 0, "mi_wrap(overclaiming sizeofcmds) refuses");
    free(buf);
}

static void test_wrap_refuses_zero_cmdsize(void) {
    /* cmdsize == 0 is the infinite-walk case: any walk that adds cmdsize each
     * iteration without checking it first would never advance past this
     * command. */
    uint8_t buf[sizeof(struct mach_header_64) + 16];
    memset(buf, 0, sizeof buf);
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    hdr->magic = MH_MAGIC_64;
    hdr->ncmds = 1;
    hdr->sizeofcmds = 16;
    struct load_command *lc = (struct load_command *)(buf + sizeof(*hdr));
    lc->cmd = LC_UUID;
    lc->cmdsize = 0;

    mi_image im;
    CHECK(mi_wrap(buf, sizeof buf, &im) != 0, "mi_wrap(cmdsize == 0) refuses");
}

static void test_wrap_refuses_a_cmdsize_striding_past_sizeofcmds(void) {
    /* sizeofcmds correctly bounds the region as a whole, but the one command
     * inside it claims more room than the region has left. */
    uint8_t buf[sizeof(struct mach_header_64) + 16];
    memset(buf, 0, sizeof buf);
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    hdr->magic = MH_MAGIC_64;
    hdr->ncmds = 1;
    hdr->sizeofcmds = 16;
    struct load_command *lc = (struct load_command *)(buf + sizeof(*hdr));
    lc->cmd = LC_UUID;
    lc->cmdsize = 32;   /* overshoots the 16-byte region */

    mi_image im;
    CHECK(mi_wrap(buf, sizeof buf, &im) != 0,
          "mi_wrap(cmdsize striding past sizeofcmds) refuses");
}

static void test_wrap_refuses_an_lc_segment_64_shorter_than_the_struct(void) {
    /* Reviewer's second repro: a 40-byte buffer holding one LC_SEGMENT_64
     * whose cmdsize (8) doesn't even cover sizeof(segment_command_64) (72).
     * Before segments got their own check, mi_wrap accepted this, and
     * mi_find_segment then read segname at buf[40..56] -- past the end of
     * the buffer. */
    uint8_t buf[sizeof(struct mach_header_64) + 8];
    memset(buf, 0, sizeof buf);
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    hdr->magic = MH_MAGIC_64;
    hdr->ncmds = 1;
    hdr->sizeofcmds = 8;
    struct load_command *lc = (struct load_command *)(buf + sizeof(*hdr));
    lc->cmd = LC_SEGMENT_64;
    lc->cmdsize = 8;   /* far below sizeof(struct segment_command_64) */

    mi_image im;
    CHECK(mi_wrap(buf, sizeof buf, &im) != 0,
          "mi_wrap(LC_SEGMENT_64 cmdsize < sizeof(segment_command_64)) refuses");
}

static void test_wrap_refuses_nsects_disagreeing_with_cmdsize(void) {
    /* Reviewer's first repro, reproduced exactly: copy a real fixture, leave
     * __TEXT's cmdsize alone but corrupt its nsects to a huge value. Before
     * this was checked, mi_open/mi_wrap ACCEPTED it, and mg_first_sect_off's
     * `for (j < seg->nsects)` walked the section_64 array straight off the
     * end of the buffer -- SEGFAULT (exit 139) end-to-end through
     * change_dylib. nsects disagreeing with cmdsize must be refused here,
     * before any walk trusts nsects. */
    mi_image src;
    if (mi_open(FIXTURE, &src) != 0) { CHECK(0, "nsects: fixture would not open"); return; }
    size_t size = src.size;
    uint8_t *buf = (uint8_t *)malloc(size);
    memcpy(buf, src.buf, size);
    mi_close(&src);

    struct segment_command_64 *text = NULL;
    {
        mi_image tmp;
        if (mi_wrap(buf, size, &tmp) == 0) text = mi_find_segment(&tmp, "__TEXT");
    }
    CHECK(text != NULL, "nsects: found __TEXT to corrupt");
    if (text) text->nsects = 0x400000;   /* cmdsize is untouched */

    mi_image im;
    CHECK(mi_wrap(buf, size, &im) != 0,
          "mi_wrap(__TEXT.nsects disagreeing with cmdsize) refuses");
    free(buf);
}

static void test_wrap_does_not_copy(void) {
    /* The defining property: no malloc, no read -- a view over memory the
     * caller already has, stack included. */
    uint8_t stackbuf[8528];
    mi_image src;
    if (mi_open(FIXTURE, &src) != 0) { CHECK(0, "wrap: fixture would not open"); return; }
    memcpy(stackbuf, src.buf, src.size);
    mi_close(&src);

    mi_image im;
    int rc = mi_wrap(stackbuf, sizeof stackbuf, &im);
    CHECK(rc == 0, "mi_wrap(stack buffer) == 0 (got %d)", rc);
    CHECK(im.buf == stackbuf, "  and it points AT the stack buffer, not a copy");
    mi_close(&im);   /* must not free/touch a stack address */
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
    test_open_reports_its_capacity();
    test_open_slack_allocates_real_headroom();
    test_release_hands_the_buffer_to_the_caller();
    test_open_refuses_a_missing_file();
    test_open_refuses_a_non_macho();
    test_wrap_accepts_a_caller_owned_buffer();
    test_wrap_refuses_bad_magic();
    test_wrap_refuses_32bit_mach_header();
    test_wrap_refuses_load_commands_past_the_end();
    test_wrap_refuses_zero_cmdsize();
    test_wrap_refuses_a_cmdsize_striding_past_sizeofcmds();
    test_wrap_refuses_an_lc_segment_64_shorter_than_the_struct();
    test_wrap_refuses_nsects_disagreeing_with_cmdsize();
    test_wrap_does_not_copy();
    test_each_lc_visits_every_command();
    test_find_segment();
    test_find_section();
    test_text_base_is_the_segment_mapping_the_header();

    if (fails == 0) { printf("image_test: all cases pass\n"); return 0; }
    printf("image_test: %d FAILED\n", fails);
    return 1;
}
