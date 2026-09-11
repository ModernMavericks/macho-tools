/* fat.h — validate and read a fat (universal) Mach-O's arch table.
 *
 * Both change_dylib and fix_macho need to tell a fat file's slices apart;
 * until now each had its own hand-rolled fat_header/fat_arch walk, and they
 * disagreed about validation -- fix_macho trusted an arch's offset/size
 * outright (an out-of-bounds READ on a malformed fat file), change_dylib
 * checked them. That is exactly the class of bug this toolkit's shared src/
 * layer exists to rule out (see image.h's file header, and Task 3's two
 * independently-diverging deletion predicates). This is the one place both
 * now go through.
 *
 * Scope: reading and validating the arch table only. Writing a fresh
 * fat_header/fat_arch (change_dylib's -grow reassembly) is specific to that
 * one caller and stays there. */

#ifndef MACHO9_FAT_H
#define MACHO9_FAT_H

#include <stdint.h>
#include <stddef.h>

/* One arch entry, already decoded to host-native byte order. */
typedef struct {
    uint32_t offset;
    uint32_t size;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t align;
} mfat_arch;

/* mfat_parse's two failure reasons -- the same distinction image.h's
 * MI_IO_ERROR/MI_NOT_MACHO draws, for the same reason (cli/macho9.c's
 * EX_REFUSED/EX_FAIL, src/rewrite.c's/src/version_min.c's MR_REFUSED/
 * MR_FAIL). mfat_parse takes an already-read buffer, not a path, so it has
 * no open/fstat/read of its own to fail -- its ONE environment failure is
 * the two `malloc`s it uses to track slice offsets/sizes while checking for
 * overlaps. Every other failure (bad magic, arch table past the end, a
 * slice out of bounds or overlapping the header/table, two slices
 * overlapping each other) is a decision about the file's own content. */
#define MFAT_IO_ERROR   (-1)
#define MFAT_MALFORMED  (-2)

/* Validate buf[0..size) as a fat (universal) Mach-O: magic is FAT_MAGIC or
 * FAT_CIGAM, the fat_arch table (narch entries) fits inside `size`, EVERY
 * entry's offset+size is in bounds AND does not start before the end of the
 * fat header + arch table (a slice cannot overlap the very structure that
 * describes it -- a hostile or corrupt file could otherwise claim a slice
 * that aliases the header), AND no two entries' [offset, offset+size) ranges
 * overlap EACH OTHER (a fat file whose own arch table already aliases two
 * slices is malformed on the read side, independent of what any caller
 * intends to do with it). Returns 0 on success and sets *narch_out and
 * *swapped_out (nonzero if the file is byte-swapped relative to this host --
 * true for every real fat file read on a little-endian machine). Returns
 * MFAT_IO_ERROR or MFAT_MALFORMED on failure (see those constants above)
 * and touches neither output; it prints nothing, so the caller can phrase
 * its own diagnostic. */
int mfat_parse(const uint8_t *buf, size_t size, uint32_t *narch_out, int *swapped_out);

/* Fetch arch `idx`'s fields, byte-order-corrected to host-native. `idx` must
 * be < the narch a prior mfat_parse(buf, size, ...) returned successfully --
 * that call already proved every entry's offset+size is in bounds, so this
 * does no further checking of its own. */
void mfat_get(const uint8_t *buf, int swapped, uint32_t idx, mfat_arch *out);

#endif /* MACHO9_FAT_H */
