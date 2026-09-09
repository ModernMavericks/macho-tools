/* trie.h — rebuild a dyld export trie whose addresses need to widen.
 *
 * macho_grow.h's image-base trick adds a fixed `shift` to every exported
 * address (a ULEB offset from the image base). Usually that re-encodes in
 * place, at the address's ORIGINAL byte width (see mg_trie_node) — but
 * sometimes adding `shift` pushes an address's minimal ULEB encoding one byte
 * wider, which an in-place patch cannot absorb without cascading into every
 * offset after it. This module handles that case: decode the whole trie,
 * shift every address, and re-serialize a fresh one with everything minimally
 * encoded, so no entry's original width matters anymore.
 *
 * Adapted from Wowfunhappy's export-trie rebuilder in
 * github.com/Wowfunhappy/insert_dylib commit 6d3aa61 ("Handle binaries
 * without enough space. (Vibecoded)"), which he has stated is public
 * domain/CC0/WTFPL (github.com/Wowfunhappy/Mavericks-Porting-Resources issue
 * #4). Rewritten, not ported: no global mutable state (trie_nodes_pool /
 * trie_node_count / trie_node_cap there), no fixed TRIE_MAX_EDGES(128) /
 * payload[64] / ch[].lbl[256] caps that silently drop edges or truncate
 * labels/payloads, and reuses this repo's own src/uleb.h instead of a second
 * ULEB implementation. See docs/prior-art.md for the fuller licensing note.
 */
#ifndef MACHO9_TRIE_H
#define MACHO9_TRIE_H

#include <stdint.h>

/* Rebuild the export trie at trie[0..size). Every exported address (nonzero)
 * gains `shift`; address 0 is left as 0 — that is __mh_execute_header, which
 * names the header itself, and the header moved down with the base too, so 0
 * remains correct (same rule mg_trie_node applies for the in-place path).
 *
 * On success returns 0: *out is a freshly malloc'd buffer of *out_size bytes,
 * fully independent of `trie` (the caller may free/realloc/move `trie`
 * immediately afterward). Caller owns *out and must free() it.
 *
 * On failure returns -1, *out is NULL, *out_size is 0, and a reason has been
 * printed to stderr — refuses rather than guesses, same rule as the rest of
 * macho_grow: a malformed trie (an offset outside the buffer, a truncated
 * ULEB, a terminal size or label running past the buffer's end), a node
 * offset reachable more than one way (this rebuild does not support shared
 * subtrees — no well-formed export trie needs to, since ld64 emits a plain
 * tree), or recursion past MT_TRIE_MAX_DEPTH (a pathological/adversarial
 * trie deep enough to risk exhausting the C stack). There is no separate cap
 * on node count or edge count: both are bounded by the input's own `size`
 * (each node is keyed by its unique byte offset in [0,size), so no more of
 * them can exist than there are bytes), so nothing here can silently drop an
 * edge or truncate a label/payload the way the reference implementation's
 * fixed-size buffers could. */
int mt_trie_rebuild(const uint8_t *trie, uint32_t size, uint64_t shift,
                     uint8_t **out, uint32_t *out_size);

#endif /* MACHO9_TRIE_H */
