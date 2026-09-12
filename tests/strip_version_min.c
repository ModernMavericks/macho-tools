/* tests/strip_version_min.c -- remove the FIRST LC_VERSION_MIN_MACOSX load
 * command from a Mach-O file, in place: memmove the load commands after it
 * down over it, zero the freed tail bytes (they become header pad), and fix
 * up ncmds/sizeofcmds.
 *
 * A FIXTURE BUILDER, for the tests that need a binary machotool minos (and
 * add_version_min) has something to do to. A fixture linked with
 * -mmacosx-version-min=10.9 by a 10.9 linker already CARRIES the one load
 * command those tools add, so without this a "it added the command"
 * assertion passes against a tool that does nothing at all.
 *
 * Direct structure surgery, compiled by plain $CC with no special flags --
 * the same idiom tests/change_dylib_test.sh's ordinal_of.c uses. Not `machotool
 * lc -delete` or change_dylib's -strip-lc: neither vocabulary covers
 * LC_VERSION_MIN_MACOSX, and building a test's fixture with the tool under
 * test would be circular anyway.
 *
 * The GOAL is a fixture that LACKS LC_VERSION_MIN_MACOSX, not "removed one".
 * A 2026 linker emits LC_BUILD_VERSION instead of LC_VERSION_MIN_MACOSX in
 * the first place (a 10.9-era linker emits the latter), so on a cross host
 * there is nothing to strip -- the goal is already met. That is SUCCESS,
 * not an error: exit 0 either way. Only a genuine failure to remove one
 * that IS present is exit 2. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s FILE\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 2; }
    size_t size = (size_t)st.st_size;
    uint8_t *buf = malloc(size);
    if (!buf || read(fd, buf, size) != (ssize_t)size) {
        fprintf(stderr, "read failed\n"); close(fd); return 2;
    }
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    if (hdr->magic != MH_MAGIC_64) { fprintf(stderr, "not a 64-bit Mach-O\n"); return 2; }

    uint8_t *lcp = buf + sizeof(*hdr);
    uint32_t found_off = 0, found_size = 0;
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_VERSION_MIN_MACOSX) {
            found_off = (uint32_t)(lcp - buf);
            found_size = lc->cmdsize;
            break;
        }
        lcp += lc->cmdsize;
    }
    if (!found_size) { printf("no LC_VERSION_MIN_MACOSX present; nothing to strip (goal already met)\n"); return 0; }

    uint32_t lc_end = (uint32_t)sizeof(*hdr) + hdr->sizeofcmds;
    uint32_t after = found_off + found_size;
    memmove(buf + found_off, buf + after, lc_end - after);
    memset(buf + lc_end - found_size, 0, found_size);
    hdr->ncmds -= 1;
    hdr->sizeofcmds -= found_size;

    lseek(fd, 0, SEEK_SET);
    if (write(fd, buf, size) != (ssize_t)size) { perror("write"); return 2; }
    close(fd);
    return 0;
}
