/* ordinals.h -- the library-ordinal map, shared by the rewriter and the
 * renumberer so they cannot independently disagree about where a dylib
 * landed.
 *
 * In a two-level-namespace image, every undefined symbol records WHICH dylib
 * it comes from as a 1-based index into the LC_LOAD_DYLIB (and weak/reexport/
 * upward) load commands, in load-command order. The index lives in two
 * places: the nlist n_desc of each undefined symbol, and the
 * SET_DYLIB_ORDINAL opcodes of the LC_DYLD_INFO bind/weak/lazy streams.
 * Inserting or deleting one of those load commands shifts every later index,
 * so both places need renumbering -- and whatever decided which load
 * commands survive has to be the SAME decision the renumberer renumbers
 * against.
 *
 * -delete once produced binaries dyld refused to load ("library ordinal (4)
 * too big") because the load-command rewrite and the ordinal renumbering
 * disagreed about which dylib had been removed. Building one mo_map and
 * having every ordinal-writing step apply exactly that map is meant to make
 * that class of bug unrepresentable, not merely to fix the one instance of
 * it: there is no second place left to independently get it wrong. (There
 * remains exactly one thing a caller must still get right: the `is_deleted`
 * predicate passed to mo_map_build must be the SAME test the caller uses to
 * decide which load commands to drop, so the two stay in lockstep.)
 */

#ifndef MACHO9_ORDINALS_H
#define MACHO9_ORDINALS_H

#include <stdint.h>

/* MAX_LIBRARY_ORDINAL: the encoding's own ceiling (mach-o/loader.h). No image
 * can have more ordinal-bearing dylibs than this, so it doubles as the
 * largest map this module will ever build. */
#define MO_MAX_DYLIBS 253   /* MAX_LIBRARY_ORDINAL */

/* old_to_new[old] is the new 1-based ordinal for what used to be library
 * ordinal `old`, or 0 if that dylib was deleted. Index 0 is unused (ordinals
 * are 1-based); valid indices are 1..n. `old_to_new` is caller-owned, sized
 * at least MO_MAX_DYLIBS+1, so a caller can put it on the stack and know
 * exactly how long it needs to stay alive -- this module never allocates. */
typedef struct mo_map {
    int *old_to_new;
    int n;
} mo_map;

/* Is `cmd` a load command that consumes a library ordinal? LC_ID_DYLIB is
 * deliberately excluded: it names the image itself and is not addressable by
 * ordinal. Neither is LC_RPATH, which is a search path, not a dependency.
 * Shared here so a caller's load-command walk (deciding what to keep) and
 * mo_map_build's walk (deciding what ordinal it gets) can't drift apart by
 * using two different definitions of "ordinal-bearing". */
int mo_is_ordinal_lc(uint32_t cmd);

/* Walk the `ncmds` load commands starting right after the mach_header_64 at
 * `buf`, in order, and assign each ordinal-bearing dylib (mo_is_ordinal_lc)
 * its new ordinal: `is_deleted(name, ctx)` true maps it to 0, otherwise it
 * gets the next ordinal after `base` (so -insert, which claims 1..ninserts
 * ahead of the existing dylibs, passes ninserts as `base`). Fills
 * map->old_to_new[1..map->n] and map->n itself; map->old_to_new must already
 * point at a caller-owned array of at least MO_MAX_DYLIBS+1 ints. *out_nnew
 * receives the highest new ordinal handed out (base + count of survivors),
 * which is also the ordinal ceiling for mo_map_validate. Returns 0, or -1
 * (with a message on stderr) if there are more than MO_MAX_DYLIBS
 * ordinal-bearing dylibs. */
int mo_map_build(const uint8_t *buf, uint32_t ncmds, int base,
                  int (*is_deleted)(const char *name, void *ctx), void *ctx,
                  mo_map *map, int *out_nnew);

/* Check that `map` is internally consistent before trusting it: n within
 * [0, MO_MAX_DYLIBS], and every old_to_new[1..n] is either 0 (deleted) or a
 * positive ordinal no greater than `max_new` (the ordinal ceiling
 * mo_map_build reported). Returns 0 if so, -1 (with a message on stderr)
 * otherwise. Exists so a caller can verify a map before applying it, as a
 * belt-and-suspenders check independent of mo_map_apply's own per-opcode
 * range checks. */
int mo_map_validate(const mo_map *map, int max_new);

/* Apply `map` to every place `buf` records a library ordinal: the symtab's
 * undefined/prebound symbols, and the LC_DYLD_INFO bind/weak-bind/lazy-bind
 * streams. Must run against the already-committed load-command table, since
 * it reads the LC_SYMTAB and LC_DYLD_INFO offsets straight out of it.
 * Refuses (returns -1, with a message on stderr) rather than emit a binary
 * with a dangling or out-of-range ordinal: an unknown bind opcode, a new
 * ordinal that no longer fits the encoding the linker chose, or a symbol
 * still bound to a deleted dylib. `verbose` prints a one-line summary on
 * success. */
int mo_map_apply(uint8_t *buf, const mo_map *map, int verbose);

#endif /* MACHO9_ORDINALS_H */
