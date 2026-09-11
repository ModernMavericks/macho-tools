/*
 * tests/edit_test.c — hermetic tests for src/edit.c's me_run.
 *
 * The image is built here by hand (same reasoning as linkedit_test), so this
 * is host-agnostic. What is under test is the EXECUTION MODEL, not the
 * individual operations: statements apply in order, a failure part-way
 * writes nothing, and a dry run writes nothing while still verifying.
 *
 * me_run takes a path, so each test writes its image into a fresh mkdtemp
 * directory and inspects the directory afterwards as well as the file: "wrote
 * nothing" has to mean no replaced file, no --output file, and no temp file
 * left behind by a write that was started and abandoned.
 *
 * The image is tests/mkimplausible.c's shape: __TEXT with one section at
 * 0x400, __DATA with two sections (the second an __init_offsets whose one
 * entry names an initializer), __LINKEDIT, an LC_UUID, and an
 * LC_FUNCTION_STARTS declaring the single function start base + 0x400. With
 * the initializer naming 0x400 the image is plausible; naming 0x999 it is
 * the implausible twin, which mg_plausible refuses. One test adds an empty
 * LC_DYLD_INFO_ONLY, which makes the image one `fixups set classic` passes
 * through as already converted.
 *
 * Build: ctest runs it as edit_test. By hand, compile this file with -Isrc
 * together with every .c file under src/ -- me_run reaches most of them.
 */
#include "edit.h"
#include "image.h"
#include "rewrite.h"
#include "script.h"
#include "mach_compat.h"

#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <libkern/OSByteOrder.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(cond, msg, ...) do { if (!(cond)) { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); fails++; } } while (0)

#define TEXT_VMADDR 0x100000000ULL
#define SECT_OFF    0x400        /* the one address LC_FUNCTION_STARTS names */
#define DATA_OFF    0x1000
#define DATA_SIZE   0x1000
#define LE_OFF      0x2000
#define LE_SIZE     0x1000
#define INIT_OFF    (DATA_OFF + 0x800)
#define FS_SIZE     8
#define IMG_SIZE    (LE_OFF + LE_SIZE)

#define IMPLAUSIBLE 1   /* the initializer names no function start */
#define DYLD_INFO   2   /* carry an (empty) LC_DYLD_INFO_ONLY: already classic */

static void set16(char *field, const char *name) {
    size_t len = strlen(name);
    if (len > 16) len = 16;
    memset(field, 0, 16);
    memcpy(field, name, len);
}

static struct segment_command_64 *put_seg(uint8_t *p, const char *name, uint64_t vmaddr,
                                          uint64_t vmsize, uint64_t fileoff,
                                          uint64_t filesize, uint32_t nsects) {
    struct segment_command_64 *s = (struct segment_command_64 *)p;
    s->cmd = LC_SEGMENT_64;
    s->cmdsize = (uint32_t)(sizeof *s + nsects * sizeof(struct section_64));
    set16(s->segname, name);
    s->vmaddr = vmaddr; s->vmsize = vmsize;
    s->fileoff = fileoff; s->filesize = filesize;
    s->maxprot = 7; s->initprot = 3;
    s->nsects = nsects;
    return s;
}

static void put_sect(struct segment_command_64 *seg, int i, const char *sect,
                     const char *segname, uint64_t addr, uint64_t size,
                     uint32_t offset, uint32_t flags) {
    struct section_64 *s = (struct section_64 *)(seg + 1) + i;
    memset(s, 0, sizeof *s);
    set16(s->sectname, sect);
    set16(s->segname, segname);
    s->addr = addr; s->size = size; s->offset = offset; s->flags = flags;
}

static uint8_t *build_image(int flags) {
    uint8_t *buf = (uint8_t *)calloc(1, IMG_SIZE);
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    h->magic = MH_MAGIC_64;
    h->cputype = CPU_TYPE_X86_64;
    h->cpusubtype = CPU_SUBTYPE_X86_64_ALL;
    h->filetype = MH_DYLIB;
    h->flags = MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL;

    uint8_t *p = buf + sizeof *h;
    uint32_t ncmds = 0;

    struct segment_command_64 *text = put_seg(p, "__TEXT", TEXT_VMADDR, 0x1000, 0, 0x1000, 1);
    put_sect(text, 0, "__text", "__TEXT", TEXT_VMADDR + SECT_OFF, 4, SECT_OFF, 0);
    p += text->cmdsize; ncmds++;

    struct segment_command_64 *data = put_seg(p, "__DATA", TEXT_VMADDR + DATA_OFF, DATA_SIZE,
                                              DATA_OFF, DATA_SIZE, 2);
    put_sect(data, 0, "__data", "__DATA", TEXT_VMADDR + DATA_OFF, 8, DATA_OFF, 0);
    put_sect(data, 1, "__init_offsets", "__DATA", TEXT_VMADDR + INIT_OFF, 4,
             INIT_OFF, S_INIT_FUNC_OFFSETS);
    p += data->cmdsize; ncmds++;

    struct segment_command_64 *le = put_seg(p, "__LINKEDIT", TEXT_VMADDR + LE_OFF, LE_SIZE,
                                            LE_OFF, LE_SIZE, 0);
    p += le->cmdsize; ncmds++;

    struct uuid_command *uu = (struct uuid_command *)p;
    uu->cmd = LC_UUID; uu->cmdsize = sizeof *uu;
    memset(uu->uuid, 0xab, sizeof uu->uuid);
    p += uu->cmdsize; ncmds++;

    struct linkedit_data_command *fs = (struct linkedit_data_command *)p;
    fs->cmd = LC_FUNCTION_STARTS; fs->cmdsize = sizeof *fs;
    fs->dataoff = LE_OFF; fs->datasize = FS_SIZE;
    p += fs->cmdsize; ncmds++;

    if (flags & DYLD_INFO) {
        struct dyld_info_command *di = (struct dyld_info_command *)p;
        di->cmd = LC_DYLD_INFO_ONLY; di->cmdsize = sizeof *di;
        p += di->cmdsize; ncmds++;
    }

    h->ncmds = ncmds;
    h->sizeofcmds = (uint32_t)(p - (buf + sizeof *h));

    /* One function start at base + 0x400 (ULEB128 0x400 is 0x80 0x08). */
    buf[LE_OFF] = 0x80; buf[LE_OFF + 1] = 0x08;

    uint32_t init = (flags & IMPLAUSIBLE) ? 0x999u : SECT_OFF;
    memcpy(buf + INIT_OFF, &init, sizeof init);
    return buf;
}

/* ---- file and directory helpers ---------------------------------------- */

static char g_dir[256];

static void fresh_dir(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/edit_test.XXXXXX", (tmp && *tmp) ? tmp : "/tmp");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); exit(2); }
}

static void in_dir(char *out, size_t outsz, const char *name) {
    snprintf(out, outsz, "%s/%s", g_dir, name);
}

static void write_file(const char *path, const uint8_t *buf, size_t len, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0 || write(fd, buf, len) != (ssize_t)len) { perror(path); exit(2); }
    close(fd);
    chmod(path, mode);
}

static uint8_t *read_file(const char *path, size_t *len) {
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    uint8_t *buf = (uint8_t *)malloc((size_t)st.st_size + 1);
    int fd = open(path, O_RDONLY);
    if (!buf || fd < 0 || read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) {
        perror(path); exit(2);
    }
    close(fd);
    *len = (size_t)st.st_size;
    return buf;
}

/* FNV-1a, 64-bit: the "hash taken up front". A byte-for-byte comparison is
 * made as well; the hash is what a reader can check at a glance in a FAIL
 * line. */
static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

static int dir_entries(void) {
    DIR *d = opendir(g_dir);
    int n = 0;
    struct dirent *e;
    if (!d) { perror(g_dir); exit(2); }
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) n++;
    closedir(d);
    return n;
}

static void rm_dir(void) {
    DIR *d = opendir(g_dir);
    struct dirent *e;
    char p[512];
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(g_dir);
}

/* Everything "left untouched" means, taken before a run and compared after. */
typedef struct {
    uint8_t *bytes;
    size_t   len;
    uint64_t hash;
    ino_t    ino;
    int      entries;
} snap;

static snap take(const char *path) {
    snap s;
    struct stat st;
    s.bytes = read_file(path, &s.len);
    s.hash = fnv1a(s.bytes, s.len);
    stat(path, &st);
    s.ino = st.st_ino;
    s.entries = dir_entries();
    return s;
}

static void check_untouched(const char *what, const char *path, snap *before) {
    size_t len = 0;
    uint8_t *now = read_file(path, &len);
    struct stat st;
    CHECK(now != NULL, "%s: %s still exists", what, path);
    if (now) {
        uint64_t h = fnv1a(now, len);
        CHECK(h == before->hash, "%s: hash changed (%016llx -> %016llx)", what,
              (unsigned long long)before->hash, (unsigned long long)h);
        CHECK(len == before->len && memcmp(now, before->bytes, len) == 0,
              "%s: bytes changed (%zu -> %zu bytes)", what, before->len, len);
    }
    CHECK(stat(path, &st) == 0 && st.st_ino == before->ino,
          "%s: inode changed, so the file was replaced", what);
    CHECK(dir_entries() == before->entries,
          "%s: directory has %d entries, had %d -- a temp or output file was left",
          what, dir_entries(), before->entries);
    free(now);
    free(before->bytes);
}

/* ---- image inspection -------------------------------------------------- */

struct find_ctx { uint32_t cmd; int n; const char *name; };

static int find_cb(const struct load_command *lc, void *ctx_) {
    struct find_ctx *c = ctx_;
    if (lc->cmd != c->cmd) return 0;
    if (c->name) {
        const struct dylib_command *dc = (const struct dylib_command *)lc;
        if (strcmp((const char *)lc + dc->dylib.name.offset, c->name) != 0) return 0;
    }
    c->n++;
    return 0;
}

/* How many load commands of kind `cmd` (and, for a dylib command, naming
 * `name`) the file at `path` has; -1 if it is not a valid image. */
static int count_lc(const char *path, uint32_t cmd, const char *name) {
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    mi_image im;
    struct find_ctx c = { cmd, 0, name };
    if (!buf) return -1;
    if (mi_wrap(buf, len, &im) != 0) { free(buf); return -1; }
    mi_each_lc(&im, find_cb, &c);
    free(buf);
    return c.n;
}

static int has_segment(const char *path, const char *seg, const char *sect_segname) {
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    mi_image im;
    int ok = 0;
    if (!buf) return 0;
    if (mi_wrap(buf, len, &im) == 0) {
        struct segment_command_64 *s = mi_find_segment(&im, seg);
        if (s) {
            ok = 1;
            /* Each section repeats its segment's name; a rename must reach it. */
            if (sect_segname) {
                struct section_64 *sc = (struct section_64 *)(s + 1);
                for (uint32_t i = 0; i < s->nsects; i++)
                    if (strncmp(sc[i].segname, sect_segname, 16) != 0) ok = 0;
            }
        }
    }
    free(buf);
    return ok;
}

/* ---- running a script --------------------------------------------------- */

static char g_log[8192];

/* Parse `text` and run it; the report lands in g_log. */
static int run(const char *path, const char *out, const char *text, int verbose, int dry_run) {
    ms_script s;
    char err[256];
    if (ms_parse(text, strlen(text), &s, err, sizeof err) != 0) {
        printf("FAIL: test script does not parse: %s\n", err);
        fails++;
        return -99;
    }
    FILE *log = tmpfile();
    me_opts o;
    memset(&o, 0, sizeof o);
    o.verbose = verbose;
    o.dry_run = dry_run;
    o.log = log;
    int rc = me_run(path, out, &s, &o);
    fflush(log);
    rewind(log);
    size_t n = fread(g_log, 1, sizeof g_log - 1, log);
    g_log[n] = '\0';
    fclose(log);
    ms_free(&s);
    return rc;
}

/* ---- the tests ----------------------------------------------------------- */

static void test_statements_apply_in_order(void) {
    fresh_dir();
    char path[512];
    in_dir(path, sizeof path, "img");
    uint8_t *img = build_image(0);
    write_file(path, img, IMG_SIZE, 0755);
    free(img);

    int rc = run(path, NULL,
                 "load-command delete uuid\n"
                 "segment rename __DATA __DATX\n", 1, 0);
    CHECK(rc == 0, "in order: a script that succeeds returns 0 (got %d; log: %s)", rc, g_log);
    CHECK(count_lc(path, LC_UUID, NULL) == 0, "in order: LC_UUID was deleted");
    CHECK(has_segment(path, "__DATX", "__DATX"),
          "in order: __DATA was renamed, its sections' copy of the name too");
    CHECK(!has_segment(path, "__DATA", NULL), "in order: no __DATA segment remains");

    const char *first = strstr(g_log, "  load-command delete uuid\n");
    const char *second = strstr(g_log, "  segment rename __DATA __DATX\n");
    const char *verified = strstr(g_log, ": verified\n");
    const char *written = strstr(g_log, ": written (");
    CHECK(first && second && first < second,
          "in order: the log names the statements in script order (log: %s)", g_log);
    CHECK(second && verified && second < verified,
          "in order: verification is reported after the last statement (log: %s)", g_log);
    CHECK(verified && written && verified < written,
          "in order: the write is reported after verification (log: %s)", g_log);
    rm_dir();
}

/* THE PROPERTY THE WHOLE DESIGN EXISTS TO BUY. The first statement succeeds
 * and changes the in-memory image; the second is refused on its own merits (a
 * load command too long for the header pad, with no allow-grow). Nothing may
 * reach the disk: not the input rewritten, not an --output created, not a
 * temp file abandoned. The input is compared by hash, byte for byte, and by
 * inode, because a rename-based write would keep the bytes of a successful
 * rewrite but never the inode. */
static void test_a_failure_part_way_writes_nothing(void) {
    fresh_dir();
    char path[512], out[512], script[1024];
    in_dir(path, sizeof path, "img");
    in_dir(out, sizeof out, "img.out");
    uint8_t *img = build_image(0);
    write_file(path, img, IMG_SIZE, 0755);
    free(img);

    /* 600 bytes of path: well past the ~500 bytes of header pad. */
    char longpath[640];
    memset(longpath, 'x', sizeof longpath);
    longpath[0] = '/';
    longpath[600] = '\0';
    /* Statement 2 is on source line 4: the refusal must name both. */
    snprintf(script, sizeof script,
             "# harmless first\n"
             "load-command delete uuid\n"
             "\n"
             "dylib append %s\n", longpath);

    snap before = take(path);
    int rc = run(path, NULL, script, 1, 0);
    CHECK(rc == MR_REFUSED, "part-way: a refused second statement returns MR_REFUSED (got %d)", rc);
    check_untouched("part-way, in place", path, &before);
    CHECK(strstr(g_log, "  load-command delete uuid\n") != NULL,
          "part-way: the first statement did run before the refusal (log: %s)", g_log);
    CHECK(strstr(g_log, "refused at statement 2 of 2 (line 4)") != NULL,
          "part-way: the refusal names statement 2 of 2 and its source line (log: %s)", g_log);
    CHECK(strstr(g_log, "left unmodified") != NULL,
          "part-way: the refusal says the file was left unmodified (log: %s)", g_log);
    CHECK(strstr(g_log, "written (") == NULL,
          "part-way: nothing claims a write happened (log: %s)", g_log);

    /* Unconditional: the refusal line is not a --verbose detail. */
    before = take(path);
    rc = run(path, NULL, script, 0, 0);
    CHECK(rc == MR_REFUSED, "part-way, quiet: still MR_REFUSED (got %d)", rc);
    check_untouched("part-way, quiet", path, &before);
    CHECK(strstr(g_log, "refused at statement 2 of 2 (line 4)") != NULL,
          "part-way, quiet: the refusal line prints without --verbose (log: %s)", g_log);

    /* With --output: the input stays as it was and OUT is never created. */
    before = take(path);
    rc = run(path, out, script, 0, 0);
    CHECK(rc == MR_REFUSED, "part-way, --output: MR_REFUSED (got %d)", rc);
    check_untouched("part-way, --output", path, &before);
    CHECK(access(out, F_OK) != 0 && errno == ENOENT,
          "part-way, --output: %s was not created", out);
    rm_dir();
}

static void test_dry_run_writes_nothing_but_still_verifies(void) {
    fresh_dir();
    char path[512], out[512];
    in_dir(path, sizeof path, "img");
    in_dir(out, sizeof out, "img.out");
    uint8_t *img = build_image(0);
    write_file(path, img, IMG_SIZE, 0755);
    free(img);

    snap before = take(path);
    int rc = run(path, NULL, "load-command delete uuid\n", 0, 1);
    CHECK(rc == 0, "dry run: a script that would succeed returns 0 (got %d; log: %s)", rc, g_log);
    check_untouched("dry run", path, &before);
    CHECK(strstr(g_log, "NOT written (--dry-run) -- would be ") != NULL,
          "dry run: says it did not write, without --verbose (log: %s)", g_log);

    before = take(path);
    rc = run(path, out, "load-command delete uuid\n", 0, 1);
    CHECK(rc == 0, "dry run, --output: returns 0 (got %d)", rc);
    check_untouched("dry run, --output", path, &before);
    CHECK(access(out, F_OK) != 0, "dry run, --output: %s was not created", out);

    /* "Still verifies": the same dry run over an image the final verify
     * refuses is refused, exactly as the real run would be. A segment rename
     * is used because the rename step itself skips mg_plausible, so the only
     * thing that can refuse this run is me_run's own verify. */
    img = build_image(IMPLAUSIBLE);
    write_file(path, img, IMG_SIZE, 0755);
    free(img);
    before = take(path);
    rc = run(path, NULL, "segment rename __DATA __DATX\n", 0, 1);
    CHECK(rc == MR_REFUSED, "dry run: an image that fails verification is refused (got %d)", rc);
    check_untouched("dry run, implausible", path, &before);
    CHECK(strstr(g_log, "NOT written") == NULL,
          "dry run: a refused run does not report a skipped write (log: %s)", g_log);
    rm_dir();
}

/* The final verify has no escape hatch. MACHO_NO_VERIFY is a documented
 * opt-out of the check inside the dylib/rpath/lc rewrite step; it must not
 * reach the gate that runs after the last statement. */
static void test_the_final_verify_ignores_MACHO_NO_VERIFY(void) {
    fresh_dir();
    char path[512];
    in_dir(path, sizeof path, "img");
    uint8_t *img = build_image(IMPLAUSIBLE);
    write_file(path, img, IMG_SIZE, 0755);
    free(img);

    setenv("MACHO_NO_VERIFY", "1", 1);
    snap before = take(path);
    int rc = run(path, NULL, "segment rename __DATA __DATX\n", 1, 0);
    unsetenv("MACHO_NO_VERIFY");
    CHECK(rc == MR_REFUSED, "no escape hatch: MACHO_NO_VERIFY=1 does not skip the final "
          "verify (got %d)", rc);
    check_untouched("no escape hatch", path, &before);
    CHECK(strstr(g_log, "verified\n") == NULL,
          "no escape hatch: the log does not claim the image verified (log: %s)", g_log);
    CHECK(strstr(g_log, "refused at verification") != NULL,
          "no escape hatch: the refusal names verification (log: %s)", g_log);
    rm_dir();
}

/* Sequential, not batched: a statement sees what the one before it did. In
 * one batched operation set the replace could never match the command the
 * append creates, and fatal-warnings would refuse the run. */
static void test_later_statements_see_earlier_ones(void) {
    fresh_dir();
    char path[512];
    in_dir(path, sizeof path, "img");
    uint8_t *img = build_image(0);
    write_file(path, img, IMG_SIZE, 0755);
    free(img);

    int rc = run(path, NULL,
                 "fatal-warnings\n"
                 "dylib append /usr/lib/libfoo.dylib\n"
                 "dylib replace /usr/lib/libfoo.dylib /usr/lib/libbar.dylib\n", 0, 0);
    CHECK(rc == 0, "sequential: the replace matched the appended dylib (got %d)", rc);
    CHECK(count_lc(path, LC_LOAD_DYLIB, "/usr/lib/libbar.dylib") == 1,
          "sequential: the result loads libbar");
    CHECK(count_lc(path, LC_LOAD_DYLIB, "/usr/lib/libfoo.dylib") == 0,
          "sequential: nothing still loads libfoo");
    rm_dir();
}

/* An operation that matched nothing is a report without fatal-warnings and
 * a refusal with it. */
static void test_fatal_warnings_refuses_an_unmatched_operation(void) {
    fresh_dir();
    char path[512];
    in_dir(path, sizeof path, "img");
    uint8_t *img = build_image(0);
    write_file(path, img, IMG_SIZE, 0755);
    free(img);

    snap before = take(path);
    int rc = run(path, NULL,
                 "fatal-warnings\n"
                 "load-command delete uuid\n"
                 "dylib delete /definitely/not/linked.dylib\n", 0, 0);
    CHECK(rc == MR_REFUSED, "fatal-warnings: an unmatched operation refuses (got %d)", rc);
    check_untouched("fatal-warnings", path, &before);

    rc = run(path, NULL,
             "load-command delete uuid\n"
             "dylib delete /definitely/not/linked.dylib\n", 0, 0);
    CHECK(rc == 0, "without fatal-warnings: the run continues and succeeds (got %d)", rc);
    CHECK(count_lc(path, LC_UUID, NULL) == 0,
          "without fatal-warnings: the statements that matched were applied");
    rm_dir();
}

/* version-min, swift-abi and fixups were reachable only through file-level
 * entry points; each must run against the in-memory image. */
static void test_the_file_level_operations_run_in_memory(void) {
    fresh_dir();
    char path[512];
    in_dir(path, sizeof path, "img");
    uint8_t *img = build_image(DYLD_INFO);
    write_file(path, img, IMG_SIZE, 0755);

    int rc = run(path, NULL,
                 "fixups set classic\n"
                 "version-min set 10.9\n"
                 "swift-abi set legacy\n", 0, 0);
    CHECK(rc == 0, "in memory: an already-classic image passes fixups set classic, "
          "then gains a version-min (got %d; log: %s)", rc, g_log);
    CHECK(count_lc(path, LC_VERSION_MIN_MACOSX, NULL) == 1,
          "in memory: LC_VERSION_MIN_MACOSX was appended");
    {
        size_t len = 0;
        uint8_t *now = read_file(path, &len);
        CHECK(now && len == IMG_SIZE,
              "in memory: a pass-through fixups statement leaves the size alone (got %zu)", len);
        free(now);
    }
    free(img);

    /* No chained fixups and no LC_DYLD_INFO_ONLY: there is nothing to lower
     * and nothing already lowered, which the conversion refuses. */
    img = build_image(0);
    write_file(path, img, IMG_SIZE, 0755);
    free(img);
    snap before = take(path);
    rc = run(path, NULL, "version-min set 10.9\nfixups set classic\n", 0, 0);
    CHECK(rc == MR_REFUSED, "in memory: fixups set classic with nothing to lower refuses (got %d)", rc);
    check_untouched("fixups refused", path, &before);
    rm_dir();
}

static void test_output_leaves_the_input_alone(void) {
    fresh_dir();
    char path[512], out[512];
    in_dir(path, sizeof path, "img");
    in_dir(out, sizeof out, "img.out");
    uint8_t *img = build_image(0);
    write_file(path, img, IMG_SIZE, 0751);
    free(img);

    snap before = take(path);
    before.entries++;   /* the output file is the one expected newcomer */
    int rc = run(path, out, "load-command delete uuid\n", 0, 0);
    CHECK(rc == 0, "--output: returns 0 (got %d)", rc);
    check_untouched("--output input", path, &before);
    CHECK(count_lc(out, LC_UUID, NULL) == 0, "--output: the edit landed in the output");
    struct stat st;
    CHECK(stat(out, &st) == 0 && (st.st_mode & 07777) == 0751,
          "--output: the output takes the input's mode (got %o)", (unsigned)(st.st_mode & 07777));
    rm_dir();
}

/* Only a thin 64-bit Mach-O is accepted. A fat one is refused, saying why; so
 * is anything else; an input that cannot be read is an error, not a refusal. */
static void test_only_a_thin_image_is_accepted(void) {
    fresh_dir();
    char path[512];
    in_dir(path, sizeof path, "fat");

    /* A one-slice fat container around the ordinary thin image. */
    size_t fatlen = 0x1000 + IMG_SIZE;
    uint8_t *fat = (uint8_t *)calloc(1, fatlen);
    uint8_t *thin = build_image(0);
    struct fat_header *fh = (struct fat_header *)fat;
    struct fat_arch *fa = (struct fat_arch *)(fh + 1);
    fh->magic = OSSwapHostToBigInt32(FAT_MAGIC);
    fh->nfat_arch = OSSwapHostToBigInt32(1);
    fa->cputype = (cpu_type_t)OSSwapHostToBigInt32(CPU_TYPE_X86_64);
    fa->cpusubtype = (cpu_subtype_t)OSSwapHostToBigInt32(CPU_SUBTYPE_X86_64_ALL);
    fa->offset = OSSwapHostToBigInt32(0x1000);
    fa->size = OSSwapHostToBigInt32(IMG_SIZE);
    fa->align = OSSwapHostToBigInt32(12);
    memcpy(fat + 0x1000, thin, IMG_SIZE);
    free(thin);
    write_file(path, fat, fatlen, 0755);
    free(fat);

    snap before = take(path);
    int rc = run(path, NULL, "load-command delete uuid\n", 0, 0);
    CHECK(rc == MR_REFUSED, "thin only: a fat input is refused (got %d)", rc);
    check_untouched("fat input", path, &before);
    CHECK(strstr(g_log, "fat") != NULL, "thin only: the refusal says the input is fat (log: %s)", g_log);

    in_dir(path, sizeof path, "text");
    write_file(path, (const uint8_t *)"not a Mach-O at all\n", 20, 0644);
    before = take(path);
    rc = run(path, NULL, "load-command delete uuid\n", 0, 0);
    CHECK(rc == MR_REFUSED, "thin only: a non-Mach-O is refused (got %d)", rc);
    check_untouched("non-Mach-O input", path, &before);

    in_dir(path, sizeof path, "absent");
    int entries = dir_entries();
    rc = run(path, NULL, "load-command delete uuid\n", 0, 0);
    CHECK(rc == MR_FAIL, "thin only: an absent input is an error, MR_FAIL (got %d)", rc);
    CHECK(access(path, F_OK) != 0 && dir_entries() == entries,
          "thin only: nothing was created for an absent input");
    rm_dir();
}

int main(void) {
    test_statements_apply_in_order();
    test_a_failure_part_way_writes_nothing();
    test_dry_run_writes_nothing_but_still_verifies();
    test_the_final_verify_ignores_MACHO_NO_VERIFY();
    test_later_statements_see_earlier_ones();
    test_fatal_warnings_refuses_an_unmatched_operation();
    test_the_file_level_operations_run_in_memory();
    test_output_leaves_the_input_alone();
    test_only_a_thin_image_is_accepted();

    printf("edit_test: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
