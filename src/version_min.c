/*
 * mv_ -- see version_min.h. This is compat/add_version_min.c's former main(),
 * unchanged in behaviour and in every message it prints; only the argument
 * check stayed behind in that tool. Its in-memory middle is
 * mv_add_version_min_image, so an edit script can apply it to a buffer.
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
#include "rewrite.h"    /* MR_REFUSED/MR_FAIL: this function's own exit-code
                         * vocabulary, shared with mr_apply_file -- see its
                         * comment there for the dividing line this follows. */

struct mv_scan {
    uint32_t first_sect_off;   /* upper bound of header pad; UINT32_MAX if no
                                 * section has a nonzero file offset */
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

/* cli/macho9.c's cmd_minos forwards this function's return value verbatim
 * (`return mv_add_version_min(path);`), the same arrangement mr_apply_file
 * has with dylib/rpath/lc -- so every return below is MR_REFUSED or MR_FAIL,
 * the same two codes and the same dividing line mr_apply_file's own comment
 * (rewrite.h) draws: MR_FAIL for this function's own open/fstat/write, for
 * mi_open's own I/O (MI_IO_ERROR, below), and for the race guard below (both
 * a failed stat() and a dev/ino mismatch -- the mismatch is the same
 * "changed underneath us mid-run" condition src/swift_retag.c calls
 * MSWIFT_RACED and reports as EX_FAIL, not EX_REFUSED, for the identical
 * reason); MR_REFUSED for every site that examined the file and declined,
 * including mi_open's MI_NOT_MACHO and "no room for
 * LC_VERSION_MIN_MACOSX". */
int mv_add_version_min(const char *path, int allow_grow) {
    /* Open O_RDWR early so an unwritable file fails immediately, before any
     * analysis; mi_open (O_RDONLY) does the actual read and validation, same
     * split as change_dylib and patch_macho use. */
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return MR_FAIL; }
    struct stat st0;
    if (fstat(fd, &st0) != 0) { perror("fstat"); close(fd); return MR_FAIL; }

    mi_image im;
    int mo_rc = mi_open(path, &im);
    if (mo_rc == MI_IO_ERROR) {
        /* The open()/fstat() above only proved this path opens, not that
         * mi_open's own independent open, read of the whole file, or the
         * malloc it reads into will succeed too -- any of those, or an
         * actual TOCTOU race, land here. Not a considered refusal either
         * way. */
        fprintf(stderr, "%s: cannot open or read\n", path);
        close(fd);
        return MR_FAIL;
    }
    if (mo_rc != 0) {
        fprintf(stderr, "%s: not a readable 64-bit Mach-O\n", path);
        close(fd);
        return MR_REFUSED;
    }

    /* mi_open reads `path` through its OWN, separate O_RDONLY descriptor --
     * necessarily, since mi_open only ever opens by path -- so the bytes
     * just validated and the fd this tool writes back through (opened
     * above) are, between them, two different opens of whatever `path`
     * named at each moment. If something replaces `path` in between (a
     * concurrent install, a symlink retarget), this tool would read the NEW
     * file's bytes but write them into the OLD file's inode via the
     * already-open fd -- silently, since both opens report success. This
     * narrows that window: refuse rather than proceed if `path` no longer
     * names the same inode the O_RDWR fd above opened. It does not close
     * the window entirely (path could still change between this check and
     * the write below), only the gap mi_open's own re-open introduced. */
    struct stat st1;
    if (stat(path, &st1) != 0 ||
        st1.st_dev != st0.st_dev || st1.st_ino != st0.st_ino) {
        /* Both halves are MR_FAIL, not MR_REFUSED: a failed stat() here is a
         * plain syscall failure, and a dev/ino mismatch is this function's
         * version of MSWIFT_RACED (src/swift_retag.c) -- "the file changed
         * under us mid-run" is an environment condition, not a judgement
         * about the file's content, and retag-swift already reports its own
         * identical race as EX_FAIL for exactly that reason. */
        fprintf(stderr, "%s: changed underneath us between open and validation; refusing\n", path);
        mi_close(&im);
        close(fd);
        return MR_FAIL;
    }

    /* The edit itself, in memory; what is left here is the file around it.
     * mi_release first: growing may reallocate the buffer, and the image
     * wrapper must not be left owning a pointer that realloc moved. */
    size_t fsize = im.size;
    uint8_t *buf = mi_release(&im);
    int added = 0;
    int rc = mv_add_version_min_image(&buf, &fsize, allow_grow, &added);
    if (rc != 0 || !added) {
        free(buf);
        close(fd);
        return rc;
    }

    /* A grown image is larger than the file it was read from; writing it
     * from offset 0 extends the file. The write is in place, through the
     * descriptor the race guard above checked -- a failed write can leave
     * the file partly written, exactly as before growth existed. */
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    lseek(fd, 0, SEEK_SET);
    if (write(fd, buf, fsize) != (ssize_t)fsize) { perror("write"); free(buf); close(fd); return MR_FAIL; }
    close(fd);
    printf("Added LC_VERSION_MIN_MACOSX 10.9 (ncmds=%u, sizeofcmds=%u)\n",
           hdr->ncmds, hdr->sizeofcmds);
    free(buf);
    return 0;
}

/* See version_min.h. mv_add_version_min's former middle, moved rather than
 * copied: the scan, the "already present" and "no room" answers, and the
 * append, all against the caller's buffer and none of the file around it. */
int mv_add_version_min_image(uint8_t **pbuf, size_t *psize, int allow_grow,
                             int *out_added) {
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
    /* The third way is the ordinary one -- the pad before the first section
     * is too small -- and that is mg_ensure_pad's to decide, grow or refuse,
     * the same as for every other load-command edit. */
    if (need_end > scan.first_sect_off) {
        if (mg_ensure_pad(pbuf, psize, need_end, allow_grow, "LC_VERSION_MIN_MACOSX") != 0) {
            fprintf(stderr, "no room for LC_VERSION_MIN_MACOSX\n");
            return MR_REFUSED;
        }
        hdr = (struct mach_header_64 *)*pbuf;   /* growth reallocated the buffer */
    }

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
