/* linkedit.h — the __LINKEDIT offset-bump table.
 *
 * macho_grow.h's image-base trick (see its own header comment) lowers the
 * image base by `grow` bytes and inserts `grow` bytes of file space at
 * `insert`. Every VM address stays fixed, but every FILE OFFSET at or past
 * `insert` must move down by `grow` to keep pointing at the same bytes --
 * and __LINKEDIT is where nearly all of those offsets live: the symbol
 * table, the string table, the indirect-symbol table, the rebase/bind/
 * lazy-bind/export streams, function starts, data-in-code, the code
 * signature, and their less common siblings (split-info, code-sign DRs,
 * linker-optimization hints, the standalone export trie, chained fixups).
 * This module is that exhaustive list, in one place, so it cannot drift
 * between a hand-rolled `mg_bump` call site here and another one there --
 * exactly the drift class this extraction exists to retire.
 *
 * Deliberately narrow: LC_SEGMENT_64's own fileoff/vmaddr/vmsize and its
 * sections' offset/reloff, and LC_MAIN's entryoff, stay in macho_grow.h --
 * they are about repositioning the header pad and the entry point, not
 * about __LINKEDIT's own resident structures, and the task that created
 * this module scoped it to the latter only.
 */
#ifndef MACHO9_LINKEDIT_H
#define MACHO9_LINKEDIT_H

#include <stdint.h>

#include "image.h"

/* Shift one file-offset field down by `grow` if it points at/after
 * `insert`; leaves it untouched otherwise (a field of 0, meaning "this
 * stream is absent", is always < insert and so is never bumped). */
void ml_bump(uint32_t *off, uint32_t insert, uint32_t grow);

/* Walk every load command in `im` (via mi_each_lc) and bump each
 * __LINKEDIT-resident structure's file-offset field(s) that qualify, by
 * `grow`:
 *   LC_SYMTAB              symoff, stroff
 *   LC_DYSYMTAB             tocoff, modtaboff, extrefsymoff, indirectsymoff,
 *                           extreloff, locreloff
 *                           (the symbol-table INDEX/COUNT fields --
 *                           ilocalsym, nlocalsym, iextdefsym, nextdefsym,
 *                           iundefsym, nundefsym, nlocrel, nextrel,
 *                           nindirectsyms, nextrefsyms, ntoc, nmodtab -- are
 *                           not file offsets and are never touched)
 *   LC_DYLD_INFO[_ONLY]    rebase_off, bind_off, weak_bind_off,
 *                           lazy_bind_off, export_off
 *   LC_FUNCTION_STARTS, LC_DATA_IN_CODE, LC_CODE_SIGNATURE,
 *   LC_SEGMENT_SPLIT_INFO, LC_DYLIB_CODE_SIGN_DRS,
 *   LC_LINKER_OPTIMIZATION_HINT, LC_DYLD_EXPORTS_TRIE,
 *   LC_DYLD_CHAINED_FIXUPS  dataoff (linkedit_data_command)
 * Every other load command carries no file offset this module knows about
 * and is left alone -- same "refuse rather than guess" posture as the rest
 * of this toolkit, just expressed as "do nothing" rather than an error,
 * because an unrecognized load command with no offset field is not a
 * hazard the way an unrecognized SECTION TYPE is elsewhere in this codebase.
 *
 * `im` must already be wrapped/opened over the buffer at its FINAL size --
 * the size after the insert -- so mi_each_lc's own bounds checking
 * (established at mi_wrap/mi_open time) still matches the buffer being
 * edited. The walk never stops early (every command is independent, so
 * there is nothing to refuse partway through); mi_each_lc's completion
 * result is not meaningful here and is not checked. */
void ml_bump_all(mi_image *im, uint32_t insert, uint32_t grow);

#endif /* MACHO9_LINKEDIT_H */
