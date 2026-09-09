/*
 * tests/live_test.c — tests for src/live.h, the header-only, malloc-free
 * queries against a LOADED image (see live.h's own top comment for the full
 * contract).
 *
 * Ground truth is this test binary's OWN loaded image: `_dyld_get_image_header(0)`
 * / `_dyld_get_image_vmaddr_slide(0)` return the same shape avxemu would get
 * for any image in its process, so querying "myself, right now" is querying
 * the real thing this header exists to answer questions about — not a
 * synthetic header this test invented, the same reasoning image_test.c gives
 * for reading a real linker-built fixture instead of a hand-built one.
 * Assertions are things independently true of any Mach-O executable this
 * build produces: it has a __TEXT segment, that segment has a __text
 * section, and this test's own code (mlp_probe_addr, taken by address) lives
 * inside that section's mapped range once the runtime slide is added in —
 * which is the one thing live.h computes that image.c has no equivalent
 * for, so exercising it against a real, checkable address is the point, not
 * incidental.
 *
 * The next-to-last test pins the file's hardest constraint: tests/live_probe.c
 * #includes ONLY src/live.h. This test compiles that fixture to a fresh
 * object with a plain `cc -O0 -c` (temp path, unique per run) and runs
 * `nm -u` over the result, asserting the undefined-symbol list is EMPTY --
 * an ALLOWLIST of nothing, not a denylist of a few names a reviewer found
 * missing (errno, pthread_once, getenv, a dyld-locking call, a call into an
 * unflagged new .c all passed a denylist of ~18 substrings; none can pass
 * an allowlist of nothing). -O0 is deliberate, not a leftover: at -O2 an
 * OBSERVED malloc/free pair -- not just an unobserved one -- was confirmed
 * (by hand, and independently by code review) to be optimized away
 * entirely, which would make this check pass whether or not live.h actually
 * stayed allocation-free. -O0 guarantees calls the source actually makes
 * show up in the object, so the check reflects the source, not the
 * optimizer's mood. Reading `nm`'s output here is inspecting this test's
 * own freshly-built object for symbol PRESENCE, not parsing a tool's
 * human-readable text as an oracle for Mach-O structure (tests/README.md's
 * lesson on that is about *.macho fixtures parsed for byte-level facts) —
 * the same distinction live_probe.c's own header comment draws.
 *
 * The last test pins live.h's bounds checking: a header-shaped fixture with
 * a plausible magic, ncmds=4e9 and a first load command whose cmdsize is 0
 * must be REFUSED on the very first iteration (mlive_each_lc returns -1
 * immediately), not walked -- an earlier revision with no cmdsize/sizeofcmds
 * bound was measured, by code review, to spin for 20+ seconds on exactly
 * this input inside what is supposed to be a signal handler. This is timed,
 * not just checked for the right return value, so a future regression back
 * to an unbounded walk fails loudly rather than merely "eventually" passing.
 *
 * Build (standalone):
 *   clang -O2 -Wall -Wextra -I src -o /tmp/livetest tests/live_test.c && /tmp/livetest
 * (run from the repo root: it shells out to `cc` on tests/live_probe.c using
 * a path relative to the current directory, same WORKING_DIRECTORY
 * convention image_test uses via CMakeLists.txt.)
 */
#include "live.h"

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(cond, msg, ...) do { if (!(cond)) { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); fails++; } } while (0)

/* ---- basic validity against this test's own loaded image ---- */

static void test_own_image_is_valid_64bit(void) {
    const struct mach_header_64 *mh =
        (const struct mach_header_64 *)_dyld_get_image_header(0);
    CHECK(mh != NULL, "_dyld_get_image_header(0) is non-NULL");
    if (!mh) return;
    CHECK(mlive_valid(mh), "mlive_valid(own header) (magic=%#x)", mh->magic);
    CHECK(mh->magic == MH_MAGIC_64, "own header magic == MH_MAGIC_64 (got %#x)", mh->magic);
}

static void test_finds_text_segment(void) {
    const struct mach_header_64 *mh =
        (const struct mach_header_64 *)_dyld_get_image_header(0);
    const struct segment_command_64 *sg = mlive_find_segment(mh, "__TEXT");
    CHECK(sg != NULL, "mlive_find_segment(own image, \"__TEXT\") is non-NULL");
    if (!sg) return;
    CHECK(sg->cmd == LC_SEGMENT_64, "__TEXT command is LC_SEGMENT_64 (got %#x)", sg->cmd);
    CHECK(mlive_name_eq(sg->segname, "__TEXT"), "segname reads back as __TEXT");
    CHECK(sg->nsects > 0, "__TEXT has at least one section (nsects=%u)", sg->nsects);
}

static void test_finds_text_section(void) {
    const struct mach_header_64 *mh =
        (const struct mach_header_64 *)_dyld_get_image_header(0);
    const struct section_64 *se = mlive_find_section(mh, "__TEXT", "__text");
    CHECK(se != NULL, "mlive_find_section(own image, \"__TEXT\", \"__text\") is non-NULL");
    if (!se) return;
    CHECK(mlive_name_eq(se->sectname, "__text"), "sectname reads back as __text");
    CHECK(se->size > 0, "__text has nonzero size (got %llu)", (unsigned long long)se->size);
}

/* mlive_find_section walks sg->nsects entries. A mutant off-by-one
 * (`i + 1 < sg->nsects` instead of `i < sg->nsects`) would still find
 * __text, since it's virtually always __TEXT's FIRST section -- code
 * review confirmed exactly this mutant survives testing that stops at
 * __text alone. So this checks the LAST section instead, found
 * independently of live.h (direct struct access, not a second call into the
 * function under test) so the ground truth doesn't depend on the very
 * code being checked. The name isn't hardcoded -- which section a linker
 * puts last in __TEXT is toolchain-dependent -- so this reads whatever is
 * actually there. */
static void test_finds_last_section_of_text(void) {
    const struct mach_header_64 *mh =
        (const struct mach_header_64 *)_dyld_get_image_header(0);
    const struct segment_command_64 *sg = mlive_find_segment(mh, "__TEXT");
    if (!sg || sg->nsects == 0) { CHECK(0, "last-section: __TEXT has no sections"); return; }

    const struct section_64 *secs = (const struct section_64 *)(const void *)(sg + 1);
    const struct section_64 *last = &secs[sg->nsects - 1];

    /* Build a NUL-terminated copy per tests/README.md's lesson nine:
     * sectname is char[16] and may legally fill all 16 bytes with no room
     * for a terminator, so memcpy-and-cap, never strcpy/sprintf. */
    char name[17];
    memcpy(name, last->sectname, 16);
    name[16] = '\0';

    const struct section_64 *found = mlive_find_section(mh, "__TEXT", name);
    CHECK(found == last,
          "mlive_find_section finds __TEXT's LAST section (\"%.16s\", index %u of %u), "
          "not just its first",
          name, sg->nsects - 1, sg->nsects);
}

/* A real function this test defines, so its address is a fact about the
 * running binary that mlive_addr's arithmetic can be checked against. */
static volatile int mlp_probe_addr_marker;
static void mlp_probe_addr_fn(void) { mlp_probe_addr_marker = 1; }

static void test_text_section_contains_a_known_function(void) {
    const struct mach_header_64 *mh =
        (const struct mach_header_64 *)_dyld_get_image_header(0);
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    const struct section_64 *se = mlive_find_section(mh, "__TEXT", "__text");
    if (!se) { CHECK(0, "text section: fixture unavailable"); return; }

    uintptr_t start = mlive_addr(se->addr, slide);
    uintptr_t end   = start + (uintptr_t)se->size;
    uintptr_t fn    = (uintptr_t)&mlp_probe_addr_fn;

    CHECK(fn >= start && fn < end,
          "mlp_probe_addr_fn (%#lx) falls inside __text's mapped range [%#lx, %#lx)",
          (unsigned long)fn, (unsigned long)start, (unsigned long)end);
}

/* ---- mlive_each_lc, cross-checked against an independent hand-rolled walk ---- */

typedef struct { uint32_t nseg; } count_ctx;

static int count_segments_cb(const struct load_command *lc, void *ctx_) {
    count_ctx *ctx = (count_ctx *)ctx_;
    if (lc->cmd == LC_SEGMENT_64) ctx->nseg++;
    return 0; /* keep walking */
}

static void test_each_lc_visits_every_segment(void) {
    const struct mach_header_64 *mh =
        (const struct mach_header_64 *)_dyld_get_image_header(0);

    count_ctx via_live = { 0 };
    int walked_all = mlive_each_lc(mh, count_segments_cb, &via_live);
    CHECK(walked_all == 1, "mlive_each_lc visited every command (got %d)", walked_all);

    /* Independent ground truth: a hand-rolled walk over the same mh, not
     * using live.h at all, the same way image_test.c trusts a fixture's
     * known byte layout rather than a function under test. */
    uint32_t independent = 0;
    const uint8_t *p = (const uint8_t *)mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)(const void *)p;
        if (lc->cmd == LC_SEGMENT_64) independent++;
        p += lc->cmdsize;
    }

    CHECK(via_live.nseg > 0, "at least one LC_SEGMENT_64 (got %u)", via_live.nseg);
    CHECK(via_live.nseg == independent,
          "mlive_each_lc's count agrees with an independent walk (%u vs %u)",
          via_live.nseg, independent);
}

static int stop_immediately_cb(const struct load_command *lc, void *ctx) {
    (void)lc; (void)ctx;
    return 1; /* ask to stop on the very first command */
}

static void test_each_lc_stopping_early_is_reported(void) {
    const struct mach_header_64 *mh =
        (const struct mach_header_64 *)_dyld_get_image_header(0);
    int walked_all = mlive_each_lc(mh, stop_immediately_cb, NULL);
    CHECK(walked_all == 0, "mlive_each_lc reports 0 when a callback stops early (got %d)",
          walked_all);
}

/* ---- malformed / absent input is handled without crashing ---- */

static void test_null_header_returns_null_or_zero(void) {
    CHECK(mlive_valid(NULL) == 0, "mlive_valid(NULL) == 0");
    CHECK(mlive_find_segment(NULL, "__TEXT") == NULL, "mlive_find_segment(NULL, ...) == NULL");
    CHECK(mlive_find_section(NULL, "__TEXT", "__text") == NULL,
          "mlive_find_section(NULL, ...) == NULL");
    CHECK(mlive_each_lc(NULL, count_segments_cb, NULL) == -1,
          "mlive_each_lc(NULL, ...) == -1 (refuses rather than walking garbage)");
}

static void test_bad_magic_is_rejected(void) {
    /* A header-shaped struct that is NOT a real Mach-O -- ncmds is nonzero
     * on purpose, to prove mlive_valid's magic check is what stops the walk,
     * not merely ncmds==0 making every loop a no-op. */
    struct mach_header_64 fake;
    memset(&fake, 0, sizeof fake);
    fake.magic  = 0xdeadbeef;
    fake.ncmds  = 5;
    fake.sizeofcmds = 200;

    CHECK(mlive_valid(&fake) == 0, "mlive_valid rejects a bad magic");
    CHECK(mlive_find_segment(&fake, "__TEXT") == NULL,
          "mlive_find_segment refuses a bad-magic header rather than walking it");
    CHECK(mlive_each_lc(&fake, count_segments_cb, NULL) == -1,
          "mlive_each_lc refuses a bad-magic header rather than walking it");
}

/* This is the exact shape code review reported: valid magic, ncmds in the
 * billions, and a first load command whose cmdsize is 0. Before
 * mlive_each_lc bounded cmdsize against sizeofcmds, this spun for 20+
 * seconds (4e9 callback invocations) inside what is supposed to be a
 * signal handler; a garbage nonzero cmdsize instead walks off the mapped
 * region and segfaults. With the bound in place it must refuse on the
 * FIRST iteration -- checked here both by return value and by wall-clock
 * time, so a regression back to the unbounded walk fails loudly rather
 * than "eventually" passing. */
static void test_malformed_sizeofcmds_refuses_fast(void) {
    struct { struct mach_header_64 hdr; struct load_command first_lc; } fixture;
    memset(&fixture, 0, sizeof fixture);
    fixture.hdr.magic       = MH_MAGIC_64;
    fixture.hdr.ncmds       = 4000000000u;   /* the reviewer's exact repro */
    fixture.hdr.sizeofcmds  = (uint32_t)sizeof(struct load_command); /* just
        enough for ONE real command -- so the walk actually dereferences
        `first_lc` (cmdsize=0, from the zero-fill above) rather than being
        refused before ever reading memory, matching the reported input
        precisely rather than a weaker "sizeofcmds also lies" variant. */
    fixture.first_lc.cmd     = 0;
    fixture.first_lc.cmdsize = 0;

    clock_t start = clock();
    int rc = mlive_each_lc((const struct mach_header_64 *)&fixture, count_segments_cb, NULL);
    clock_t end = clock();
    double secs = (double)(end - start) / CLOCKS_PER_SEC;

    CHECK(rc == -1, "ncmds=4e9/cmdsize=0 fixture is refused (-1), not walked (got %d)", rc);
    CHECK(secs < 1.0,
          "refusal took %.3fs (want < 1s -- a spin back to O(ncmds) would take ~20s here)",
          secs);
}

/* ---- the four header-internal bounds checks, individually discriminated ----
 *
 * Review found live.h's four checks (see its own top comment's "WHAT THIS
 * FILE VALIDATES" section) asymmetrically covered relative to image.c's
 * identical mi_validate: the ONLY malformed case above (cmdsize=0) exercises
 * just one of the four -- `cmdsize < sizeof(load_command)`. Deleting either
 * `cmdsize % 8 != 0` (live.h:191) or LC_SEGMENT_64's `cmdsize != want`
 * (live.h:205) left every one of the 10 suites green. This matters more here
 * than the analogous gap would in image.c: live.h ships into avxemu's SIGILL
 * handler, so a missing nsects bound is an unbounded read inside a signal
 * handler, not merely a bad refusal in an offline tool.
 *
 * Five tests below, mirroring image_test.c's own five malformed-header cases
 * one-for-one (minus the file-size bound, which has no live.h analogue --
 * see live.h's own top comment on why): together they discriminate each of
 * live.h's four checks (the segment-cmdsize family is two lines, 195 and
 * 205, each pinned by its own test here exactly as image_test.c pins its
 * mi_validate equivalents at :64). Each was mutation-proven individually:
 * commenting out just the ONE check named in the test's comment, forcing a
 * clean rebuild, and confirming ONLY that test (not the others) fails --
 * see this wave's report for the table. */

static void test_each_lc_refuses_short_cmdsize(void) {
    /* Pins live.h:186, `lc->cmdsize < sizeof(struct load_command)`,
     * independent of test_malformed_sizeofcmds_refuses_fast's giant ncmds
     * (that test's real purpose is the O(ncmds) spin-time regression, not
     * this specific bound). */
    uint8_t buf[sizeof(struct mach_header_64) + 16];
    memset(buf, 0, sizeof buf);
    struct mach_header_64 *hdr = (struct mach_header_64 *)(void *)buf;
    hdr->magic = MH_MAGIC_64;
    hdr->ncmds = 1;
    hdr->sizeofcmds = 16;
    struct load_command *lc = (struct load_command *)(void *)(buf + sizeof(*hdr));
    lc->cmd = LC_UUID;
    /* 0, not merely "below 8": any nonzero value below 8 is also caught by
     * the cmdsize%8 check (191), which would not isolate 186 from 191. 0 is
     * a multiple of 8 and satisfies off+cmdsize<=sizeofcmds trivially, so
     * this input reaches ONLY the check under test. */
    lc->cmdsize = 0;

    int rc = mlive_each_lc((const struct mach_header_64 *)(const void *)buf,
                            count_segments_cb, NULL);
    CHECK(rc == -1, "mlive_each_lc(cmdsize < sizeof(load_command)) refuses (got %d)", rc);
}

static void test_each_lc_refuses_unaligned_cmdsize(void) {
    /* Pins live.h:191, `lc->cmdsize % 8 != 0` -- a cmdsize large enough to be
     * a load command, small enough to fit sizeofcmds, but not 8-byte
     * aligned, misaligning every command walked after it. */
    uint8_t buf[sizeof(struct mach_header_64) + 24];
    memset(buf, 0, sizeof buf);
    struct mach_header_64 *hdr = (struct mach_header_64 *)(void *)buf;
    hdr->magic = MH_MAGIC_64;
    hdr->ncmds = 1;
    hdr->sizeofcmds = 24;
    struct load_command *lc = (struct load_command *)(void *)(buf + sizeof(*hdr));
    lc->cmd = LC_UUID;
    lc->cmdsize = 17;   /* well-formed size, but not a multiple of 8 */

    int rc = mlive_each_lc((const struct mach_header_64 *)(const void *)buf,
                            count_segments_cb, NULL);
    CHECK(rc == -1, "mlive_each_lc(cmdsize not 8-byte aligned) refuses (got %d)", rc);
}

static void test_each_lc_refuses_cmdsize_past_sizeofcmds(void) {
    /* Pins live.h:192, `off + lc->cmdsize > sizeofcmds` -- sizeofcmds bounds
     * the region as a whole, but this one command claims more room than the
     * region has left. */
    uint8_t buf[sizeof(struct mach_header_64) + 16];
    memset(buf, 0, sizeof buf);
    struct mach_header_64 *hdr = (struct mach_header_64 *)(void *)buf;
    hdr->magic = MH_MAGIC_64;
    hdr->ncmds = 1;
    hdr->sizeofcmds = 16;
    struct load_command *lc = (struct load_command *)(void *)(buf + sizeof(*hdr));
    lc->cmd = LC_UUID;
    lc->cmdsize = 32;   /* overshoots the 16-byte region */

    int rc = mlive_each_lc((const struct mach_header_64 *)(const void *)buf,
                            count_segments_cb, NULL);
    CHECK(rc == -1, "mlive_each_lc(cmdsize striding past sizeofcmds) refuses (got %d)", rc);
}

/* Pins live.h:195, `lc->cmdsize < sizeof(struct segment_command_64)`.
 *
 * 195 is NOT redundant with 205 (`cmdsize != want`, `want` computed from
 * `sg->nsects`) -- it is a PRECONDITION for 205: evaluating `want` requires
 * READING `sg->nsects`, which lives at byte offset 64 of the command, and
 * with 195 deleted THAT LOAD is the out-of-bounds read. By the time a
 * return-value check could observe "205 still says -1", the read 195 was
 * guarding has already happened -- for a live image mapped by dyld, an
 * ordinary-looking cmdsize that undershoots the struct by enough puts
 * offset 64 past the mapped region entirely, and this file exists
 * specifically so that read happens inside avxemu's SIGILL handler.
 * A return-value assertion (the plain-buffer test just below) cannot
 * observe that distinction: on a small stack/heap buffer, the 56 bytes
 * past `cmdsize` happen to be readable garbage either way, so 195 and 205
 * produce the identical rc == -1 whether or not 195 ran. That test still
 * documents the outcome-level invariant (matches image_test.c's own
 * analogous test_wrap_refuses_an_lc_segment_64_shorter_than_the_struct for
 * mi_validate's identical pair), but it is not proof that 195 itself does
 * anything -- for that, the read has to be made to fault, which needs a
 * guard page, not a wider assertion. See test_each_lc_195_read_is_bounded
 * below for that proof. */
static void test_each_lc_refuses_segment_shorter_than_struct(void) {
    uint8_t buf[sizeof(struct mach_header_64) + 8];
    memset(buf, 0, sizeof buf);
    struct mach_header_64 *hdr = (struct mach_header_64 *)(void *)buf;
    hdr->magic = MH_MAGIC_64;
    hdr->ncmds = 1;
    hdr->sizeofcmds = 8;
    struct load_command *lc = (struct load_command *)(void *)(buf + sizeof(*hdr));
    lc->cmd = LC_SEGMENT_64;
    lc->cmdsize = 8;   /* far below sizeof(struct segment_command_64) (72) */

    int rc = mlive_each_lc((const struct mach_header_64 *)(const void *)buf,
                            count_segments_cb, NULL);
    CHECK(rc == -1,
          "mlive_each_lc(LC_SEGMENT_64 cmdsize < sizeof(segment_command_64)) refuses (got %d)",
          rc);
}

/* The real proof for 195: a guard page, not a return-value assertion.
 *
 * mmap two pages, PROT_NONE the second, and place the fixture so `lc`'s
 * own cmd/cmdsize (the only fields 195's own check reads) sit in the last
 * 8 readable bytes of the first page, while `sg->nsects` -- offset 64 past
 * `lc`, so 56 bytes INTO the guard page -- lands somewhere 195 must never
 * let anything read. With 195 intact, the walk refuses on cmdsize alone
 * and never touches the guard page: rc == -1, no fault. With 195 deleted,
 * `want`'s computation (the very next lines, for 205's check) has to read
 * `sg->nsects` to produce an answer, and that read is INTO THE GUARD PAGE
 * -- SIGBUS, not a wrong return value.
 *
 * `sizeofcmds`/`cmdsize` are assigned through `volatile` locals rather
 * than the literal `8` a first version of this test wrote directly into
 * the fields. That literal mattered: with 195 deleted, the compiler can
 * still prove `want (>= 72, since nsects is unsigned) != cmdsize` WITHOUT
 * reading `sg->nsects` at all, PROVIDED it also knows `cmdsize`'s exact
 * value at compile time -- and a direct `lc->cmdsize = 8;` a few lines
 * before an inlined call is exactly the kind of store this compiler's
 * optimizer forwards straight into the load, at -O2 AND -O0 alike (this
 * was verified directly: a first version of this fixture, built and
 * mutated exactly as below but with `hdr->sizeofcmds = 8;` and
 * `lc->cmdsize = 8;` as plain literals, refused cleanly at -O2 with 195
 * DELETED -- rc == -1, no fault, because the compiler proved the read
 * irrelevant and never issued it. That is a sound optimization of THIS
 * fixture, not evidence 195 is unnecessary: it is provable only because
 * the fixture's own cmdsize is small enough, and known enough, for the
 * compiler to rule out every nsects value without consulting memory --
 * a live image's cmdsize is neither. Routing both fields through a
 * `volatile` intermediate is the fix: it forces a genuine, unpredictable-
 * to-the-optimizer runtime value, so `want`'s computation is no longer
 * something the compiler can resolve without actually performing the
 * read. Mutation-proven at both -O0 and -O2 with THIS fixture: 195
 * intact refuses cleanly (rc == -1, exit 0) at both; 195 deleted faults
 * (SIGBUS, exit 138) at both -- see this wave's report for the full
 * trace, including the literal-vs-volatile A/B comparison. */
static void test_each_lc_195_read_is_bounded(void) {
    long pagesz = sysconf(_SC_PAGESIZE);
    CHECK(pagesz > 0, "sysconf(_SC_PAGESIZE) succeeds");
    if (pagesz <= 0) return;

    uint8_t *region = (uint8_t *)mmap(NULL, (size_t)pagesz * 2, PROT_READ | PROT_WRITE,
                                       MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(region != MAP_FAILED, "mmap(2 pages) succeeds");
    if (region == MAP_FAILED) return;
    CHECK(mprotect(region + pagesz, (size_t)pagesz, PROT_NONE) == 0,
          "mprotect(second page, PROT_NONE) succeeds -- the guard page");

    size_t need = sizeof(struct mach_header_64) + 8;
    uint8_t *buf = region + pagesz - (long)need;
    memset(buf, 0, need);
    struct mach_header_64 *hdr = (struct mach_header_64 *)(void *)buf;
    hdr->magic = MH_MAGIC_64;
    hdr->ncmds = 1;
    volatile uint32_t v_sizeofcmds = 8;
    hdr->sizeofcmds = v_sizeofcmds;
    struct load_command *lc = (struct load_command *)(void *)(buf + sizeof(*hdr));
    lc->cmd = LC_SEGMENT_64;
    volatile uint32_t v_cmdsize = 8;   /* far below sizeof(segment_command_64) (72) */
    lc->cmdsize = v_cmdsize;

    count_ctx ctx = { 0 };
    int rc = mlive_each_lc((const struct mach_header_64 *)(const void *)buf,
                            count_segments_cb, &ctx);
    CHECK(rc == -1,
          "mlive_each_lc(LC_SEGMENT_64 cmdsize < sizeof(segment_command_64)) refuses "
          "without ever reading past cmdsize (got %d)", rc);

    munmap(region, (size_t)pagesz * 2);
}

static void test_each_lc_refuses_nsects_disagreeing_with_cmdsize(void) {
    /* Pins live.h:205, `lc->cmdsize != want` -- the check a bare
     * cmdsize/sizeofcmds bound misses: cmdsize covers the base struct but
     * disagrees with nsects, which mlive_find_section trusts unchecked once
     * this passes. This is the exact shape code review used to demonstrate
     * an unbounded read inside a signal handler is possible without it. */
    uint8_t buf[sizeof(struct mach_header_64) + sizeof(struct segment_command_64)];
    memset(buf, 0, sizeof buf);
    struct mach_header_64 *hdr = (struct mach_header_64 *)(void *)buf;
    hdr->magic = MH_MAGIC_64;
    hdr->ncmds = 1;
    hdr->sizeofcmds = (uint32_t)sizeof(struct segment_command_64);
    struct segment_command_64 *sg =
        (struct segment_command_64 *)(void *)(buf + sizeof(*hdr));
    sg->cmd = LC_SEGMENT_64;
    sg->cmdsize = (uint32_t)sizeof(struct segment_command_64);   /* covers 0 sections */
    sg->nsects = 5;   /* claims 5 trailing section_64 entries that don't fit */

    /* A real context, not NULL: with 205 deleted, mlive_each_lc's own bound
     * check no longer refuses this segment, so the callback DOES run (see
     * this wave's report -- an earlier version of this test passed NULL
     * here on the assumption the callback could never actually be reached,
     * which held only as long as 205 was intact; deleting 205 let the
     * callback run and NULL-deref, crashing the whole test binary instead
     * of failing this one CHECK cleanly). count_segments_cb only reads
     * lc->cmd and increments ctx->nseg, so a real context keeps this test's
     * own discrimination entirely in rc, matching the other three checks in
     * this family. */
    count_ctx ctx = { 0 };
    int rc = mlive_each_lc((const struct mach_header_64 *)(const void *)buf,
                            count_segments_cb, &ctx);
    CHECK(rc == -1,
          "mlive_each_lc(LC_SEGMENT_64 nsects disagreeing with cmdsize) refuses (got %d)",
          rc);
}

static void test_missing_segment_or_section_returns_null(void) {
    const struct mach_header_64 *mh =
        (const struct mach_header_64 *)_dyld_get_image_header(0);
    CHECK(mlive_find_segment(mh, "__NOT_A_REAL_SEG") == NULL,
          "mlive_find_segment: a segment name that doesn't exist returns NULL");
    CHECK(mlive_find_section(mh, "__NOT_A_REAL_SEG", "__text") == NULL,
          "mlive_find_section: segment doesn't exist -> NULL");
    CHECK(mlive_find_section(mh, "__TEXT", "__not_a_real_sect") == NULL,
          "mlive_find_section: segment exists, section doesn't -> NULL");
}

/* ---- pin the allocation-free constraint with a real, running check ---- */

/* Read an entire small file into a malloc'd, NUL-terminated buffer. This is
 * test-harness code, not live.h -- it is fine for it to allocate. */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    size_t cap = 0, len = 0;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (len + n + 1 > cap) {
            cap = (len + n + 1) * 2;
            buf = (char *)realloc(buf, cap);
        }
        memcpy(buf + len, chunk, n);
        len += n;
    }
    fclose(f);
    if (!buf) { buf = (char *)malloc(1); len = 0; }
    buf[len] = '\0';
    return buf;
}

static void test_probe_object_is_allocation_free(void) {
    char objpath[256], logpath[256], nmpath[256];
    pid_t pid = getpid();
    snprintf(objpath, sizeof objpath, "%s/live_probe.%ld.o",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (long)pid);
    snprintf(logpath, sizeof logpath, "%s/live_probe.%ld.cc.log",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (long)pid);
    snprintf(nmpath, sizeof nmpath, "%s/live_probe.%ld.nm.log",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (long)pid);

    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";

    /* -O0, deliberately: at -O2 a single unobserved malloc(16) call was
     * confirmed (by hand, during this test's own development) to be
     * optimized away entirely, which would make this check pass whether or
     * not live.h stayed allocation-free. -O0 guarantees the object reflects
     * what the source actually calls. */
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "%s -O0 -Wall -Wextra -I src -c tests/live_probe.c -o %s > %s 2>&1",
             cc, objpath, logpath);
    int rc = system(cmd);
    CHECK(rc == 0, "compiling tests/live_probe.c (a %s-only TU) succeeds (rc=%d)", "live.h", rc);
    if (rc != 0) {
        char *log = slurp(logpath);
        if (log) { printf("  compiler output:\n%s\n", log); free(log); }
        remove(logpath);
        return;
    }
    remove(logpath);

    /* ALLOWLIST, not a denylist: live.h calls no external function at all
     * (see its own top comment on why even strncmp was removed), so the
     * permitted set of undefined symbols in this probe object is EMPTY.
     * A denylist of specific names (the previous shape of this check)
     * missed errno, pthread_once, getenv, a call to
     * _dyld_get_image_header, and a call into an unflagged new .c -- all
     * confirmed by code review to sail through ~18 forbidden substrings.
     * None of those, or anything else, can produce an undefined symbol
     * without failing an allowlist of nothing. */
    /* stderr goes to /dev/null here, deliberately NOT merged with stdout:
     * under DYLD_INSERT_LIBRARIES=libgmalloc (this test is required to run
     * clean under it), that env var reaches the `nm` child too and
     * GuardMalloc prints its own startup banner to ITS stderr -- merging
     * that into what this check treats as "the undefined-symbol list"
     * would fail a perfectly clean object for a reason that has nothing to
     * do with live.h. rc==0 still confirms nm itself ran successfully. */
    snprintf(cmd, sizeof cmd, "nm -u %s > %s 2>/dev/null", objpath, nmpath);
    rc = system(cmd);
    CHECK(rc == 0, "nm -u on the probe object succeeds (rc=%d)", rc);

    char *nm_out = slurp(nmpath);
    CHECK(nm_out != NULL, "nm -u output was captured");
    if (nm_out) {
        size_t len = strlen(nm_out);
        while (len > 0 && (nm_out[len-1] == '\n' || nm_out[len-1] == ' ' ||
                            nm_out[len-1] == '\t' || nm_out[len-1] == '\r')) {
            nm_out[--len] = '\0';
        }
        CHECK(len == 0,
              "probe object's undefined-symbol list is EMPTY (allowlist of nothing) -- "
              "got: \"%s\"", nm_out);
    }
    free(nm_out);

    /* A vacuously-clean check (e.g. nm silently failing to run, or being
     * pointed at an empty/wrong object) would pass the CHECK above for the
     * wrong reason. Confirm the object is real and non-trivial by looking
     * for mlp_probe's own DEFINED symbol in a plain (non -u) listing --
     * proof `nm` actually inspected the object this test just built, not
     * nothing. */
    snprintf(cmd, sizeof cmd, "nm %s > %s 2>/dev/null", objpath, nmpath);
    rc = system(cmd);
    CHECK(rc == 0, "plain nm on the probe object succeeds (rc=%d)", rc);
    char *nm_all = slurp(nmpath);
    CHECK(nm_all != NULL && strstr(nm_all, "_mlp_probe") != NULL,
          "sanity: the probe object DOES define _mlp_probe (proves nm inspected the real "
          "object, not an empty one)");
    free(nm_all);

    remove(objpath);
    remove(nmpath);
}

int main(void) {
    test_own_image_is_valid_64bit();
    test_finds_text_segment();
    test_finds_text_section();
    test_finds_last_section_of_text();
    test_text_section_contains_a_known_function();
    test_each_lc_visits_every_segment();
    test_each_lc_stopping_early_is_reported();
    test_null_header_returns_null_or_zero();
    test_bad_magic_is_rejected();
    test_malformed_sizeofcmds_refuses_fast();
    test_each_lc_refuses_short_cmdsize();
    test_each_lc_refuses_unaligned_cmdsize();
    test_each_lc_refuses_cmdsize_past_sizeofcmds();
    test_each_lc_refuses_segment_shorter_than_struct();
    test_each_lc_195_read_is_bounded();
    test_each_lc_refuses_nsects_disagreeing_with_cmdsize();
    test_missing_segment_or_section_returns_null();
    test_probe_object_is_allocation_free();

    if (fails) {
        printf("%d check(s) FAILED\n", fails);
        return 1;
    }
    printf("all live_test checks passed\n");
    return 0;
}
