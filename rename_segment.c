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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <mach-o/loader.h>

#include "image.h"

struct rs_ctx {
    const char *oldname;
    const char *newname;
    int renamed;
};

/* Renames EVERY LC_SEGMENT_64 whose segname matches oldname, not just the
 * first -- a binary that has already been through this tool once (or has
 * duplicate segment names for any other reason) can legitimately have more
 * than one match, and each one's sections need the same rename. That rules
 * out mi_find_segment, which only ever returns the first match.
 *
 * This edits segname/sectname content in place but never touches lc->cmd or
 * lc->cmdsize, so it cannot desynchronize mi_each_lc's own `p += lc->cmdsize`
 * stride -- the thing "must not modify the command chain" in image.h's
 * comment is guarding against. */
static void rs_rename_lc(const struct load_command *lc, void *ctx_) {
    struct rs_ctx *ctx = ctx_;
    if (lc->cmd != LC_SEGMENT_64) return;
    struct segment_command_64 *seg = (struct segment_command_64 *)lc;
    if (strncmp(seg->segname, ctx->oldname, 16) != 0) return;

    memset(seg->segname, 0, 16);
    strncpy(seg->segname, ctx->newname, 16);
    /* Each section repeats its segment's name; getsectiondata matches on the
     * section's copy, so it has to change too. */
    struct section_64 *sects = (struct section_64 *)(seg + 1);
    for (uint32_t s = 0; s < seg->nsects; s++) {
        memset(sects[s].segname, 0, 16);
        strncpy(sects[s].segname, ctx->newname, 16);
    }
    ctx->renamed++;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s binary OLDNAME NEWNAME\n", argv[0]);
        return 1;
    }
    const char *path = argv[1], *oldname = argv[2], *newname = argv[3];
    if (strlen(newname) > 16) {
        fprintf(stderr, "new segment name longer than 16 bytes\n");
        return 1;
    }

    /* Open O_RDWR early so an unwritable file fails immediately, before any
     * analysis; mi_open (O_RDONLY) does the actual read and validation, same
     * split as change_dylib and patch_macho use. */
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    mi_image im;
    if (mi_open(path, &im) != 0) {
        fprintf(stderr, "%s: not a readable 64-bit Mach-O\n", path);
        close(fd);
        return 1;
    }
    size_t fsize = im.size;

    struct rs_ctx ctx = { oldname, newname, 0 };
    mi_each_lc(&im, rs_rename_lc, &ctx);

    /* mi_release, not the image, owns the buffer from here: this tool wrote
     * straight into it above and eventually free()s it. */
    uint8_t *buf = mi_release(&im);

    if (!ctx.renamed) { free(buf); close(fd); return 2; }   /* nothing to do */

    if (lseek(fd, 0, SEEK_SET) != 0 ||
        write(fd, buf, fsize) != (ssize_t)fsize) {
        perror("write"); free(buf); close(fd); return 1;
    }
    free(buf); close(fd);
    printf("%s: renamed %d segment(s) %s -> %s\n", path, ctx.renamed, oldname, newname);
    return 0;
}
