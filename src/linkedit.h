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
 * linker-optimization hints, the standalone export trie, chained fixups,
 * the two-level namespace hint table, and an encrypted range's bounds).
 * This module is that list. It was already single-sourced before this
 * extraction -- at the commit this module was carved out of, all 17
 * `mg_bump` calls sat in one contiguous switch inside one function,
 * `mg_grow_header` -- so the improvement here is not deduplication of a
 * scattered copy; it is that the table now has its own file and its own
 * hermetic test (tests/linkedit_test.c) instead of being untestable
 * except by exercising the rest of `mg_grow_header` around it.
 *
 * NOT exhaustive by construction, on purpose: LC_NOTE and LC_ATOM_INFO also
 * carry a load-command-relative file offset, but this module does not touch
 * either. LC_ATOM_INFO's shape (reported elsewhere as a plain
 * linkedit_data_command) and LC_NOTE's (a note_command, with a uint64_t
 * offset/size pair, per publicly documented dyld/ld64 source) could not be
 * verified against any header available while this module was written --
 * the 10.9 SDK and the modern host SDK on hand both predate both commands.
 * Rather than guess a struct layout this module cannot check, growing a
 * file that carries either is refused outright by macho_grow.h's
 * mg_classify, before mg_grow_header ever reaches this module -- same
 * "refuse rather than guess" rule as an unclassified load command. See
 * mg_classify's LC_NOTE/LC_ATOM_INFO cases for the refusal text.
 *
 * Deliberately narrow otherwise: LC_SEGMENT_64's own fileoff/vmaddr/vmsize
 * and its sections' offset/reloff, and LC_MAIN's entryoff, stay in
 * macho_grow.h -- they are about repositioning the header pad and the
 * entry point, not about __LINKEDIT's own resident structures, and the
 * task that created this module scoped it to the latter only.
 */
#ifndef MACHO9_LINKEDIT_H
#define MACHO9_LINKEDIT_H

#include <stdint.h>

#include "image.h"

/* Load commands whose ENTIRE __LINKEDIT footprint is one or more plain file-
 * offset fields, with no VM-address CONTENT inside that this toolkit's
 * base-lowering trick needs to re-base (their payload is either opaque
 * bytes at that offset, like a code signature, or file-offset-relative
 * sub-tables, like LC_DYSYMTAB's indirect-symbol table -- nothing keyed off
 * the image's VM base the way LC_FUNCTION_STARTS' leading ULEB delta or the
 * export trie's addresses are). For exactly this group, "src/grow.c's
 * mg_classify_cb accepts it" and "ml_bump_lc bumps its offset(s)" are the
 * same fact stated twice, so both switches build their case labels for
 * this group from ONE list here instead of two independently maintained
 * ones -- a load command cannot be added to grow.c's accept side of this
 * bucket without being added here, and adding it here is what teaches
 * ml_bump_lc to bump it (a hand-written case+body per member still, since
 * the fields differ, but the case LABEL for both switches comes from this
 * macro, not a second hand-typed copy).
 *
 * A whole-branch review's mutation testing found the original gap this
 * closes: mg_classify_cb and ml_bump_lc were two switch statements
 * deciding one question with nothing coupling them, so moving a load
 * command between them could silently disagree (see tests/grow_test.c's
 * test_grow_refuses_note for the exact repro this project hit). This macro
 * makes that specific disagreement impossible for this group, not merely
 * tested.
 *
 * Deliberately NOT every load command either function touches:
 *   - LC_SEGMENT_64, LC_FUNCTION_STARTS, LC_DATA_IN_CODE,
 *     LC_DYLD_INFO[_ONLY], LC_DYLD_EXPORTS_TRIE are accepted by grow.c
 *     ONLY because it ALSO re-bases VM-address CONTENT inside them
 *     (mg_reencode_funcstarts_base, mg_trie_walk) -- ml_bump_lc's bump of
 *     their file-offset field is a second, separate requirement on top of
 *     that content re-base, not interchangeable with it, so they stay out
 *     of this list even though ml_bump_lc does bump them (see ml_bump_lc's
 *     own switch).
 *   - LC_SEGMENT_SPLIT_INFO, LC_LINKER_OPTIMIZATION_HINT, and
 *     LC_DYLD_CHAINED_FIXUPS share ml_bump_lc's identical dataoff case
 *     with this list's members, but grow.c REFUSES all three anyway: their
 *     PAYLOAD also needs content-level re-basing this toolkit does not
 *     implement. Folding them into this list would silently WIDEN grow.c's
 *     acceptance to cover them -- exactly the mistake "refuse rather than
 *     guess" exists to prevent -- so this macro must never grow to include
 *     them without also implementing that content re-base.
 *   - This mechanism covers ONLY the case where "ml_bump_lc supports it"
 *     and "safe for grow.c to accept" are truly the same question. It
 *     cannot and does not cover a load command whose file offset
 *     ml_bump_lc has never been taught to bump at all -- the LC_NOTE
 *     shape, a genuinely different struct layout needing genuinely new
 *     code on both sides, not just a new macro entry. That class stays
 *     covered by testing the refusal directly:
 *     tests/grow_test.c's test_grow_refuses_note/test_grow_refuses_atom_info
 *     are the template; extend it with a new MG_T_* fixture whenever a
 *     load command outside both this macro and that pair of tests is
 *     added to either switch. */
#define ML_PLAIN_OFFSET_LCS(X) \
    X(LC_SYMTAB) X(LC_DYSYMTAB) X(LC_CODE_SIGNATURE) \
    X(LC_DYLIB_CODE_SIGN_DRS) X(LC_TWOLEVEL_HINTS) \
    X(LC_ENCRYPTION_INFO) X(LC_ENCRYPTION_INFO_64)

/* Shift one file-offset field down by `grow` if it points at/after
 * `insert`; leaves it untouched otherwise (a field of 0, meaning "this
 * stream is absent", is always < insert and so is never bumped, UNLESS
 * insert itself is 0 -- see tests/linkedit_test.c's own boundary case).
 * Returns 0 on success. Returns -1, with nothing written to *off, if
 * shifting would overflow a uint32_t -- refuses rather than silently
 * wrapping a file offset into a small, wrong, still-plausible-looking
 * number. */
int ml_bump(uint32_t *off, uint32_t insert, uint32_t grow);

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
 *   LC_TWOLEVEL_HINTS       offset
 *   LC_ENCRYPTION_INFO,
 *   LC_ENCRYPTION_INFO_64   cryptoff
 * LC_NOTE and LC_ATOM_INFO are NOT in this list -- see this header's own
 * top comment; growing a file carrying either is refused before this
 * function is ever called. Every other load command carries no file
 * offset this module knows about and is left alone -- same "refuse rather
 * than guess" posture as the rest of this toolkit, just expressed as "do
 * nothing" rather than an error, because an unrecognized load command with
 * no offset field is not a hazard the way an unrecognized SECTION TYPE is
 * elsewhere in this codebase.
 *
 * `im` must already be wrapped/opened over the buffer at its FINAL size --
 * the size after the insert -- so mi_each_lc's own bounds checking
 * (established at mi_wrap/mi_open time) still matches the buffer being
 * edited.
 *
 * Returns 0 on success. Returns -1 if any field's shift would overflow a
 * uint32_t (ml_bump's own refusal, propagated here) -- the walk stops at
 * that command; fields on commands walked before it are already bumped in
 * place and NOT rolled back. Same contract every other internal failure
 * path in mg_grow_header already relies on: the caller must treat -1 as
 * "discard this buffer, do not write it out", never partial success. */
int ml_bump_all(mi_image *im, uint32_t insert, uint32_t grow);

#endif /* MACHO9_LINKEDIT_H */
