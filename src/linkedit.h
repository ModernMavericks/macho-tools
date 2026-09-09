/* linkedit.h — the __LINKEDIT offset-bump table, moved verbatim out of
 * macho_grow.h. See macho_grow.c/.h's own header comment for the image-base
 * trick this supports; this module is just the exhaustive list of
 * __LINKEDIT-resident file-offset fields every grow must shift.
 *
 * Names still carry macho_grow.h's own `mg_` prefix in this commit -- this
 * is a pure relocation, not yet a rename. The next commit renames to `ml_`
 * and converts the walk to mi_each_lc.
 */
#ifndef MACHO9_LINKEDIT_H
#define MACHO9_LINKEDIT_H

#include <stdint.h>
#include <mach-o/loader.h>

/* Shift one file-offset field down by `grow` if it points at/after `insert`. */
void mg_bump(uint32_t *off, uint32_t insert, uint32_t grow);

/* Walk every load command in `buf`/`hdr` and bump each __LINKEDIT-resident
 * structure's file-offset field(s) that qualify, by `grow`. Verbatim copy of
 * macho_grow.h's own switch cases for LC_SYMTAB, LC_DYSYMTAB,
 * LC_DYLD_INFO[_ONLY], LC_FUNCTION_STARTS, LC_DATA_IN_CODE,
 * LC_CODE_SIGNATURE, LC_SEGMENT_SPLIT_INFO, LC_DYLIB_CODE_SIGN_DRS,
 * LC_LINKER_OPTIMIZATION_HINT, LC_DYLD_EXPORTS_TRIE, LC_DYLD_CHAINED_FIXUPS.
 * LC_SEGMENT_64 (fileoff/vmaddr/vmsize, section offset/reloff) and LC_MAIN
 * (entryoff) stay in macho_grow.h -- they are about the header-pad
 * repositioning and the entry point, not __LINKEDIT's own structures.
 * `hdr->ncmds` load commands starting at `buf + sizeof(*hdr)`. */
void mg_bump_all(uint8_t *buf, const struct mach_header_64 *hdr,
                  uint32_t insert, uint32_t grow);

#endif /* MACHO9_LINKEDIT_H */
