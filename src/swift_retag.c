/*
 * mswift_ -- see swift_retag.h. This is compat/retag_swift_classes.c's former
 * process() and the three helpers only it used, unchanged in behaviour and in
 * every message it prints; the argv loop, the per-file report and the exit
 * code stayed behind in that tool.
 *
 * The one thing that did change is the return VALUE: process() collapsed "not
 * a Mach-O" and "raced" into the same 0 it uses for "nothing to retag", which
 * left a caller unable to tell a refusal from a silent success. Those two now
 * have their own codes (swift_retag.h). compat/retag_swift_classes.c maps them
 * back onto the same "skip quietly, not an error" it always did, so nothing it
 * prints or returns moved; cli/macho9.c's `retag-swift` verb reports them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>

#include "swift_retag.h"
#include "image.h"

#define IS_SWIFT_STABLE 2
#define IS_SWIFT_LEGACY 1

struct mswift_seg { uint64_t vmaddr, vmsize, fileoff; };

/* class_t: isa, superclass, cache, vtable, data -- data is at offset 32. */
#define CLASS_ISA_OFFSET   0
#define CLASS_DATA_OFFSET 32

struct mswift_find_ctx {
    const char *want_seg, *want_sect;
    uint64_t   *out_off, *out_size;
    int         found;
    struct mswift_seg *segs;
    int        *nsegs;
};

/* Every LC_SEGMENT_64 gets collected into segs[] (needed afterward for
 * mswift_file_off()'s address translation, over segments other than the one
 * matched here) AND checked section-by-section against want_seg/want_sect --
 * both segname and sectname, not just a segment-name lookup followed by a
 * section-name lookup within it. That is deliberately NOT the same as
 * mi_find_segment(im, want_seg) + mi_find_section: this toolkit's own segment
 * rename (src/segname.h) can leave a binary with two segments sharing a name
 * (see compat/rename_segment.c's header comment), and mi_find_segment only
 * ever returns the first. A binary that has been through that rename first
 * (__DATA_CONST -> __DATA) would then have the wanted section in the SECOND
 * "__DATA" segment, which mi_find_section's first-match-only search would miss
 * entirely. Scanning every segment's every section, as the original
 * hand-rolled walk did, finds it regardless of which same-named segment it
 * landed in or what order they come in. */
static int mswift_find_section_lc(const struct load_command *lc, void *ctx_) {
    struct mswift_find_ctx *ctx = ctx_;
    if (lc->cmd != LC_SEGMENT_64) return 0;
    const struct segment_command_64 *sc = (const struct segment_command_64 *)lc;
    if (*ctx->nsegs < 64) {
        ctx->segs[*ctx->nsegs].vmaddr  = sc->vmaddr;
        ctx->segs[*ctx->nsegs].vmsize  = sc->vmsize;
        ctx->segs[*ctx->nsegs].fileoff = sc->fileoff;
        (*ctx->nsegs)++;
    }
    const struct section_64 *s = (const struct section_64 *)(sc + 1);
    for (uint32_t k = 0; k < sc->nsects; k++) {
        if (strncmp(s[k].segname, ctx->want_seg, 16) == 0 &&
            strncmp(s[k].sectname, ctx->want_sect, 16) == 0) {
            *ctx->out_off = s[k].offset;
            *ctx->out_size = s[k].size;
            ctx->found = 1;
        }
    }
    /* Must keep scanning every segment even after a match: the comment above
     * explains why the wanted section can legitimately live in a LATER
     * same-named segment, and segs[] needs every segment regardless. */
    return 0;
}

static int mswift_find_section(const mi_image *im, const char *want_seg, const char *want_sect,
                               uint64_t *out_off, uint64_t *out_size,
                               struct mswift_seg *segs, int *nsegs) {
    *nsegs = 0;
    struct mswift_find_ctx ctx = { want_seg, want_sect, out_off, out_size, 0, segs, nsegs };
    mi_each_lc(im, mswift_find_section_lc, &ctx);
    return ctx.found;
}

static int64_t mswift_file_off(struct mswift_seg *segs, int nsegs, uint64_t va) {
    for (int i = 0; i < nsegs; i++)
        if (va >= segs[i].vmaddr && va < segs[i].vmaddr + segs[i].vmsize)
            return (int64_t)(segs[i].fileoff + (va - segs[i].vmaddr));
    return -1;
}

/* True if [off, off+len) fits entirely inside a buffer of size `fsize`,
 * without the addition itself overflowing. mi_open validates load commands
 * -- cmdsize bounds/alignment, LC_SEGMENT_64/nsects agreement -- but nothing
 * about a SECTION's file range, or an address (a class record's file offset,
 * here) derived from one; every such offset this code computes needs its
 * own bound before it's dereferenced, since mi_open never proved one. */
static int mswift_in_bounds(uint64_t fsize, uint64_t off, uint64_t len) {
    return off <= fsize && len <= fsize - off;
}

/* Retag one class record in place; returns 1 if it changed. co is a file
 * offset translated from a class virtual address via segment vmaddr/fileoff
 * -- neither mi_open-validated -- so it still needs its own bound before the
 * 8-byte data word at co+CLASS_DATA_OFFSET is read. */
static int mswift_retag(uint8_t *buf, size_t fsize, struct mswift_seg *segs, int nsegs,
                        uint64_t class_va) {
    int64_t co = mswift_file_off(segs, nsegs, class_va);
    if (co < 0 || !mswift_in_bounds(fsize, (uint64_t)co + CLASS_DATA_OFFSET, sizeof(uint64_t))) return 0;
    uint64_t *data = (uint64_t *)(buf + co + CLASS_DATA_OFFSET);
    if ((*data & 3) != IS_SWIFT_STABLE) return 0;
    *data = (*data & ~(uint64_t)3) | IS_SWIFT_LEGACY;
    return 1;
}

int mswift_retag_file(const char *path) {
    /* Open O_RDWR early so an unwritable file fails immediately; mi_open
     * (O_RDONLY) does the actual read and validation, same split as
     * change_dylib and patch_macho use. */
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror(path); return MSWIFT_ERROR; }
    struct stat st0;
    if (fstat(fd, &st0) != 0) { perror("fstat"); close(fd); return MSWIFT_ERROR; }

    mi_image im;
    if (mi_open(path, &im) != 0) {
        /* Not a (validly-formed) 64-bit Mach-O -- nothing to do here, exactly
         * as the old magic-only check decided for a bad magic. mi_open
         * additionally catches cmdsize/alignment/segment malformations the
         * old check never looked at; those are refused here too rather than
         * walked with undefined behavior. Nothing is printed: the multi-file
         * front-end wants to keep going quietly, and the single-file one
         * wants to word the refusal itself. */
        close(fd);
        return MSWIFT_NOT_MACHO;
    }

    /* mi_open reads `path` through its own, separate O_RDONLY descriptor, so
     * the bytes just validated and the fd written back through (opened
     * above) are two different opens of whatever `path` named at each
     * moment -- see src/version_min.c's identical check for the full
     * reasoning. Refuse rather than write the newly-validated bytes into a
     * possibly different inode than the one that was opened. */
    struct stat st1;
    if (stat(path, &st1) != 0 ||
        st1.st_dev != st0.st_dev || st1.st_ino != st0.st_ino) {
        fprintf(stderr, "%s: changed underneath us between open and validation; skipping\n", path);
        mi_close(&im);
        close(fd);
        return MSWIFT_RACED;
    }

    size_t fsize = im.size;

    struct mswift_seg segs[64];
    int nsegs = 0;
    uint64_t listoff = 0, listsize = 0;
    int changed = 0;

    static const char *lists[] = { "__objc_classlist", "__objc_nlclslist" };
    for (size_t li = 0; li < sizeof(lists)/sizeof(lists[0]); li++) {
        /* Modern linkers place the list in __DATA_CONST; a port may already
         * have renamed that segment to __DATA, so accept either. */
        if (!mswift_find_section(&im, "__DATA", lists[li], &listoff, &listsize, segs, &nsegs) &&
            !mswift_find_section(&im, "__DATA_CONST", lists[li], &listoff, &listsize, segs, &nsegs))
            continue;
        /* The section's own offset/size are file data mi_open never
         * validated -- a section can legitimately claim a range past the
         * file (or one that overflows the addition), and this code used to
         * index straight into it. Skip this list rather than crash; see
         * tests/leaf-tool-crashes.sh's oobsection fixture. */
        if (!mswift_in_bounds(fsize, listoff, listsize)) continue;

        for (uint64_t i = 0; i + 8 <= listsize; i += 8) {
            uint64_t cls_va = *(uint64_t *)(im.buf + listoff + i);
            if (!cls_va) continue;
            changed += mswift_retag(im.buf, fsize, segs, nsegs, cls_va);
            /* The metaclass carries the same tag and is reached via isa. */
            int64_t co = mswift_file_off(segs, nsegs, cls_va);
            if (co >= 0 && mswift_in_bounds(fsize, (uint64_t)co + CLASS_ISA_OFFSET, sizeof(uint64_t))) {
                uint64_t meta_va = *(uint64_t *)(im.buf + co + CLASS_ISA_OFFSET);
                if (meta_va) changed += mswift_retag(im.buf, fsize, segs, nsegs, meta_va);
            }
        }
    }

    /* mi_release, not the image, owns the buffer from here: this function
     * wrote straight into it above (mswift_retag()) and eventually free()s it. */
    uint8_t *buf = mi_release(&im);

    if (changed) {
        if (lseek(fd, 0, SEEK_SET) != 0 ||
            write(fd, buf, fsize) != (ssize_t)fsize) {
            perror("write"); free(buf); close(fd); return MSWIFT_ERROR;
        }
    }
    free(buf);
    close(fd);
    return changed;
}
