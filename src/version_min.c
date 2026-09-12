/*
 * mv_ -- see version_min.h. This started as compat/add_version_min.c's
 * main(); only the argument check stayed behind in that tool. It now takes
 * an allow_grow flag: with it set, a short header pad is grown instead of
 * refused, via mg_ensure_pad (src/grow.h), whose own "ERROR: ... growing the
 * header needs allow-grow" line precedes this file's "no room for
 * LC_VERSION_MIN_MACOSX" when growth isn't permitted. Its in-memory middle
 * is mv_add_version_min_image, so an edit script can apply it to a buffer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>

#include "version_min.h"
#include "image.h"
#include "grow.h"
#include "atomic_write.h"   /* wa_write_new: `path` is read, `out` is written */
#include "rewrite.h"    /* MR_REFUSED/MR_FAIL: this function's own exit-code
                         * vocabulary, shared with mr_apply_file -- see its
                         * comment there for the dividing line this follows. */

struct mv_scan {
    uint32_t first_sect_off;   /* lowest nonzero section file offset;
                                 * UINT32_MAX if no section has one. Only
                                 * that sentinel is consulted: mg_ensure_pad
                                 * finds the pad's bound for itself. */
    int      has_version_min;
};

static int mv_scan_lc(const struct load_command *lc, void *ctx_) {
    struct mv_scan *ctx = ctx_;
    if (lc->cmd == LC_SEGMENT_64) {
        const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
        const struct section_64 *sect = (const struct section_64 *)(seg + 1);
        for (uint32_t j = 0; j < seg->nsects; j++)
            if (sect[j].offset && sect[j].offset < ctx->first_sect_off)
                ctx->first_sect_off = sect[j].offset;
    } else if (lc->cmd == LC_VERSION_MIN_MACOSX) {
        ctx->has_version_min = 1;
    }
    return 0;   /* nothing here ever needs to stop the walk early */
}

/* cli/macho9.c's cmd_minos forwards this function's return value verbatim,
 * passing through its own allow_grow flag, the same arrangement mr_apply_file
 * has with dylib/rpath/lc -- so every return below is MR_REFUSED or MR_FAIL,
 * the same two codes and the same dividing line mr_apply_file's own comment
 * (rewrite.h) draws: MR_FAIL for this function's own open/fstat, for
 * mi_open's own I/O (MI_IO_ERROR, below) and for wa_write_new's failure to
 * produce `out`; MR_REFUSED for every site that examined the file and
 * declined, including mi_open's MI_NOT_MACHO and "no room for
 * LC_VERSION_MIN_MACOSX". */
int mv_add_version_min(const char *path, const char *out, int allow_grow) {
    /* Opened only to report an unreadable `path` immediately, before any
     * analysis, in the words the historical tool's own open() produced;
     * mi_open (O_RDONLY too) does the actual read and validation. Nothing is
     * ever written through this descriptor -- `path` is an input now -- so it
     * is closed again at once and the result goes to `out`. */
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return MR_FAIL; }
    struct stat st0;
    if (fstat(fd, &st0) != 0) { perror("fstat"); close(fd); return MR_FAIL; }
    close(fd);

    mi_image im;
    int mo_rc = mi_open(path, &im);
    if (mo_rc == MI_IO_ERROR) {
        /* The open()/fstat() above only proved this path opens, not that
         * mi_open's own independent open, read of the whole file, or the
         * malloc it reads into will succeed too -- any of those, or an
         * actual TOCTOU race, land here. Not a considered refusal either
         * way. */
        fprintf(stderr, "%s: cannot open or read\n", path);
        return MR_FAIL;
    }
    if (mo_rc != 0) {
        fprintf(stderr, "%s: not a readable 64-bit Mach-O\n", path);
        return MR_REFUSED;
    }

    /* The edit itself, in memory; what is left here is the file around it.
     * mi_release first: growing may reallocate the buffer, and the image
     * wrapper must not be left owning a pointer that realloc moved. */
    size_t fsize = im.size;
    uint8_t *buf = mi_release(&im);
    int added = 0;
    int rc = mv_add_version_min_image(&buf, &fsize, allow_grow, path, &added);
    if (rc != 0) {
        free(buf);
        return rc;
    }

    /* Written even when nothing was added: a 0 exit means `out` is the
     * answer, so it has to exist either way. wa_write_new creates it afresh
     * from `path`'s mode, owner and xattrs and never touches `path`. */
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    int wr = wa_write_new(path, out, buf, fsize);
    if (wr == WA_IS_INPUT) { free(buf); return MR_FAIL; }   /* checked earlier; a path changed */
    if (wr != 0) { free(buf); return MR_FAIL; }
    if (added)
        printf("Added LC_VERSION_MIN_MACOSX 10.9 (ncmds=%u, sizeofcmds=%u)\n",
               hdr->ncmds, hdr->sizeofcmds);
    printf("Wrote %s (%zu bytes)\n", out, fsize);
    free(buf);
    return 0;
}

/* See version_min.h. mv_add_version_min's former middle, moved rather than
 * copied: the scan, the "already present" and "no room" answers, and the
 * append, all against the caller's buffer and none of the file around it. */
int mv_add_version_min_image(uint8_t **pbuf, size_t *psize, int allow_grow,
                             const char *label, int *out_added) {
    *out_added = 0;
    mi_image im;
    if (mi_wrap(*pbuf, *psize, &im) != 0) {
        fprintf(stderr, "not a readable 64-bit Mach-O\n");
        return MR_REFUSED;
    }
    struct mach_header_64 *hdr = im.hdr;

    struct mv_scan scan = { UINT32_MAX, 0 };
    mi_each_lc(&im, mv_scan_lc, &scan);

    if (scan.has_version_min) {
        printf("LC_VERSION_MIN_MACOSX already present; nothing to do.\n");
        return 0;
    }

    uint32_t lc_end   = sizeof(*hdr) + hdr->sizeofcmds;
    uint32_t need_end = lc_end + (uint32_t)sizeof(struct version_min_command);
    /* Two ways "no room" is true that growing cannot cure, both refused
     * before anything is written: no section anywhere has a nonzero file
     * offset (scan.first_sect_off is still its UINT32_MAX sentinel, and a
     * write would go off whatever end the buffer has), or the command would
     * run past the buffer itself -- mi_wrap validates load commands, not
     * section file ranges, so first_sect_off is an untrusted value read
     * straight from the file. Fixed after a real heap overflow: a 104-byte
     * file (header + one LC_SEGMENT_64, nsects=0) hit exactly the first case
     * and wrote 16 bytes past a buffer whose allocation was exactly
     * file-sized; see tests/leaf-tool-crashes.sh. */
    if (scan.first_sect_off == UINT32_MAX || need_end > *psize) {
        fprintf(stderr, "no room for LC_VERSION_MIN_MACOSX\n");
        return MR_REFUSED;
    }
    /* Whether the command fits in the pad before the first section, and if
     * not whether to grow it, is mg_ensure_pad's to decide, the same as for
     * every other load-command edit. It returns 0 at once, untouched, when
     * the command fits, and it refuses an image whose first section lies
     * past the buffer's end. */
    if (mg_ensure_pad(pbuf, psize, need_end, allow_grow, label) != 0) {
        fprintf(stderr, "no room for LC_VERSION_MIN_MACOSX\n");
        return MR_REFUSED;
    }
    hdr = (struct mach_header_64 *)*pbuf;   /* growth may have reallocated it */

    struct version_min_command *vm = (struct version_min_command *)(*pbuf + lc_end);
    memset(vm, 0, sizeof(*vm));
    vm->cmd = LC_VERSION_MIN_MACOSX;
    vm->cmdsize = sizeof(*vm);
    vm->version = (10 << 16) | (9 << 8);   /* 10.9.0 */
    vm->sdk     = (10 << 16) | (9 << 8);
    hdr->ncmds++;
    hdr->sizeofcmds += sizeof(*vm);
    *out_added = 1;
    return 0;
}
