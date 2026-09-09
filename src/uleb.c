/* ULEB128 decode / minlen / fixed-width encode.
 *
 * Moved verbatim from macho_grow.h (mg_uleb_decode, mg_uleb_minlen,
 * mg_uleb_encode_fixed); only the prefix changed. The bodies are deliberately
 * untouched -- macho_grow_test already tests all three directly, so the safety
 * net predates the move, and any behaviour change here would be a regression
 * hidden inside a refactor. */

#include "uleb.h"

int mu_decode(const uint8_t *p, const uint8_t *end, uint64_t *out) {
    uint64_t r = 0; int s = 0, n = 0;
    while (p + n < end && n < 10) {
        uint8_t b = p[n]; r |= (uint64_t)(b & 0x7f) << s; n++;
        if (!(b & 0x80)) { *out = r; return n; }
        s += 7;
    }
    return 0;
}

int mu_minlen(uint64_t v) {
    int n = 1; while (v >= 0x80) { v >>= 7; n++; } return n;
}

int mu_encode_fixed(uint8_t *p, uint64_t v, int width) {
    if (width < 1 || mu_minlen(v) > width) return 0;
    for (int i = 0; i < width; i++) {
        uint8_t b = (uint8_t)((v >> (7 * i)) & 0x7f);
        if (i < width - 1) b |= 0x80;   /* keep the stream going through the pad */
        p[i] = b;
    }
    return 1;
}
