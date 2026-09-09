/*
 * Append an LC_VERSION_MIN_MACOSX load command targeting 10.9. patch_macho
 * strips LC_BUILD_VERSION and leaves no platform declaration; 10.9's dyld uses
 * that signal for some behaviors (including, possibly, TLV handling).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <mach-o/loader.h>

#include "image.h"

struct avm_scan {
    uint32_t first_sect_off;   /* upper bound of header pad; UINT32_MAX if no
                                 * section has a nonzero file offset */
    int      has_version_min;
};

static void avm_scan_lc(const struct load_command *lc, void *ctx_) {
    struct avm_scan *ctx = ctx_;
    if (lc->cmd == LC_SEGMENT_64) {
        const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
        const struct section_64 *sect = (const struct section_64 *)(seg + 1);
        for (uint32_t j = 0; j < seg->nsects; j++)
            if (sect[j].offset && sect[j].offset < ctx->first_sect_off)
                ctx->first_sect_off = sect[j].offset;
    } else if (lc->cmd == LC_VERSION_MIN_MACOSX) {
        ctx->has_version_min = 1;
    }
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "Usage: %s binary\n", argv[0]); return 1; }
    const char *path = argv[1];

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
    struct mach_header_64 *hdr = im.hdr;

    struct avm_scan scan = { UINT32_MAX, 0 };
    mi_each_lc(&im, avm_scan_lc, &scan);

    /* mi_release, not the image, owns the buffer from here: this tool writes
     * the new command straight into it and eventually free()s it. */
    uint8_t *buf = mi_release(&im);

    if (scan.has_version_min) {
        printf("LC_VERSION_MIN_MACOSX already present; nothing to do.\n");
        free(buf);
        close(fd);
        return 0;
    }

    uint32_t lc_end = sizeof(*hdr) + hdr->sizeofcmds;
    if (lc_end + sizeof(struct version_min_command) > scan.first_sect_off) {
        fprintf(stderr, "no room for LC_VERSION_MIN_MACOSX\n");
        free(buf);
        close(fd);
        return 1;
    }

    struct version_min_command *vm = (struct version_min_command *)(buf + lc_end);
    memset(vm, 0, sizeof(*vm));
    vm->cmd = LC_VERSION_MIN_MACOSX;
    vm->cmdsize = sizeof(*vm);
    vm->version = (10 << 16) | (9 << 8);   /* 10.9.0 */
    vm->sdk     = (10 << 16) | (9 << 8);
    hdr->ncmds++;
    hdr->sizeofcmds += sizeof(*vm);

    lseek(fd, 0, SEEK_SET);
    if (write(fd, buf, fsize) != (ssize_t)fsize) { perror("write"); free(buf); close(fd); return 1; }
    close(fd);
    printf("Added LC_VERSION_MIN_MACOSX 10.9 (ncmds=%u, sizeofcmds=%u)\n",
           hdr->ncmds, hdr->sizeofcmds);
    free(buf);
    return 0;
}
