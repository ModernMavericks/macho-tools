/* ordinals.c -- library-ordinal renumbering, moved out of change_dylib.c.
 *
 * Verbatim move (Task 3, step 2): renumber_ordinals, renumber_bind_stream and
 * uleb_skip are unchanged from change_dylib.c except for becoming file-scope
 * here instead of static in change_dylib.c (renumber_ordinals needs external
 * linkage to be callable from there now). Reshaping into the mo_* interface
 * described in the toolkit plan happens in a follow-up commit.
 */
#include <stdio.h>
#include <stdint.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include "ordinals.h"
#include "uleb.h"

/* The 10.9 SDK headers predate chained fixups; change_dylib.c got this
 * definition for free via macho_grow.h's guarded #define. This file doesn't
 * include macho_grow.h (it's tool-specific and header-only), so it needs its
 * own copy of the same guarded fallback. */
#ifndef LC_DYLD_CHAINED_FIXUPS
#define LC_DYLD_CHAINED_FIXUPS 0x80000034
#endif

static const uint8_t *uleb_skip(const uint8_t *p, const uint8_t *end) {
    while (p < end && (*p & 0x80)) p++;
    return p < end ? p + 1 : end;
}

/*
 * Renumber the library ordinals in one bind opcode stream. Every opcode has to
 * be decoded, not just scanned for, because operands (ULEBs, symbol names) would
 * otherwise be mistaken for opcodes. Returns 0 on success, -1 on a stream we
 * can't safely rewrite (unknown opcode, or a new ordinal that no longer fits the
 * encoding the linker chose — both refuse rather than corrupt).
 */
static int renumber_bind_stream(uint8_t *base, uint32_t size, const int *map,
                                int nold, const char *what) {
    uint8_t *p = base, *end = base + size;
    while (p < end) {
        uint8_t op = *p & BIND_OPCODE_MASK, imm = *p & BIND_IMMEDIATE_MASK;
        switch (op) {
        case BIND_OPCODE_DONE:
        case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:  /* self/exe/flat — no ordinal */
        case BIND_OPCODE_SET_TYPE_IMM:
        case BIND_OPCODE_DO_BIND:
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
            p++;
            break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM: {
            int old = imm, neu;
            if (old < 1 || old > nold) {
                fprintf(stderr, "ERROR: %s: ordinal %d out of range\n", what, old);
                return -1;
            }
            neu = map[old];
            if (neu == 0) {
                fprintf(stderr, "ERROR: %s binds a symbol to the dylib being "
                                "deleted; refusing\n", what);
                return -1;
            }
            if (neu > BIND_IMMEDIATE_MASK) {
                fprintf(stderr, "ERROR: %s: ordinal %d no longer fits the 4-bit "
                                "immediate form (would need a stream rebuild)\n", what, neu);
                return -1;
            }
            *p = (uint8_t)(BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | (neu & BIND_IMMEDIATE_MASK));
            p++;
            break;
        }
        case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: {
            uint64_t v = 0;
            int len = mu_decode(p + 1, end, &v);
            int neu;
            if (len <= 0) { fprintf(stderr, "ERROR: %s: bad ULEB\n", what); return -1; }
            if (v < 1 || v > (uint64_t)nold) {
                fprintf(stderr, "ERROR: %s: ordinal %llu out of range\n",
                        what, (unsigned long long)v);
                return -1;
            }
            neu = map[v];
            if (neu == 0) {
                fprintf(stderr, "ERROR: %s binds a symbol to the dylib being "
                                "deleted; refusing\n", what);
                return -1;
            }
            /* Rewrite in place only if the new value encodes to the same width;
             * growing the stream would shift all of LINKEDIT. */
            if (mu_minlen((uint64_t)neu) != len) {
                fprintf(stderr, "ERROR: %s: ULEB ordinal %d changes width "
                                "(would need a stream rebuild)\n", what, neu);
                return -1;
            }
            for (int i = 0; i < len; i++) {
                uint8_t byte = (uint8_t)(((uint64_t)neu >> (7 * i)) & 0x7f);
                if (i + 1 < len) byte |= 0x80;
                p[1 + i] = byte;
            }
            p += 1 + len;
            break;
        }
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
            p++;
            while (p < end && *p) p++;      /* NUL-terminated symbol name */
            if (p < end) p++;
            break;
        case BIND_OPCODE_SET_ADDEND_SLEB:
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
        case BIND_OPCODE_ADD_ADDR_ULEB:
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
            p = (uint8_t *)uleb_skip(p + 1, end);
            break;
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
            p = (uint8_t *)uleb_skip(p + 1, end);
            p = (uint8_t *)uleb_skip(p, end);
            break;
        default:
            fprintf(stderr, "ERROR: %s: unknown bind opcode 0x%02x\n", what, op);
            return -1;
        }
    }
    return 0;
}

/*
 * Apply `map` (old 1-based ordinal -> new ordinal, or 0 for "deleted") to every
 * place an image records one. Must run on the committed buffer, so the load
 * commands already carry their final LINKEDIT offsets.
 */
int renumber_ordinals(uint8_t *buf, const int *map, int nold, int verbose) {
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    uint8_t *lcp = buf + sizeof(struct mach_header_64);
    struct symtab_command *st = NULL;
    struct dyld_info_command *di = NULL;
    int chained = 0;

    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_SYMTAB) st = (struct symtab_command *)lcp;
        else if (lc->cmd == LC_DYLD_INFO || lc->cmd == LC_DYLD_INFO_ONLY)
            di = (struct dyld_info_command *)lcp;
        else if (lc->cmd == LC_DYLD_CHAINED_FIXUPS) chained = 1;
        lcp += lc->cmdsize;
    }

    if (chained) {
        fprintf(stderr, "ERROR: image uses LC_DYLD_CHAINED_FIXUPS, whose import "
                        "table also carries library ordinals; renumbering it is "
                        "not implemented. Refusing rather than corrupting.\n");
        return -1;
    }
    if (!(hdr->flags & MH_TWOLEVEL)) {
        if (verbose) printf("  Flat namespace: no library ordinals to renumber.\n");
        return 0;
    }

    long changed = 0;
    if (st) {
        struct nlist_64 *syms = (struct nlist_64 *)(buf + st->symoff);
        for (uint32_t i = 0; i < st->nsyms; i++) {
            struct nlist_64 *n = &syms[i];
            if (n->n_type & N_STAB) continue;
            uint8_t type = n->n_type & N_TYPE;
            if (type != N_UNDF && type != N_PBUD) continue;
            int old = GET_LIBRARY_ORDINAL(n->n_desc);
            if (old < 1 || old > MAX_LIBRARY_ORDINAL) continue;  /* SELF/DYNAMIC/EXECUTABLE */
            if (old > nold) {
                fprintf(stderr, "ERROR: symtab ordinal %d exceeds %d dylibs\n", old, nold);
                return -1;
            }
            if (map[old] == 0) {
                const char *nm = (const char *)(buf + st->stroff + n->n_un.n_strx);
                fprintf(stderr, "ERROR: symbol %s still binds to the dylib being "
                                "deleted; refusing\n", nm);
                return -1;
            }
            if (map[old] != old) {
                uint16_t d = n->n_desc;
                SET_LIBRARY_ORDINAL(d, (uint8_t)map[old]);
                n->n_desc = d;
                changed++;
            }
        }
    }

    if (di) {
        if (di->bind_size &&
            renumber_bind_stream(buf + di->bind_off, di->bind_size, map, nold, "bind") != 0)
            return -1;
        if (di->weak_bind_size &&
            renumber_bind_stream(buf + di->weak_bind_off, di->weak_bind_size, map, nold, "weak bind") != 0)
            return -1;
        if (di->lazy_bind_size &&
            renumber_bind_stream(buf + di->lazy_bind_off, di->lazy_bind_size, map, nold, "lazy bind") != 0)
            return -1;
    }

    if (verbose)
        printf("  Renumbered library ordinals: %ld symbol entries + bind streams\n", changed);
    return 0;
}
