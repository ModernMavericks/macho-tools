/*
 * Rename a Mach-O segment, and the segname recorded in each of its sections.
 *
 * The reason this exists: 10.9's Objective-C runtime finds an image's metadata
 * by asking for named sections of the __DATA segment -- __objc_imageinfo,
 * __objc_classlist, __objc_catlist, __objc_protolist and the rest. Linkers
 * from Xcode 10 onward place those in __DATA_CONST instead (a segment that
 * later dyld versions re-protect read-only once binding is done). 10.9's
 * libobjc does not look there, so it concludes the image contains no
 * Objective-C at all and skips it: classes go unregistered and, more subtly,
 * the __objc_selrefs entries are never fixed up. Each selref then still holds
 * a pointer to its method-name string rather than a registered SEL, and the
 * first message sent through one dies with
 *
 *   NSForwarding: warning: selector (0x...) for message 'foo:' does not match
 *   selector known to Objective C runtime
 *
 * Renaming __DATA_CONST to __DATA puts the sections where the runtime looks.
 * It is safe because the two segments carry the same protections (initprot
 * read+write); __DATA_CONST differs only in that a newer dyld hardens it after
 * fixups, which 10.9's dyld never does either way. The resulting image has two
 * segments named __DATA, which is legal -- section lookup is by the
 * (segment, section) name pair, and no section name appears in both.
 *
 * Usage: rename_segment binary OLDNAME NEWNAME
 *
 * The new name must be no longer than the 16 bytes a segname field holds.
 *
 * The rename itself is mseg_rename_image (src/segname.h), shared with
 * cli/macho9.c's `segment` verb so the two front-ends cannot drift apart about
 * what renaming a segment means. What is left here is only what differs: this
 * tool's argument grammar, its thin-only mi_open + lseek/write driver, its
 * exit 2 when nothing matched, and its message. macho9's verb routes the same
 * rename through mr_apply_file instead, which is what gives it fat containers
 * and an atomic write-back; this tool has never had either and does not gain
 * them here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "image.h"
#include "segname.h"

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s binary OLDNAME NEWNAME\n", argv[0]);
        return 1;
    }
    const char *path = argv[1], *oldname = argv[2], *newname = argv[3];
    if (!mseg_name_fits(newname)) {
        fprintf(stderr, "new segment name longer than %d bytes\n", MSEG_NAME_MAX);
        return 1;
    }

    /* Open O_RDWR early so an unwritable file fails immediately, before any
     * analysis; mi_open (O_RDONLY) does the actual read and validation, same
     * split as change_dylib and patch_macho use. */
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st0;
    if (fstat(fd, &st0) != 0) { perror("fstat"); close(fd); return 1; }

    mi_image im;
    if (mi_open(path, &im) != 0) {
        fprintf(stderr, "%s: not a readable 64-bit Mach-O\n", path);
        close(fd);
        return 1;
    }

    /* mi_open reads `path` through its own, separate O_RDONLY descriptor, so
     * the bytes just validated and the fd this tool writes back through
     * (opened above) are two different opens of whatever `path` named at
     * each moment -- see add_version_min.c's identical check for the full
     * reasoning. Refuse rather than write the newly-validated bytes into a
     * possibly different (or vanished) inode than the one that was opened. */
    struct stat st1;
    if (stat(path, &st1) != 0 ||
        st1.st_dev != st0.st_dev || st1.st_ino != st0.st_ino) {
        fprintf(stderr, "%s: changed underneath us between open and validation; refusing\n", path);
        mi_close(&im);
        close(fd);
        return 1;
    }

    size_t fsize = im.size;

    int renamed = mseg_rename_image(&im, oldname, newname);

    /* mi_release, not the image, owns the buffer from here: this tool wrote
     * straight into it above and eventually free()s it. */
    uint8_t *buf = mi_release(&im);

    if (!renamed) { free(buf); close(fd); return 2; }   /* nothing to do */

    if (lseek(fd, 0, SEEK_SET) != 0 ||
        write(fd, buf, fsize) != (ssize_t)fsize) {
        perror("write"); free(buf); close(fd); return 1;
    }
    free(buf); close(fd);
    printf("%s: renamed %d segment(s) %s -> %s\n", path, renamed, oldname, newname);
    return 0;
}
