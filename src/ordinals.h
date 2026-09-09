/* ordinals.h -- library-ordinal renumbering, moved out of change_dylib.c.
 *
 * Verbatim move (Task 3, step 2 of docs/superpowers/plans/2026-09-08-macho9-toolkit.md):
 * body unchanged, only relocated to its own translation unit so it can be
 * shared. The mo_* interface described in the plan lands in a follow-up
 * commit; this one just gets renumber_ordinals out of change_dylib.c intact.
 */

#ifndef MACHO9_ORDINALS_H
#define MACHO9_ORDINALS_H

#include <stdint.h>

/*
 * Apply `map` (old 1-based ordinal -> new ordinal, or 0 for "deleted") to every
 * place an image records one. Must run on the committed buffer, so the load
 * commands already carry their final LINKEDIT offsets.
 */
int renumber_ordinals(uint8_t *buf, const int *map, int nold, int verbose);

#endif /* MACHO9_ORDINALS_H */
