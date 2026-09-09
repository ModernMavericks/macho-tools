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
 * The last test pins the file's hardest constraint: tests/live_probe.c
 * #includes ONLY src/live.h. This test compiles that fixture to a fresh
 * object with a plain `cc -O0 -c` (temp path, unique per run) and runs
 * `nm -u` over the result, asserting no malloc/free/calloc/realloc or stdio
 * symbol shows up undefined. -O0 is deliberate, not a leftover: at -O2 a
 * single unobserved `malloc(16)` call was seen to be optimized away entirely
 * during this test's own development (confirmed by hand — see the task
 * report), which would make the check trivially pass regardless of whether
 * live.h actually stayed allocation-free. -O0 guarantees calls the source
 * actually makes show up in the object, so the check reflects the source,
 * not the optimizer's mood. Reading `nm`'s output here is inspecting this
 * test's own freshly-built object for symbol PRESENCE, not parsing a tool's
 * human-readable text as an oracle for Mach-O structure (tests/README.md's
 * lesson on that is about *.macho fixtures parsed for byte-level facts) —
 * the same distinction live_probe.c's own header comment draws.
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
#include <unistd.h>
#include <sys/wait.h>

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
    CHECK(mlive_each_lc(NULL, count_segments_cb, NULL) == 0,
          "mlive_each_lc(NULL, ...) == 0 (refuses rather than walking garbage)");
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
    CHECK(mlive_each_lc(&fake, count_segments_cb, NULL) == 0,
          "mlive_each_lc refuses a bad-magic header rather than walking it");
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

    snprintf(cmd, sizeof cmd, "nm -u %s > %s 2>&1", objpath, nmpath);
    rc = system(cmd);
    CHECK(rc == 0, "nm -u on the probe object succeeds (rc=%d)", rc);

    char *nm_out = slurp(nmpath);
    CHECK(nm_out != NULL, "nm output was captured");
    if (nm_out) {
        static const char *forbidden[] = {
            "malloc", "free", "calloc", "realloc", "reallocf", "valloc",
            "printf", "fprintf", "sprintf", "snprintf", "vprintf",
            "puts", "fputs", "fopen", "fread", "fwrite", "fclose",
            "pthread_mutex", "pthread_create",
            NULL
        };
        for (int i = 0; forbidden[i]; i++) {
            CHECK(strstr(nm_out, forbidden[i]) == NULL,
                  "probe object's undefined symbols do not mention \"%s\" (nm -u said: %s)",
                  forbidden[i], nm_out);
        }
        /* A vacuously-clean check (e.g. nm silently failing to run at all)
         * would pass every CHECK above for the wrong reason. strncmp is the
         * one external symbol live.h legitimately calls (see live.h's own
         * top-of-file comment on why it's async-signal-safe), so its
         * presence is proof nm actually inspected a real, non-empty object
         * rather than an empty or failed run. */
        CHECK(strstr(nm_out, "strncmp") != NULL,
              "sanity: the probe object DOES reference strncmp (proves nm actually ran; got: %s)",
              nm_out);
    }
    free(nm_out);

    remove(objpath);
    remove(nmpath);
}

int main(void) {
    test_own_image_is_valid_64bit();
    test_finds_text_segment();
    test_finds_text_section();
    test_text_section_contains_a_known_function();
    test_each_lc_visits_every_segment();
    test_each_lc_stopping_early_is_reported();
    test_null_header_returns_null_or_zero();
    test_bad_magic_is_rejected();
    test_missing_segment_or_section_returns_null();
    test_probe_object_is_allocation_free();

    if (fails) {
        printf("%d check(s) FAILED\n", fails);
        return 1;
    }
    printf("all live_test checks passed\n");
    return 0;
}
