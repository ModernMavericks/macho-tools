/* fat.c — validate and read a fat (universal) Mach-O's arch table. See fat.h. */

#include "fat.h"

#include <mach-o/fat.h>
#include <libkern/OSByteOrder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t mfat_swap32(uint32_t v) { return OSSwapInt32(v); }

int mfat_parse(const uint8_t *buf, size_t size, uint32_t *narch_out, int *swapped_out) {
    if (size < sizeof(struct fat_header)) return MFAT_MALFORMED;

    uint32_t magic = *(const uint32_t *)buf;
    if (magic != FAT_MAGIC && magic != FAT_CIGAM) return MFAT_MALFORMED;
    int swap = (magic == FAT_CIGAM);

    const struct fat_header *fh = (const struct fat_header *)buf;
    uint32_t narch = swap ? mfat_swap32(fh->nfat_arch) : fh->nfat_arch;

    /* Widen to uint64_t before multiplying, same reasoning as image.c's
     * LC_SEGMENT_64/nsects check: the maximum product fits easily in 64
     * bits, so this bound can never itself overflow. */
    uint64_t region = (uint64_t)sizeof(struct fat_header) +
                       (uint64_t)narch * sizeof(struct fat_arch);
    if (region > size) return MFAT_MALFORMED;

    /* Collect every entry's offset+size, validating each individually
     * against the file and the header/table region as before, so a second
     * pass can also check that no two DECLARED slices overlap EACH OTHER.
     * That is a read-side malformation independent of what any caller
     * intends to do with the file: two arch entries that already alias the
     * same bytes describe an ill-formed fat file, and catching that here is
     * cheaper than every writer having to reason about what a rewrite of it
     * should mean (this is exactly what let change_dylib's reassembly
     * silently place two REWRITTEN slices at the same output offset --
     * closed on the write side in process_fat, and closed here too so a
     * malformed INPUT is refused before either tool does anything with it). */
    uint32_t *offs = NULL, *sizes = NULL;
    if (narch) {
        offs  = (uint32_t *)malloc((size_t)narch * sizeof(uint32_t));
        sizes = (uint32_t *)malloc((size_t)narch * sizeof(uint32_t));
        if (!offs || !sizes) { free(offs); free(sizes); return MFAT_IO_ERROR; }
    }

    const struct fat_arch *ar = (const struct fat_arch *)(buf + sizeof(struct fat_header));
    for (uint32_t i = 0; i < narch; i++) {
        uint32_t o = swap ? mfat_swap32((uint32_t)ar[i].offset) : (uint32_t)ar[i].offset;
        uint32_t s = swap ? mfat_swap32((uint32_t)ar[i].size)   : (uint32_t)ar[i].size;
        /* A slice cannot start before the region that describes it -- that
         * would let it alias the fat header/arch table itself. */
        if ((uint64_t)o < region) { free(offs); free(sizes); return MFAT_MALFORMED; }
        if ((uint64_t)o + s > size) { free(offs); free(sizes); return MFAT_MALFORMED; }
        offs[i] = o; sizes[i] = s;
    }

    for (uint32_t i = 0; i < narch; i++) {
        uint64_t a0 = offs[i], a1 = a0 + sizes[i];
        for (uint32_t j = i + 1; j < narch; j++) {
            uint64_t b0 = offs[j], b1 = b0 + sizes[j];
            if (a0 < b1 && b0 < a1) { free(offs); free(sizes); return MFAT_MALFORMED; }
        }
    }
    free(offs); free(sizes);

    *narch_out = narch;
    *swapped_out = swap;
    return 0;
}

void mfat_get(const uint8_t *buf, int swapped, uint32_t idx, mfat_arch *out) {
    const struct fat_arch *ar =
        (const struct fat_arch *)(buf + sizeof(struct fat_header));
    const struct fat_arch *a = &ar[idx];
    out->offset      = swapped ? mfat_swap32((uint32_t)a->offset)      : (uint32_t)a->offset;
    out->size        = swapped ? mfat_swap32((uint32_t)a->size)        : (uint32_t)a->size;
    out->cputype     = swapped ? mfat_swap32((uint32_t)a->cputype)     : (uint32_t)a->cputype;
    out->cpusubtype  = swapped ? mfat_swap32((uint32_t)a->cpusubtype)  : (uint32_t)a->cpusubtype;
    out->align       = swapped ? mfat_swap32(a->align)                 : a->align;
}

int mfat_rewrite(uint8_t **pbuf, size_t *psize, uint32_t narch, int swapped,
                 mfat_slice_fn fn, mfat_placed_fn placed, void *ctx, int *modified) {
    *modified = 0;
    const uint8_t *buf = *pbuf;
    size_t n = narch ? narch : 1;
    uint8_t   **sbuf  = (uint8_t **)calloc(n, sizeof *sbuf);
    size_t     *ssize = (size_t *)calloc(n, sizeof *ssize);
    mfat_arch  *arch  = (mfat_arch *)calloc(n, sizeof *arch);
    uint64_t   *noff  = (uint64_t *)calloc(n, sizeof *noff);
    uint8_t    *newbuf = NULL;
    uint32_t    held = 0;   /* how many sbuf[] entries are allocated */
    int rc = 0;

    if (!sbuf || !ssize || !arch || !noff) {
        fprintf(stderr, "ERROR: out of memory\n");
        rc = MFAT_IO_ERROR;
        goto out;
    }
    for (uint32_t i = 0; i < narch; i++) {
        mfat_get(buf, swapped, i, &arch[i]);
        sbuf[i] = (uint8_t *)malloc(arch[i].size ? arch[i].size : 1);
        if (!sbuf[i]) { fprintf(stderr, "ERROR: out of memory\n"); rc = MFAT_IO_ERROR; goto out; }
        held = i + 1;
        memcpy(sbuf[i], buf + arch[i].offset, arch[i].size);
        ssize[i] = arch[i].size;
        int changed = 0;
        int frc = fn(&sbuf[i], &ssize[i], &arch[i], i, &changed, ctx);
        if (frc != 0) { rc = frc; goto out; }
        if (changed) *modified = 1;
    }
    if (!*modified) goto out;

    /* Reassemble: each slice keeps its original offset until some earlier
     * slice's size actually changed; from then on later slices pack
     * sequentially, honoring each slice's own (preserved) alignment.
     *
     * `cursor` tracks where the NEXT slice may start, which only means
     * "the end of the file" when the arch table happens to be in ascending
     * offset order -- nothing in the fat format requires that (lipo merely
     * happens to emit it that way). A fat file with, say, arch[0] at a
     * HIGHER offset than arch[1] is legal and both entries can independently
     * pass the offset+size-in-bounds check in mfat_parse. Sizing the output
     * buffer from `cursor` (the LAST slice processed) instead of the
     * MAXIMUM end across every slice undersizes the allocation whenever the
     * table isn't ascending, and the memcpy below then writes past it --
     * heap corruption in the best case, and an exit-0 write of a truncated,
     * silently-corrupted file in the worst, since the caller would then write
     * exactly `newbuf`'s (too-small) size back over the real input. Track
     * the true maximum explicitly so the allocation is never smaller than
     * every slice it has to hold, regardless of table order. */
    int shift = 0;
    uint64_t cursor = 0, max_end = 0;
    for (uint32_t j = 0; j < narch; j++) {
        uint64_t want;
        if (!shift) {
            want = arch[j].offset;
        } else {
            uint32_t shift_amt = arch[j].align > 31 ? 31 : arch[j].align;  /* hostile input guard */
            uint64_t a = (uint64_t)1 << shift_amt;
            want = (cursor + a - 1) & ~(a - 1);
        }
        noff[j] = want;
        cursor = want + ssize[j];
        if (cursor > max_end) max_end = cursor;
        if (ssize[j] != arch[j].size) shift = 1;
    }

    /* Refuse rather than guess: an unshifted slice keeps its ORIGINAL offset
     * unconditionally (see above), but a later, SHIFTED slice's sequential
     * packing has no idea where that still-fixed slice sits -- on a
     * non-ascending table it can walk a shifted slice's new range right on
     * top of a still-fixed one's. That is silent data loss with an exit 0
     * (the final memcpy below would just overwrite one slice's bytes with
     * another's) -- exactly the failure class the previous fix closed the
     * memory-safety half of; this closes the correctness half. Check every
     * pair -- not just neighbors in table order, since the colliding pair
     * need not be adjacent -- BEFORE allocating or writing anything, so a
     * refusal here leaves the input completely untouched. */
    for (uint32_t a = 0; a < narch; a++) {
        uint64_t a0 = noff[a], a1 = a0 + ssize[a];
        for (uint32_t b = a + 1; b < narch; b++) {
            uint64_t b0 = noff[b], b1 = b0 + ssize[b];
            if (a0 < b1 && b0 < a1) {
                fprintf(stderr, "ERROR: reassembly would place arch %u [%llu,%llu) and "
                                "arch %u [%llu,%llu) at overlapping offsets; refusing "
                                "rather than guess a different layout\n",
                        a, (unsigned long long)a0, (unsigned long long)a1,
                        b, (unsigned long long)b0, (unsigned long long)b1);
                rc = MFAT_MALFORMED;
                goto out;
            }
        }
    }

    newbuf = (uint8_t *)calloc(1, (size_t)max_end);
    if (!newbuf) {
        fprintf(stderr, "ERROR: out of memory reassembling the fat file\n");
        rc = MFAT_IO_ERROR;
        goto out;
    }
    struct fat_header *nfh = (struct fat_header *)newbuf;
    nfh->magic = swapped ? mfat_swap32(FAT_MAGIC) : FAT_MAGIC;
    nfh->nfat_arch = swapped ? mfat_swap32(narch) : narch;
    struct fat_arch *nar = (struct fat_arch *)(newbuf + sizeof(struct fat_header));
    for (uint32_t j = 0; j < narch; j++) {
        uint32_t o = (uint32_t)noff[j], s = (uint32_t)ssize[j];
        nar[j].cputype    = (cpu_type_t)(swapped ? mfat_swap32(arch[j].cputype) : arch[j].cputype);
        nar[j].cpusubtype = (cpu_subtype_t)(swapped ? mfat_swap32(arch[j].cpusubtype) : arch[j].cpusubtype);
        nar[j].offset = swapped ? mfat_swap32(o) : o;
        nar[j].size   = swapped ? mfat_swap32(s) : s;
        nar[j].align  = swapped ? mfat_swap32(arch[j].align) : arch[j].align;
        memcpy(newbuf + noff[j], sbuf[j], ssize[j]);
        if (placed) placed(&arch[j], j, noff[j], ssize[j], ctx);
    }
    free(*pbuf);
    *pbuf = newbuf;
    *psize = (size_t)max_end;   /* NOT cursor -- see the comment above the layout loop */

out:
    for (uint32_t j = 0; j < held; j++) free(sbuf[j]);
    free(sbuf); free(ssize); free(arch); free(noff);
    return rc;
}
