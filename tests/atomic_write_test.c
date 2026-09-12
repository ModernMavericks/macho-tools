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
