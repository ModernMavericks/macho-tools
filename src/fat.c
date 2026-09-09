/* fat.c — validate and read a fat (universal) Mach-O's arch table. See fat.h. */

#include "fat.h"

#include <mach-o/fat.h>
#include <libkern/OSByteOrder.h>
#include <stdlib.h>

static uint32_t mfat_swap32(uint32_t v) { return OSSwapInt32(v); }

int mfat_parse(const uint8_t *buf, size_t size, uint32_t *narch_out, int *swapped_out) {
    if (size < sizeof(struct fat_header)) return 1;

    uint32_t magic = *(const uint32_t *)buf;
    if (magic != FAT_MAGIC && magic != FAT_CIGAM) return 1;
    int swap = (magic == FAT_CIGAM);

    const struct fat_header *fh = (const struct fat_header *)buf;
    uint32_t narch = swap ? mfat_swap32(fh->nfat_arch) : fh->nfat_arch;

    /* Widen to uint64_t before multiplying, same reasoning as image.c's
     * LC_SEGMENT_64/nsects check: the maximum product fits easily in 64
     * bits, so this bound can never itself overflow. */
    uint64_t region = (uint64_t)sizeof(struct fat_header) +
                       (uint64_t)narch * sizeof(struct fat_arch);
    if (region > size) return 1;

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
        if (!offs || !sizes) { free(offs); free(sizes); return 1; }
    }

    const struct fat_arch *ar = (const struct fat_arch *)(buf + sizeof(struct fat_header));
    for (uint32_t i = 0; i < narch; i++) {
        uint32_t o = swap ? mfat_swap32((uint32_t)ar[i].offset) : (uint32_t)ar[i].offset;
        uint32_t s = swap ? mfat_swap32((uint32_t)ar[i].size)   : (uint32_t)ar[i].size;
        /* A slice cannot start before the region that describes it -- that
         * would let it alias the fat header/arch table itself. */
        if ((uint64_t)o < region) { free(offs); free(sizes); return 1; }
        if ((uint64_t)o + s > size) { free(offs); free(sizes); return 1; }
        offs[i] = o; sizes[i] = s;
    }

    for (uint32_t i = 0; i < narch; i++) {
        uint64_t a0 = offs[i], a1 = a0 + sizes[i];
        for (uint32_t j = i + 1; j < narch; j++) {
            uint64_t b0 = offs[j], b1 = b0 + sizes[j];
            if (a0 < b1 && b0 < a1) { free(offs); free(sizes); return 1; }
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
