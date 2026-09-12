/* ULEB128: the variable-length integer encoding Mach-O uses throughout
 * __LINKEDIT -- the dyld info bind/rebase/export streams, and the
 * LC_FUNCTION_STARTS delta blob.
 *
 * These three are the smallest thing every rewriter in this repo needs, which is
 * why they are the first extraction. The fixed-width encoder is the interesting
 * one: it exists so a value can be re-encoded IN PLACE, keeping its original
 * byte width, so the blob -- and all of __LINKEDIT after it -- never moves. That
 * is the constraint the whole toolkit is built around.
 *
 * Note the deliberate absence of a variable-width encoder here. patch_macho.c
 * has one (ob_uleb), but it appends to a growable buffer while rebuilding a
 * stream from scratch, which is a different job from editing one in place. */

#ifndef MACHOTOOL_ULEB_H
#define MACHOTOOL_ULEB_H

#include <stdint.h>

/* Decode one ULEB128 at p (< end). Returns bytes consumed, 0 if malformed
 * (continuation runs past end, or > 10 bytes). *out = value. */
int mu_decode(const uint8_t *p, const uint8_t *end, uint64_t *out);

/* Minimal number of bytes to ULEB-encode v (>= 1). */
int mu_minlen(uint64_t v);

/* Encode v into exactly `width` ULEB128 bytes at p, padding non-minimally with
 * continuation groups if width exceeds the minimal length. Returns 1 on success,
 * 0 if v does not fit in `width` bytes. */
int mu_encode_fixed(uint8_t *p, uint64_t v, int width);

#endif /* MACHOTOOL_ULEB_H */
