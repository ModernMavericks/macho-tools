/*
 * macho_grow.h — make room in a Mach-O header so the load commands can expand.
 *
 * The problem: tools like change_dylib (and patch_macho's LC_DYLD_INFO_ONLY
 * insertion) write load commands in place, bounded by the file offset of the
 * first section's data. When the linker leaves little padding there (recent
 * Bun/JSC builds leave as few as 16 bytes), a longer dylib path or an extra
 * load command no longer fits.
 *
 * The fix, studied from LIEF (src/MachO/Binary.cpp `shift`) and llvm-objcopy
 * (MachOLayoutBuilder): make room by inserting page-aligned space after the
 * load commands. LIEF/llvm push every later segment to a HIGHER vm address and
 * then fix up everything that depended on those addresses — section-symbol
 * n_values, LC_MAIN, function-start deltas, relocations, rebase/bind/chained
 * targets. That is a lot of machinery — and we can't just run those tools on
 * 10.9 anyway: LIEF needs a modern-macOS C++ runtime and llvm-objcopy a
 * cross-built toolchain, while install_name_tool / optool / insert_dylib refuse
 * to grow the header at all. This header compiles with the stock 10.9 clang and
 * has no dependencies.
 *
 * We take a simpler, equivalent route available to any PIE executable with a
 * __PAGEZERO: instead of raising data, we LOWER the image base. We donate the
 * inserted bytes from __PAGEZERO and drop __TEXT's vmaddr by the same amount,
 * growing __TEXT's vm/file size. Net effect: every section and segment keeps
 * its ORIGINAL vm address, so no pointer, rebase, bind, n_value, or entry
 * address ever changes. The only fields that move are file offsets — which we
 * shift uniformly. (Borrowed from LIEF: the exhaustive list of offset fields.)
 *
 * Precondition: a MH_PIE executable with a __PAGEZERO at least `grow` bytes
 * large. (Always true for the Claude Code executable: 0x1_0000_0000 pagezero.)
 * Dylibs without a __PAGEZERO can't lower the base; mg_grow_header reports that
 * and leaves the buffer untouched so the caller can fall back / error cleanly.
 */
#ifndef MACHO_GROW_H
#define MACHO_GROW_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mach-o/loader.h>

/* ULEB128 decode / minlen / fixed-width encode. */
#include "uleb.h"

/* Validated open/wrap/iterate over a Mach-O buffer. */
#include "image.h"

/* Export-trie rebuild, for when an in-place re-encode (mg_trie_node, below)
 * can't absorb an address's widened ULEB. */
#include "trie.h"

/* Load-command constants newer than the 10.9 SDK headers. */
#ifndef LC_DYLD_EXPORTS_TRIE
#define LC_DYLD_EXPORTS_TRIE        0x80000033
#endif
#ifndef LC_DYLD_CHAINED_FIXUPS
#define LC_DYLD_CHAINED_FIXUPS      0x80000034
#endif
#ifndef LC_DYLIB_CODE_SIGN_DRS
#define LC_DYLIB_CODE_SIGN_DRS      0x2B
#endif
#ifndef LC_LINKER_OPTIMIZATION_HINT
#define LC_LINKER_OPTIMIZATION_HINT 0x2E
#endif
/* dyld4-era section type: 4-byte initializer offsets FROM THE IMAGE BASE,
 * replacing the absolute pointers of S_MOD_INIT_FUNC_POINTERS. */
#ifndef S_INIT_FUNC_OFFSETS
#define S_INIT_FUNC_OFFSETS 0x16
#endif
#define MG_EXPORT_KIND_MASK        0x03
#define MG_EXPORT_REEXPORT         0x08
#define MG_EXPORT_STUB_AND_RESOLVER 0x10

#define MG_PAGE 0x1000UL

/* Lowest section file offset — this bounds the header pad. `fsize` is the
 * buffer's real size, wrapped through mi_wrap so this walk cannot stride past
 * it -- the bug class this whole extraction exists to prevent.
 *
 * Returns UINT32_MAX, with a message on stderr, if the buffer fails to wrap
 * (bad magic, or load commands that don't fit): refuse rather than guess. A
 * fixed fallback here would be a real hazard, not a theoretical one --
 * change_dylib.c's memset(buf + 32, 0, first_sect_off - 32) turns a wrong
 * guess directly into an out-of-bounds write. Every caller must check for
 * UINT32_MAX. This differs from the UINT32_MAX -> 4096 default a few lines
 * down, which is a validated image that simply has no sections -- a real,
 * if unusual, answer rather than a guess about an image we couldn't read. */
static uint32_t mg_first_sect_off(const uint8_t *buf, size_t fsize) {
    mi_image im;
    if (mi_wrap((uint8_t *)buf, fsize, &im) != 0) {
        fprintf(stderr, "macho_grow: image fails validation (bad magic, or load commands "
                        "that don't fit); refusing to guess the header pad boundary\n");
        return UINT32_MAX;
    }

    uint32_t first = UINT32_MAX;
    const uint8_t *lcp = im.buf + sizeof(*im.hdr);
    for (uint32_t i = 0; i < im.hdr->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)lcp;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)lcp;
            const struct section_64 *sect = (const struct section_64 *)(lcp + sizeof(*seg));
            for (uint32_t j = 0; j < seg->nsects; j++)
                if (sect[j].offset && sect[j].offset < first) first = sect[j].offset;
        }
        lcp += lc->cmdsize;
    }
    return first == UINT32_MAX ? 4096 : first;
}

/* Shift one file-offset field down by `grow` if it points at/after `insert`. */
static void mg_bump(uint32_t *off, uint32_t insert, uint32_t grow) {
    if (*off >= insert) *off += grow;
}

/* ---- ULEB128, for the LC_FUNCTION_STARTS leading-delta re-encode ----------
 *
 * LC_FUNCTION_STARTS is a stream of ULEB128 deltas: the FIRST is relative to the
 * image base, the rest are function-to-function. Lowering the image base by N
 * (the grow trick) leaves every function's VM address fixed, so the later deltas
 * are unchanged, but the first must gain N or every reconstructed function
 * address comes out N low. We adjust it in place, preserving its byte width so
 * the blob — and all of __LINKEDIT after it — never moves. (When the widened
 * delta would need more bytes than the original encoding, we refuse rather than
 * resize LINKEDIT; see mg_grow_header.) */

/* Re-encode the leading (base-relative) LC_FUNCTION_STARTS delta after lowering
 * the image base by `grow`: delta[0] += grow, keeping the leading delta's byte
 * width so blob size is unchanged and the trailing deltas are untouched.
 * Returns: 1 patched in place; 0 the widened delta needs more bytes than the
 * original leading encoding (caller must refuse — LINKEDIT resize unsupported);
 * -1 malformed blob (empty / bad leading ULEB). */
static int mg_reencode_funcstarts_base(uint8_t *blob, uint32_t size, uint32_t grow) {
    if (size == 0) return -1;
    uint64_t d0; int n0 = mu_decode(blob, blob + size, &d0);
    if (n0 == 0) return -1;
    uint64_t nd = d0 + grow;
    if (mu_minlen(nd) > n0) return 0;          /* would widen -> caller refuses */
    return mu_encode_fixed(blob, nd, n0) ? 1 : 0;
}

/* Decode the whole function-starts blob into absolute addresses given the image
 * base. Stops at a 0 delta (terminator/padding) or end. Returns count (<= max),
 * or -1 on a malformed ULEB. (Used by tests to assert the grow moved nothing.) */
static int mg_funcstarts_decode(const uint8_t *blob, uint32_t size,
                                uint64_t base, uint64_t *out, int max) {
    const uint8_t *p = blob, *end = blob + size; uint64_t addr = base; int n = 0;
    while (p < end && n < max) {
        uint64_t d; int c = mu_decode(p, end, &d);
        if (c == 0) return -1;
        p += c;
        if (d == 0) break;
        addr += d; out[n++] = addr;
    }
    return n;
}


/* ---- base-relative structures ---------------------------------------------
 *
 * Lowering the image base keeps every ABSOLUTE vm address fixed, which is what
 * makes this trick cheap. Values stored as an OFFSET FROM THE IMAGE BASE are the
 * exception: the base moved out from under them, so each must gain `grow`.
 * LC_FUNCTION_STARTS' leading delta (handled above) is one. These are the rest.
 *
 * Note S_MOD_INIT_FUNC_POINTERS needs nothing: those are absolute pointers that
 * dyld rebases, and their target addresses do not change. */

/* Add `grow` to every entry of every S_INIT_FUNC_OFFSETS section, or with
 * patch=0 just verify the pass would be sound. Without this, dyld4-era static
 * constructors are called at (base - grow) + offset and jump into whatever
 * precedes them. Returns 0 ok, -1 malformed/unsafe. */
static int mg_init_offsets_pass(uint8_t *buf, size_t fsize, uint32_t grow, int patch) {
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    uint8_t *lcp = buf + sizeof(*hdr);
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *seg = (struct segment_command_64 *)lcp;
            struct section_64 *sect = (struct section_64 *)(lcp + sizeof(*seg));
            for (uint32_t j = 0; j < seg->nsects; j++) {
                if ((sect[j].flags & SECTION_TYPE) != S_INIT_FUNC_OFFSETS) continue;
                if (sect[j].size % 4) return -1;
                if ((uint64_t)sect[j].offset + sect[j].size > (uint64_t)fsize) return -1;
                uint32_t n = (uint32_t)(sect[j].size / 4);
                uint32_t *e = (uint32_t *)(buf + sect[j].offset);
                for (uint32_t k = 0; k < n; k++) {
                    if (!patch) { if (e[k] > UINT32_MAX - grow) return -1; }
                    else e[k] += grow;
                }
            }
        }
        lcp += lc->cmdsize;
    }
    return 0;
}

/* Walk the export trie looking for an exported address we would have to
 * re-encode. Addresses there are ULEB offsets from the image base; bumping one
 * can widen its encoding and force __LINKEDIT to be rebuilt, which this header
 * does not do. __mh_execute_header is exported at offset 0 and stays correct --
 * it names the header, which moved down with the base -- so a trie whose
 * addresses are all zero is safe to leave alone.
 * Returns 0 safe, 1 needs re-encoding, -1 malformed. */
static int mg_trie_scan(const uint8_t *trie, uint32_t size, uint32_t off, int depth) {
    if (depth > 128 || off >= size) return -1;
    const uint8_t *p = trie + off, *end = trie + size;
    uint64_t term; int n = mu_decode(p, end, &term);
    if (n == 0) return -1;
    p += n;
    if (term) {
        const uint8_t *tend = p + term;
        if (tend > end) return -1;
        uint64_t flags; n = mu_decode(p, end, &flags);
        if (n == 0) return -1;
        p += n;
        if (!(flags & MG_EXPORT_REEXPORT)) {       /* re-exports carry no address */
            uint64_t a; n = mu_decode(p, end, &a);
            if (n == 0) return -1;
            if (a != 0) return 1;
            if (flags & MG_EXPORT_STUB_AND_RESOLVER) {
                p += n;
                n = mu_decode(p, end, &a);
                if (n == 0) return -1;
                if (a != 0) return 1;
            }
        }
        p = tend;
    }
    if (p >= end) return -1;
    uint8_t nch = *p++;
    for (uint8_t i = 0; i < nch; i++) {
        while (p < end && *p) p++;
        if (p >= end) return -1;
        p++;
        uint64_t coff; n = mu_decode(p, end, &coff);
        if (n == 0) return -1;
        p += n;
        int r = mg_trie_scan(trie, size, (uint32_t)coff, depth + 1);
        if (r != 0) return r;
    }
    return 0;
}

/* Forward: mg_collect (below) needs the compact-unwind walker, which is defined
 * after it so its long explanation sits next to the grow it serves. */
static int mg_unwind_walk(uint8_t *buf, size_t fsize, uint32_t grow, int patch,
                          uint64_t base, uint64_t *out, uint8_t *kinds,
                          uint32_t *n, uint32_t max);
static int mg_dice_walk(uint8_t *buf, size_t fsize, uint32_t grow, int patch,
                        uint64_t base, uint64_t *out, uint8_t *kinds,
                        uint32_t *n, uint32_t max);
static int mg_trie_walk(uint8_t *buf, size_t fsize, uint32_t grow, int patch,
                        uint64_t base, uint64_t *out, uint8_t *kinds,
                        uint32_t *n, uint32_t max);

/* ---- verification: prove the grow moved nothing ---------------------------
 * Every structure below stores an offset FROM THE IMAGE BASE, so lowering the
 * base by `grow` must leave the RESOLVED address (base + offset) unchanged.
 * Snapshot those resolved addresses before the transform, recompute them after,
 * and compare. That catches a handler that did not run, one that ran twice, and
 * one that ran with the wrong delta -- without needing to know which.
 *
 * mg_collect walks the base-relative structures in load-command order, which is
 * deterministic and identical before and after, so element i means the same
 * thing in both snapshots. */
/* Not every base-relative address names a function. Initializers and
 * compact-unwind entries do; a data export, a jump-table range, an LSDA blob and
 * a personality GOT slot do not. Only MG_K_FUNC entries can be checked against
 * LC_FUNCTION_STARTS. */
#define MG_K_ANY  0
#define MG_K_FUNC 1

typedef struct { uint64_t *addr; uint32_t n; } mg_snapshot;

#define MG_SNAP_MAX 65536

static int mg_collect(const uint8_t *buf, size_t fsize, uint64_t *out, uint8_t *kinds,
                      uint32_t max, uint32_t *n_out) {
    const struct mach_header_64 *h = (const struct mach_header_64 *)buf;
    const uint8_t *sp = buf + sizeof *h;
    uint64_t base = 0;
    uint32_t n = 0;
    /* image base first: __TEXT is the segment mapping the header (fileoff 0, has content) */
    const uint8_t *q = sp;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)q;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)q;
            if (seg->fileoff == 0 && seg->filesize > 0) { base = seg->vmaddr; break; }
        }
        q += lc->cmdsize;
    }
    if (!base) return -1;

    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)sp;
        if (lc->cmd == LC_FUNCTION_STARTS) {
            const struct linkedit_data_command *d = (const struct linkedit_data_command *)sp;
            if (d->datasize) {
                if ((size_t)d->dataoff + d->datasize > fsize) return -1;
                uint64_t d0;
                if (mu_decode(buf + d->dataoff, buf + d->dataoff + d->datasize, &d0) == 0)
                    return -1;
                if (n >= max) return -1;
                if (kinds) kinds[n] = MG_K_FUNC;   /* the first function's address */
                out[n++] = base + d0;
            }
        }
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)sp;
            const struct section_64 *sect = (const struct section_64 *)(sp + sizeof *seg);
            for (uint32_t j = 0; j < seg->nsects; j++) {
                if ((sect[j].flags & SECTION_TYPE) != S_INIT_FUNC_OFFSETS) continue;
                if ((size_t)sect[j].offset + sect[j].size > fsize) return -1;
                const uint32_t *e = (const uint32_t *)(buf + sect[j].offset);
                uint64_t cnt = sect[j].size / sizeof(uint32_t);
                for (uint64_t k = 0; k < cnt; k++) {
                    if (n >= max) return -1;
                    if (kinds) kinds[n] = MG_K_FUNC;   /* initializers are functions */
                    out[n++] = base + e[k];
                }
            }
        }
        sp += lc->cmdsize;
    }
    /* Compact unwind last, so element order is stable across before/after. The
     * cast is safe: with `out` non-NULL the walker only reads. */
    if (mg_trie_walk((uint8_t *)buf, fsize, 0, 0, base, out, kinds, &n, max) != 0) return -1;
    if (mg_dice_walk((uint8_t *)buf, fsize, 0, 0, base, out, kinds, &n, max) != 0) return -1;
    if (mg_unwind_walk((uint8_t *)buf, fsize, 0, 0, base, out, kinds, &n, max) != 0) return -1;
    *n_out = n;
    return 0;
}

static int mg_snapshot_take(const uint8_t *buf, size_t fsize, mg_snapshot *s) {
    s->addr = (uint64_t *)malloc(MG_SNAP_MAX * sizeof(uint64_t));
    if (!s->addr) return -1;
    if (mg_collect(buf, fsize, s->addr, NULL, MG_SNAP_MAX, &s->n) != 0) {
        free(s->addr); s->addr = NULL; s->n = 0; return -1;
    }
    return 0;
}

static void mg_snapshot_free(mg_snapshot *s) { free(s->addr); s->addr = NULL; s->n = 0; }

/* 0 if every base-relative structure resolves exactly where it did before the
 * grow; -1 (with a message naming the first mismatch) otherwise. */
static int mg_verify(const uint8_t *buf, size_t fsize, const mg_snapshot *before) {
    uint64_t *now = (uint64_t *)malloc(MG_SNAP_MAX * sizeof(uint64_t));
    if (!now) return -1;
    uint32_t n = 0;
    if (mg_collect(buf, fsize, now, NULL, MG_SNAP_MAX, &n) != 0) {
        fprintf(stderr, "macho_grow: verify could not re-read the base-relative structures\n");
        free(now); return -1;
    }
    if (n != before->n) {
        fprintf(stderr, "macho_grow: verify found %u base-relative entries, %u before -- "
                        "the grow added or dropped one\n", n, before->n);
        free(now); return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (now[i] == before->addr[i]) continue;
        int64_t moved = (int64_t)(now[i] - before->addr[i]);
        fprintf(stderr, "macho_grow: verify FAILED -- base-relative entry %u resolved to "
                        "%#llx before the grow and %#llx after (moved %+lld bytes). The grow "
                        "must leave every resolved address unchanged; refusing.\n",
                i, (unsigned long long)before->addr[i], (unsigned long long)now[i],
                (long long)moved);
        free(now); return -1;
    }
    free(now);
    return 0;
}

/* ---- __TEXT,__unwind_info -------------------------------------------------
 * Compact unwind stores several different things as 32-bit words, and only some
 * are measured from the image base. Getting that distinction wrong is silent:
 * the tables still parse, and only an actual unwind notices.
 *
 * MUST gain `grow` (offsets from the image base):
 *   - personality array entries (they address the routine's GOT slot)
 *   - first-level index functionOffset, INCLUDING the trailing sentinel
 *   - LSDA index entries: both functionOffset and lsdaOffset
 *   - regular (kind 2) second-level page entry functionOffset
 * MUST NOT be touched:
 *   - compressed (kind 3) second-level entries. Their low 24 bits are a delta
 *     from their own page's first-level functionOffset, which the bump above
 *     already moved, so they are correct untouched and corrupt if bumped.
 *   - every *SectionOffset field: those are offsets within this section.
 *   - common encodings: encodings, not addresses.
 *
 * One walker, three uses -- audit (patch=0), apply (patch=1), collect for verify
 * (out != NULL). Deliberately one function: the __init_offsets double-apply
 * happened because two functions encoded the same knowledge and both ran. */
static int mg_uw_bump(uint8_t *p, uint32_t grow, int patch) {
    uint32_t v; memcpy(&v, p, sizeof v);
    if (v > 0xffffffffu - grow) return -1;      /* would overflow the 32-bit field */
    if (patch) { v += grow; memcpy(p, &v, sizeof v); }
    return 0;
}

static int mg_unwind_walk(uint8_t *buf, size_t fsize, uint32_t grow, int patch,
                          uint64_t base, uint64_t *out, uint8_t *kinds,
                          uint32_t *n, uint32_t max) {
    const struct mach_header_64 *h = (const struct mach_header_64 *)buf;
    const uint8_t *sp = buf + sizeof *h;
    uint8_t *u = NULL; uint32_t usz = 0;
    for (uint32_t i = 0; i < h->ncmds && !u; i++) {
        const struct load_command *lc = (const struct load_command *)sp;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)sp;
            const struct section_64 *sect = (const struct section_64 *)(sp + sizeof *seg);
            for (uint32_t j = 0; j < seg->nsects; j++) {
                if (strncmp(sect[j].sectname, "__unwind_info", sizeof sect[j].sectname)) continue;
                if (!sect[j].size) return 0;
                if ((uint64_t)sect[j].offset + sect[j].size > fsize) return -1;
                u = buf + sect[j].offset; usz = (uint32_t)sect[j].size; break;
            }
        }
        sp += lc->cmdsize;
    }
    if (!u) return 0;                       /* no compact unwind: nothing to do */

#define UW_RD(off) ({ uint32_t _v; memcpy(&_v, u + (off), sizeof _v); _v; })
#define UW_VISIT(off, k) do {                                                  \
        if (out) {                                                             \
            if (*n >= max) return -1;                                          \
            if (kinds) kinds[*n] = (k);                                        \
            out[(*n)++] = base + UW_RD(off);                                   \
        } else if (mg_uw_bump(u + (off), grow, patch) != 0) return -1;          \
    } while (0)

    if (usz < 28) return -1;
    if (UW_RD(0) != 1) return -1;           /* unknown version -> refuse, do not guess */
    uint32_t peOff = UW_RD(12), peCnt = UW_RD(16);
    uint32_t idxOff = UW_RD(20), idxCnt = UW_RD(24);

    if (peCnt) {
        if ((uint64_t)peOff + 4ull * peCnt > usz) return -1;
        for (uint32_t k = 0; k < peCnt; k++) UW_VISIT(peOff + 4 * k, MG_K_ANY); /* GOT slot */
    }

    if (idxCnt < 1) return -1;
    if ((uint64_t)idxOff + 12ull * idxCnt > usz) return -1;
    /* The LAST first-level entry is the sentinel: its functionOffset marks the END
     * of the final function, not the start of one, so it is not required to appear
     * in LC_FUNCTION_STARTS. It still needs re-basing like the rest. (It happens to
     * coincide with a function start on Claude Code 2.1.263 -- 13/13 -- which is
     * exactly the sort of single-sample coincidence that makes a wrong rule look
     * right; a small dylib in change_dylib_test.sh disproved it.) */
    for (uint32_t k = 0; k < idxCnt; k++)
        UW_VISIT(idxOff + 12 * k, (k + 1 == idxCnt) ? MG_K_ANY : MG_K_FUNC);

    /* The last first-level entry is the sentinel: it has no page, and its lsda
     * offset marks the end of the previous entry's LSDA array. */
    for (uint32_t k = 0; k + 1 < idxCnt; k++) {
        uint32_t lo = UW_RD(idxOff + 12 * k + 8);
        uint32_t hi = UW_RD(idxOff + 12 * (k + 1) + 8);
        if (hi < lo || hi > usz) return -1;
        for (uint32_t e = lo; e + 8 <= hi; e += 8) {
            UW_VISIT(e, MG_K_FUNC);        /* functionOffset */
            UW_VISIT(e + 4, MG_K_ANY);     /* lsdaOffset -> __gcc_except_tab */
        }
    }

    for (uint32_t k = 0; k + 1 < idxCnt; k++) {
        uint32_t pg = UW_RD(idxOff + 12 * k + 4);
        if (!pg) continue;
        if ((uint64_t)pg + 8 > usz) return -1;
        uint32_t kind = UW_RD(pg);
        if (kind == 3) continue;            /* COMPRESSED: deltas, leave alone */
        if (kind != 2) return -1;           /* unknown page kind -> refuse */
        uint16_t epo, ec;
        memcpy(&epo, u + pg + 4, sizeof epo);
        memcpy(&ec,  u + pg + 6, sizeof ec);
        uint64_t first = (uint64_t)pg + epo;
        if (first + 8ull * ec > usz) return -1;
        for (uint32_t e = 0; e < ec; e++) UW_VISIT((uint32_t)(first + 8ull * e), MG_K_FUNC);
    }
#undef UW_VISIT
#undef UW_RD
    return 0;
}

/* ---- LC_DATA_IN_CODE ------------------------------------------------------
 * A flat array of data_in_code_entry { uint32 offset; uint16 length; uint16 kind }.
 * ONLY `offset` is measured from the image base. `length` and `kind` are not
 * offsets at all, so a walker that bumps whole words instead of the first field
 * of each entry corrupts every range while still "changing by grow".
 * One walker, three uses, as for compact unwind. */
static int mg_dice_walk(uint8_t *buf, size_t fsize, uint32_t grow, int patch,
                        uint64_t base, uint64_t *out, uint8_t *kinds,
                        uint32_t *n, uint32_t max) {
    const struct mach_header_64 *h = (const struct mach_header_64 *)buf;
    const uint8_t *sp = buf + sizeof *h;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)sp;
        if (lc->cmd == LC_DATA_IN_CODE) {
            const struct linkedit_data_command *d = (const struct linkedit_data_command *)sp;
            if (!d->datasize) return 0;
            if ((uint64_t)d->dataoff + d->datasize > fsize) return -1;
            if (d->datasize % 8) return -1;          /* not a whole number of entries */
            uint8_t *e = buf + d->dataoff;
            for (uint32_t k = 0; k < d->datasize; k += 8) {
                if (out) {
                    if (*n >= max) return -1;
                    uint32_t v; memcpy(&v, e + k, sizeof v);
                    if (kinds) kinds[*n] = MG_K_ANY;   /* jump tables sit mid-function */
                    out[(*n)++] = base + v;
                } else if (mg_uw_bump(e + k, grow, patch) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        sp += lc->cmdsize;
    }
    return 0;
}

/* ---- export trie ----------------------------------------------------------
 * Each exported address is a ULEB offset FROM THE IMAGE BASE, so lowering the
 * base means every one must gain `grow`. The reason this is safe to do in place:
 * adding a page never widens the encoding on a real binary. Measured across all
 * 670 entries of Claude Code 2.1.263 at 4K, 8K and 16K grows, zero needed a
 * wider ULEB and zero needed redundant padding. So each address is re-encoded at
 * its ORIGINAL byte width, the trie keeps its size, and no __LINKEDIT offset
 * moves. If one ever would widen, we refuse -- that is the case the old guard
 * was written for, and it is still handled, just no longer assumed.
 *
 * Address 0 stays 0. That is __mh_execute_header, which names the header itself;
 * the header moved down with the base, so 0 remains correct. It is therefore
 * neither bumped nor collected -- its resolved address is base+0, which SHOULD
 * change, and collecting it would make verify fail on a correct grow.
 *
 * `seen` guards a shared subtree from being bumped twice -- the same hazard as
 * the __init_offsets double-apply. */
static int mg_trie_node(uint8_t *trie, uint32_t size, uint32_t off, int depth,
                        uint32_t grow, int patch, uint64_t base,
                        uint64_t *out, uint8_t *kinds, uint32_t *n, uint32_t max,
                        uint8_t *seen) {
    if (depth > 128 || off >= size) return -1;
    if (seen[off]) return 0;
    seen[off] = 1;
    uint8_t *p = trie + off, *end = trie + size;
    uint64_t term; int k = mu_decode(p, end, &term);
    if (k == 0) return -1;
    p += k;
    if (term) {
        uint8_t *tend = p + term;
        if (tend > end) return -1;
        uint64_t flags; k = mu_decode(p, end, &flags);
        if (k == 0) return -1;
        p += k;
        if (!(flags & MG_EXPORT_REEXPORT)) {          /* re-exports carry no address */
            int rounds = (flags & MG_EXPORT_STUB_AND_RESOLVER) ? 2 : 1;
            for (int r = 0; r < rounds; r++) {
                uint64_t a; int w = mu_decode(p, end, &a);
                if (w == 0) return -1;
                if (a != 0) {
                    if (out) {
                        if (*n >= max) return -1;
                        if (kinds) kinds[*n] = MG_K_ANY;  /* data exports are not functions */
                        out[(*n)++] = base + a;
                    } else {
                        if (mu_minlen(a + grow) > w) return 1;   /* would widen */
                        if (patch && !mu_encode_fixed(p, a + grow, w)) return 1;
                    }
                }
                p += w;
            }
        }
        p = tend;
    }
    if (p >= end) return -1;
    uint8_t nch = *p++;
    for (uint8_t i = 0; i < nch; i++) {
        while (p < end && *p) p++;
        if (p >= end) return -1;
        p++;
        uint64_t coff; k = mu_decode(p, end, &coff);
        if (k == 0) return -1;
        p += k;
        int r = mg_trie_node(trie, size, (uint32_t)coff, depth + 1, grow, patch,
                             base, out, kinds, n, max, seen);
        if (r != 0) return r;
    }
    return 0;
}

/* Locate the export trie's load command -- LC_DYLD_INFO, LC_DYLD_INFO_ONLY, or
 * LC_DYLD_EXPORTS_TRIE, whichever this image carries -- and return its BYTE
 * OFFSET from buf (not a pointer: a caller that goes on to realloc buf, as
 * the widen-append path does, needs an offset it can re-derive a pointer
 * from afterward, not a pointer the realloc may have invalidated). Returns 1
 * with *lc_off and *cmd set, or 0 if this image has no such load command (not an
 * error -- just nothing to walk).
 *
 * The single source of truth for "which load command carries the export
 * trie": mg_find_trie (below) and mg_grow_header's widen-append path both
 * call this rather than each re-scanning load commands on their own, so the
 * two can never disagree about which one it is. (They once could: an
 * earlier version had the append path re-scan without breaking on the first
 * match, landing on the LAST export-trie-shaped load command while this
 * function -- and mg_find_trie -- always meant the FIRST. A file carrying
 * both LC_DYLD_INFO_ONLY and LC_DYLD_EXPORTS_TRIE would have repointed the
 * wrong one. It failed safe -- mg_verify would see pre-shift addresses
 * through the untouched first LC and refuse -- but "two places
 * independently deciding the same thing" is exactly the bug shape this
 * whole toolkit plan exists to eliminate, so it is not left as a coincidence
 * that happens to agree today.) */
static int mg_find_trie_lc(const uint8_t *buf, long *lc_off, uint32_t *cmd) {
    const struct mach_header_64 *h = (const struct mach_header_64 *)buf;
    const uint8_t *sp = buf + sizeof *h;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)sp;
        if (lc->cmd == LC_DYLD_INFO || lc->cmd == LC_DYLD_INFO_ONLY ||
            lc->cmd == LC_DYLD_EXPORTS_TRIE) {
            *lc_off = sp - buf; *cmd = lc->cmd; return 1;
        }
        sp += lc->cmdsize;
    }
    return 0;
}

/* Locate the export trie's (off, size), whichever load command carries it --
 * LC_DYLD_INFO[_ONLY]'s export_off/export_size, or LC_DYLD_EXPORTS_TRIE's
 * dataoff/datasize. Returns 1 with *off and *size set, or 0 if this image has
 * no export-trie load command at all (not an error -- just nothing to walk). */
static int mg_find_trie(const uint8_t *buf, uint32_t *off, uint32_t *size) {
    long lc_off; uint32_t cmd;
    if (!mg_find_trie_lc(buf, &lc_off, &cmd)) return 0;
    if (cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY) {
        const struct dyld_info_command *d = (const struct dyld_info_command *)(buf + lc_off);
        *off = d->export_off; *size = d->export_size;
    } else {
        const struct linkedit_data_command *d =
            (const struct linkedit_data_command *)(buf + lc_off);
        *off = d->dataoff; *size = d->datasize;
    }
    return 1;
}

static int mg_trie_walk(uint8_t *buf, size_t fsize, uint32_t grow, int patch,
                        uint64_t base, uint64_t *out, uint8_t *kinds,
                        uint32_t *n, uint32_t max) {
    uint32_t off, size;
    if (!mg_find_trie(buf, &off, &size)) return 0;
    if (!off || !size) return 0;
    if ((uint64_t)off + size > fsize) return -1;
    uint8_t *seen = (uint8_t *)calloc(size, 1);
    if (!seen) return -1;
    int r = mg_trie_node(buf + off, size, 0, 0, grow, patch, base, out, kinds, n, max, seen);
    free(seen);
    return r;
}

/* ---- classification: unknown means unsafe ---------------------------------
 * Lowering the image base is only safe if NOTHING in the file stores an offset
 * measured from that base which we do not re-base. The handlers above cover the
 * five structures we know about. This covers the ones we do not.
 *
 * A load command or section type nobody has classified may carry base-relative
 * data exactly as __init_offsets and compact unwind do, and there is no way to
 * tell from the number alone. Growing anyway is precisely how LC_DATA_IN_CODE
 * and __TEXT,__unwind_info came to be silently corrupted. So: everything is
 * enumerated on purpose, and anything unrecognised refuses.
 *
 * HONEST LIMIT: section classification is by TYPE, which describes how the
 * contents are encoded, plus a by-NAME list of the S_REGULAR sections known to
 * hold base-relative data (today just __unwind_info). A *new* S_REGULAR section
 * carrying base-relative offsets would pass this check. Type covers the
 * encoding families; the name list cannot cover what has not been invented.
 * That residual risk is what the verify pass exists to narrow. */
#ifndef LC_LAZY_LOAD_DYLIB
#define LC_LAZY_LOAD_DYLIB 0x20
#endif
#ifndef LC_DYLD_ENVIRONMENT
#define LC_DYLD_ENVIRONMENT 0x27
#endif
#ifndef LC_LINKER_OPTION
#define LC_LINKER_OPTION 0x2D
#endif
#ifndef LC_NOTE
#define LC_NOTE 0x31
#endif
#ifndef LC_BUILD_VERSION
#define LC_BUILD_VERSION 0x32
#endif
#ifndef LC_FILESET_ENTRY
#define LC_FILESET_ENTRY 0x80000035
#endif
#ifndef LC_ATOM_INFO
#define LC_ATOM_INFO 0x36
#endif
#ifndef S_INIT_FUNC_OFFSETS
#define S_INIT_FUNC_OFFSETS 0x16
#endif

static int mg_classify(const uint8_t *buf, size_t fsize) {
    mi_image im;
    if (mi_wrap((uint8_t *)buf, fsize, &im) != 0) {
        fprintf(stderr, "macho_grow: image fails validation (bad magic, or load commands "
                        "that don't fit); refusing to classify\n");
        return -1;
    }
    const uint8_t *sp = im.buf + sizeof(*im.hdr);
    for (uint32_t i = 0; i < im.hdr->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)sp;
        const char *why = NULL;
        switch (lc->cmd) {
        /* Handled by a re-baser above. */
        case LC_FUNCTION_STARTS: case LC_DATA_IN_CODE:
        case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY: case LC_DYLD_EXPORTS_TRIE:
        case LC_SEGMENT_64:
            break;

        /* Inert under a base move: absolute addresses (which do not change),
         * file offsets (shifted by the walk), indices, strings, or build metadata. */
        case LC_SYMTAB: case LC_DYSYMTAB: case LC_UUID:
        case LC_LOAD_DYLIB: case LC_ID_DYLIB: case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB: case LC_LAZY_LOAD_DYLIB: case LC_PREBOUND_DYLIB:
        case LC_LOAD_DYLINKER: case LC_ID_DYLINKER: case LC_DYLD_ENVIRONMENT:
        case LC_RPATH: case LC_MAIN: case LC_UNIXTHREAD: case LC_THREAD:
        case LC_CODE_SIGNATURE: case LC_DYLIB_CODE_SIGN_DRS:
        case LC_ENCRYPTION_INFO: case LC_ENCRYPTION_INFO_64:
        case LC_VERSION_MIN_MACOSX: case LC_VERSION_MIN_IPHONEOS:
        case LC_SOURCE_VERSION: case LC_BUILD_VERSION: case LC_LINKER_OPTION:
        case LC_NOTE: case LC_SUB_FRAMEWORK: case LC_SUB_UMBRELLA:
        case LC_SUB_CLIENT: case LC_SUB_LIBRARY: case LC_TWOLEVEL_HINTS:
        case LC_PREBIND_CKSUM: case LC_ROUTINES_64: case LC_ATOM_INFO:
            break;

        /* Known to carry base-relative payloads we do NOT re-base. */
        case LC_SEGMENT_SPLIT_INFO:
            why = "LC_SEGMENT_SPLIT_INFO carries base-relative offsets that are not re-based";
            break;
        case LC_LINKER_OPTIMIZATION_HINT:
            why = "LC_LINKER_OPTIMIZATION_HINT carries base-relative ULEB offsets that are "
                  "not re-based";
            break;
        case LC_DYLD_CHAINED_FIXUPS:
            why = "LC_DYLD_CHAINED_FIXUPS is not supported here; run patch_macho first to "
                  "convert it to LC_DYLD_INFO_ONLY";
            break;
        default:
            fprintf(stderr, "macho_grow: load command %#x is not classified, so it cannot be "
                            "shown safe to grow past. Unknown means unsafe: it may hold "
                            "offsets from the image base, as LC_DATA_IN_CODE does. Refusing.\n",
                    lc->cmd);
            return -1;
        }
        if (why) {
            fprintf(stderr, "macho_grow: %s. Refusing to grow. Reclaim header bytes instead "
                            "(change_dylib -strip-lc uuid -strip-lc codesig).\n", why);
            return -1;
        }

        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)sp;
            const struct section_64 *sect = (const struct section_64 *)(sp + sizeof *seg);
            for (uint32_t j = 0; j < seg->nsects; j++) {
                uint32_t type = sect[j].flags & SECTION_TYPE;
                if (type > S_INIT_FUNC_OFFSETS) {
                    fprintf(stderr, "macho_grow: %.16s,%.16s has section type %#x, which is not "
                                    "classified; it may hold offsets from the image base. "
                                    "Refusing.\n", sect[j].segname, sect[j].sectname, type);
                    return -1;
                }
            }
        }
        sp += lc->cmdsize;
    }
    return 0;
}

/* ---- plausibility: verification with no "before" to compare against --------
 * mg_verify is stronger, but it needs a snapshot taken before the transform.
 * The wrapper cannot have one: it checks the end state of a pipeline whose
 * earlier stages ran in other processes. This works from the finished file.
 *
 * The useful check is not "is this address inside __text" -- __text is 63 MB on
 * Claude Code, so a one-page error stays comfortably inside it. It is that
 * initializers and compact-unwind entries name FUNCTIONS, so their targets must
 * appear in LC_FUNCTION_STARTS. Measured on 2.1.263: 13/13 first-level, 198/198
 * LSDA and 9/9 initializers land exactly on one of 71,974 known starts.
 *
 * Without LC_FUNCTION_STARTS there is nothing to check against, so this passes
 * rather than refusing -- a weaker guarantee, honestly reported by returning 0. */
static int mg_addr_known(const uint64_t *sorted, int n, uint64_t a) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (sorted[mid] == a) return 1;
        if (sorted[mid] < a) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int mg_plausible(const uint8_t *buf, size_t fsize) {
    const struct mach_header_64 *h = (const struct mach_header_64 *)buf;
    const uint8_t *sp = buf + sizeof *h;
    uint64_t base = 0; uint32_t fsoff = 0, fssize = 0;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)sp;
        if (lc->cmd == LC_FUNCTION_STARTS) {
            const struct linkedit_data_command *d = (const struct linkedit_data_command *)sp;
            fsoff = d->dataoff; fssize = d->datasize;
        }
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)sp;
            if (!base && seg->fileoff == 0 && seg->filesize > 0) base = seg->vmaddr;
        }
        sp += lc->cmdsize;
    }
    if (!base) return -1;
    if (!fsoff || !fssize) return 0;                 /* nothing to check against */
    if ((uint64_t)fsoff + fssize > fsize) return -1;

    uint64_t *starts = (uint64_t *)malloc((size_t)fssize * sizeof(uint64_t));
    uint64_t *addr   = (uint64_t *)malloc(MG_SNAP_MAX * sizeof(uint64_t));
    uint8_t  *kinds  = (uint8_t  *)malloc(MG_SNAP_MAX);
    if (!starts || !addr || !kinds) { free(starts); free(addr); free(kinds); return -1; }

    int ns = mg_funcstarts_decode(buf + fsoff, fssize, base, starts, (int)fssize);
    uint32_t n = 0;
    int rc = 0;
    if (ns <= 0 || mg_collect(buf, fsize, addr, kinds, MG_SNAP_MAX, &n) != 0) {
        rc = -1;
    } else {
        for (uint32_t i = 0; i < n && rc == 0; i++) {
            if (kinds[i] != MG_K_FUNC) continue;      /* only these name functions */
            if (mg_addr_known(starts, ns, addr[i])) continue;
            fprintf(stderr, "macho_grow: implausible -- a base-relative entry names %#llx, "
                            "which is not one of the %d addresses in LC_FUNCTION_STARTS. "
                            "Initializers and unwind entries must land on a function start; "
                            "this is what an un-re-based offset looks like.\n",
                    (unsigned long long)addr[i], ns);
            rc = -1;
        }
    }
    free(starts); free(addr); free(kinds);
    return rc;
}

/*
 * Grow the header pad by at least `grow_req` bytes (rounded up to a page).
 * pbuf is realloc'd, pfsize updated. Returns 0 on success, -1 if the
 * precondition (PIE-style __PAGEZERO large enough) isn't met — in which case
 * the buffer and size are left unchanged.
 */
static int mg_grow_header(uint8_t **pbuf, size_t *pfsize, uint32_t grow_req) {
    uint8_t *buf = *pbuf;
    size_t fsize = *pfsize;
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;

    uint32_t grow = (uint32_t)((grow_req + MG_PAGE - 1) & ~(MG_PAGE - 1));
    if (grow == 0) return 0;

    /* Validate preconditions before mutating anything. The image-base trick is
     * only sound for a PIE executable: it needs a __PAGEZERO to donate vm space
     * from, and it relies on the image being position-independent so that
     * lowering the base (every address shifts by the same amount) is a no-op at
     * load time. A non-PIE image, or a dylib/bundle (no __PAGEZERO), would
     * require fixing up absolute pointers — which this tool deliberately does
     * not do. Refuse loudly rather than silently corrupt.
     *
     * 32-bit stays refused here too, on purpose (toolkit plan Task 5, gap 2):
     * this function and everything it calls -- mg_first_sect_off, mg_collect
     * (which mg_snapshot_take/mg_verify use), mg_classify, mg_unwind_walk,
     * mg_init_offsets_pass, and the LC_SEGMENT_64/section_64 patching loop
     * below -- walk the 64-bit segment/section structs. Supporting 32-bit
     * would mean adding a parallel LC_SEGMENT/struct section path to each of
     * those roughly seven places, in the one file whose correctness already
     * depends on ULEB-exact, snapshot-verified arithmetic (mg_verify,
     * mg_plausible). That is a lot of new surface in the highest-risk part of
     * this toolkit, for a format none of the other six rewriters here support
     * either (src/image.h draws the identical line, deliberately, for the
     * same reason) and that Apple stopped shipping newly linked 10.9-era
     * binaries in years before this toolkit existed. See
     * macho_grow_test.c's test_grow_refuses_32bit_mach_header for the pinned
     * regression test and docs/prior-art.md for the fuller write-up. */
    if (hdr->magic != MH_MAGIC_64) {
        fprintf(stderr, "macho_grow: not a 64-bit Mach-O (magic=0x%x); 32-bit is a "
                        "deliberately unsupported format, not a bug -- see the comment "
                        "above this check\n", hdr->magic);
        return -1;
    }
    if (hdr->filetype != MH_EXECUTE) {
        fprintf(stderr, "macho_grow: only MH_EXECUTE is supported (filetype=%u); the "
                        "image-base trick needs a __PAGEZERO. Use the heavyweight "
                        "shift-up approach for dylibs/bundles (see HEADER_PAD_GROWTH.md)\n",
                hdr->filetype);
        return -1;
    }
    if (!(hdr->flags & MH_PIE)) {
        fprintf(stderr, "macho_grow: executable is not PIE (flags=0x%x); lowering the "
                        "image base would require fixing absolute relocations, which "
                        "this tool does not do (see HEADER_PAD_GROWTH.md)\n", hdr->flags);
        return -1;
    }

    uint32_t insert = mg_first_sect_off(buf, fsize);
    if (insert == UINT32_MAX) return -1;   /* already explained itself on stderr */

    /* We insert space at `insert` (the first section's file offset) and shift
     * everything from there onward. That point must be at/after the end of the
     * load commands, or we'd memmove the tail of the LC table itself. A healthy
     * binary always satisfies this; refuse the anomalous case rather than
     * corrupt it. */
    uint32_t lc_end = (uint32_t)sizeof(*hdr) + hdr->sizeofcmds;
    if (insert < lc_end) {
        fprintf(stderr, "macho_grow: first section (%u) precedes end of load commands "
                        "(%u); refusing to grow a malformed header\n", insert, lc_end);
        return -1;
    }

    /* Locate the donor (__PAGEZERO) and the header-bearing segment (__TEXT). */
    struct segment_command_64 *pagezero = NULL, *text = NULL;
    uint8_t *lcp = buf + sizeof(*hdr);
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *seg = (struct segment_command_64 *)lcp;
            if (strcmp(seg->segname, "__PAGEZERO") == 0) pagezero = seg;
            else if (seg->fileoff == 0 && seg->filesize > 0) text = seg;
        }
        lcp += lc->cmdsize;
    }
    if (!text) {
        fprintf(stderr, "macho_grow: no __TEXT-like segment holds the header\n");
        return -1;
    }
    if (!pagezero || pagezero->vmsize < grow) {
        fprintf(stderr, "macho_grow: need a __PAGEZERO >= %u bytes to lower the image "
                        "base (image-base trick requires a PIE executable)\n", grow);
        return -1;
    }

    /* Locate LC_FUNCTION_STARTS and confirm its base-relative leading delta can
     * absorb `grow` without widening its ULEB encoding (the common case for a
     * page-sized grow). We check BEFORE mutating so a refusal leaves the buffer
     * untouched. dyld ignores function-starts, but avxemu uses it to find
     * function bounds for patch-safety; a stale leading delta makes every bound N
     * low and the emulator declines to patch (a SIGILL storm). fs_dataoff is the
     * ORIGINAL file offset; after the memmove the blob lives at fs_dataoff+grow. */
    uint32_t fs_dataoff = 0, fs_datasize = 0;
    {
        const uint8_t *sp = buf + sizeof(*hdr);
        for (uint32_t i = 0; i < hdr->ncmds; i++) {
            const struct load_command *lc = (const struct load_command *)sp;
            if (lc->cmd == LC_FUNCTION_STARTS) {
                const struct linkedit_data_command *ld =
                    (const struct linkedit_data_command *)sp;
                fs_dataoff = ld->dataoff; fs_datasize = ld->datasize;
                break;
            }
            sp += lc->cmdsize;
        }
    }
    if (fs_dataoff && fs_datasize) {
        uint64_t d0; int n0 = mu_decode(buf + fs_dataoff,
                                             buf + fs_dataoff + fs_datasize, &d0);
        if (n0 == 0) {
            fprintf(stderr, "macho_grow: malformed LC_FUNCTION_STARTS leading delta\n");
            return -1;
        }
        if (mu_minlen(d0 + grow) > n0) {
            fprintf(stderr, "macho_grow: grow of %u would widen the LC_FUNCTION_STARTS "
                            "leading delta (%llu -> %llu crosses a ULEB byte boundary); "
                            "in-place re-encode impossible and __LINKEDIT resize is not "
                            "implemented. Use a smaller grow.\n",
                    grow, (unsigned long long)d0, (unsigned long long)(d0 + grow));
            return -1;
        }
    }

    /* Audit the other base-relative structures before touching the buffer, so a
     * refusal leaves it pristine. Silently shipping a binary whose constructors
     * or exports are `grow` bytes low is far worse than failing here. */
    if (mg_classify(buf, fsize) != 0) return -1;
    if (mg_dice_walk(buf, fsize, grow, 0, 0, NULL, NULL, NULL, 0) != 0) {
        fprintf(stderr, "macho_grow: LC_DATA_IN_CODE is malformed or an entry offset would "
                        "overflow; refusing to grow\n");
        return -1;
    }
    if (mg_unwind_walk(buf, fsize, grow, 0, 0, NULL, NULL, NULL, 0) != 0) {
        fprintf(stderr, "macho_grow: __TEXT,__unwind_info is malformed, uses a layout this "
                        "does not understand, or an offset would overflow; refusing to grow\n");
        return -1;
    }
    if (mg_init_offsets_pass(buf, fsize, grow, 0) != 0) {
        fprintf(stderr, "macho_grow: malformed S_INIT_FUNC_OFFSETS section; refusing to grow\n");
        return -1;
    }
    /* If an address's ULEB would widen, mg_trie_node's in-place patch (below,
     * after the buffer is mutated) can't do it: widening one entry cascades
     * into the byte width of every child-offset ULEB after it in the trie.
     * REBUILD it instead: decode the whole thing, add `grow` to every
     * nonzero address, and re-serialize from scratch with everything
     * minimally encoded (src/trie.c, mt_trie_rebuild) -- adapted from
     * Wowfunhappy's export-trie rebuilder in insert_dylib commit 6d3aa61
     * (public domain/CC0/WTFPL per his own statement, see
     * docs/prior-art.md). Done HERE, before any mutation, so a rebuild
     * failure (malformed trie, or its own MT_TRIE_MAX_DEPTH guard) leaves
     * the buffer untouched, same as every other audit in this function --
     * and so the exact same bytes get patched below as were validated here,
     * with no possibility of the two disagreeing. */
    uint8_t *mg_new_trie = NULL;
    uint32_t mg_new_trie_size = 0;
    int mg_trie_needs_rebuild = 0;
    {
        int r = mg_trie_walk(buf, fsize, grow, 0, 0, NULL, NULL, NULL, 0);
        if (r < 0) {
            fprintf(stderr, "macho_grow: export trie is malformed; refusing to grow\n");
            return -1;
        }
        if (r > 0) {
            uint32_t toff, tsize;
            if (!mg_find_trie(buf, &toff, &tsize) || !toff || !tsize) {
                fprintf(stderr, "macho_grow: internal error locating the export trie that "
                                "just reported needing a wider ULEB\n");
                return -1;
            }
            if (mt_trie_rebuild(buf + toff, tsize, grow, &mg_new_trie, &mg_new_trie_size) != 0) {
                fprintf(stderr, "macho_grow: refusing to grow -- see the trie error above.\n");
                return -1;
            }
            mg_trie_needs_rebuild = 1;
        }
    }

    /* Phase 4 prep: snapshot every base-relative resolved address BEFORE touching
     * a byte, so the verify at the end has something to prove against. If we
     * cannot read them we cannot prove anything, so refuse rather than grow
     * blind. */
    mg_snapshot snap;
    if (mg_snapshot_take(buf, fsize, &snap) != 0) {
        fprintf(stderr, "macho_grow: could not snapshot the base-relative structures; "
                        "refusing to grow without a way to verify the result\n");
        free(mg_new_trie);
        return -1;
    }

    /* Insert `grow` zero bytes after the load commands, shifting file data down. */
    uint8_t *nbuf = (uint8_t *)realloc(buf, fsize + grow);
    if (!nbuf) {
        fprintf(stderr, "macho_grow: realloc failed\n");
        mg_snapshot_free(&snap);
        free(mg_new_trie);
        return -1;
    }
    buf = nbuf;
    /* Hand the new pointer back IMMEDIATELY. realloc may have moved the block and
     * freed the old one, so from here on the caller's *pbuf would otherwise be
     * dangling on any failure return -- a double free waiting for whoever frees
     * on error. The paths below are "cannot happen" assertions, which is exactly
     * the kind of path that is never exercised until it is. */
    *pbuf = buf;
    hdr = (struct mach_header_64 *)buf;
    memmove(buf + insert + grow, buf + insert, fsize - insert);
    memset(buf + insert, 0, grow);

    /* Every size below this point is FINAL_SIZE, not fsize+grow: if the
     * export trie needed rebuilding AND the rebuild is wider than the
     * original trie, the block just below grows __LINKEDIT (and so the
     * file) a second time, independently of the header-pad `grow` above. */
    size_t final_size = fsize + grow;

    /* Patch the header. Load commands live before `insert`, so memmove didn't
     * touch them; we walk them now and adjust only file-offset fields, plus the
     * three VM fields that keep every address fixed. */
    lcp = buf + sizeof(*hdr);
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        switch (lc->cmd) {
        case LC_SEGMENT_64: {
            struct segment_command_64 *seg = (struct segment_command_64 *)lcp;
            /* Identify segments by criteria, not by a saved pointer: realloc
             * above may have moved the buffer, invalidating the pointers found
             * during validation. The header-bearing segment is the one mapped
             * at file offset 0 with content (i.e. __TEXT, not __PAGEZERO). */
            if (strcmp(seg->segname, "__PAGEZERO") == 0) {
                seg->vmsize -= grow;            /* donate space below __TEXT */
            } else if (seg->fileoff == 0 && seg->filesize > 0) {
                seg->vmaddr  -= grow;           /* lower the image base */
                seg->vmsize  += grow;
                seg->filesize += grow;          /* fileoff stays 0 */
            } else if (seg->fileoff >= insert) {
                seg->fileoff += grow;           /* later segment: file moves, vm fixed */
            }
            struct section_64 *sect = (struct section_64 *)(lcp + sizeof(*seg));
            for (uint32_t j = 0; j < seg->nsects; j++) {
                mg_bump(&sect[j].offset, insert, grow);   /* addr stays fixed */
                if (sect[j].reloff) mg_bump(&sect[j].reloff, insert, grow);
            }
            break;
        }
        case LC_SYMTAB: {
            struct symtab_command *c = (struct symtab_command *)lcp;
            mg_bump(&c->symoff, insert, grow);
            mg_bump(&c->stroff, insert, grow);
            break;
        }
        case LC_DYSYMTAB: {
            struct dysymtab_command *c = (struct dysymtab_command *)lcp;
            mg_bump(&c->tocoff, insert, grow);
            mg_bump(&c->modtaboff, insert, grow);
            mg_bump(&c->extrefsymoff, insert, grow);
            mg_bump(&c->indirectsymoff, insert, grow);
            mg_bump(&c->extreloff, insert, grow);
            mg_bump(&c->locreloff, insert, grow);
            break;
        }
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY: {
            struct dyld_info_command *c = (struct dyld_info_command *)lcp;
            mg_bump(&c->rebase_off, insert, grow);
            mg_bump(&c->bind_off, insert, grow);
            mg_bump(&c->weak_bind_off, insert, grow);
            mg_bump(&c->lazy_bind_off, insert, grow);
            mg_bump(&c->export_off, insert, grow);
            break;
        }
        case LC_MAIN: {
            /* entryoff is a file offset within __TEXT; bumping it keeps the
             * entry's vm address fixed (base went down by the same amount). */
            struct entry_point_command *c = (struct entry_point_command *)lcp;
            uint32_t e = (uint32_t)c->entryoff;
            mg_bump(&e, insert, grow);
            c->entryoff = e;
            break;
        }
        case LC_FUNCTION_STARTS:
        case LC_DATA_IN_CODE:
        case LC_CODE_SIGNATURE:
        case LC_SEGMENT_SPLIT_INFO:
        case LC_DYLIB_CODE_SIGN_DRS:
        case LC_LINKER_OPTIMIZATION_HINT:
        case LC_DYLD_EXPORTS_TRIE:
        case LC_DYLD_CHAINED_FIXUPS: {
            struct linkedit_data_command *c = (struct linkedit_data_command *)lcp;
            mg_bump(&c->dataoff, insert, grow);
            break;
        }
        default:
            break;  /* LC_LOAD_DYLIB/DYLINKER/UUID/VERSION_MIN carry no file offsets */
        }
        lcp += lc->cmdsize;
    }

    /* Re-point the dyld4 initializer offsets: the base dropped by `grow`, the
     * constructors did not move, so each offset must gain `grow`. Section file
     * offsets were bumped in the walk above, so these read from the new home.
     * The pre-mutation audit proved this cannot overflow. */
    if (!mg_trie_needs_rebuild) {
        /* The common case (measured: zero widening entries across all 670
         * exports of Claude Code 2.1.263 at 4K/8K/16K grows): every address
         * re-encodes in its ORIGINAL byte width, so the trie -- and every
         * __LINKEDIT offset after it -- keeps its size. */
        if (mg_trie_walk(buf, final_size, grow, 1, 0, NULL, NULL, NULL, 0) != 0) {
            fprintf(stderr, "macho_grow: internal error re-basing the export trie after "
                            "passing the pre-check\n");
            mg_snapshot_free(&snap);
            return -1;
        }
    } else {
        /* At least one address widened. mg_new_trie/mg_new_trie_size (built
         * during the pre-mutation audit, from the SAME bytes -- content is
         * unchanged by the memmove above, only position moved) replace the
         * trie outright. */
        uint32_t toff, tsize;
        if (!mg_find_trie(buf, &toff, &tsize)) {
            fprintf(stderr, "macho_grow: internal error -- the export trie load command "
                            "vanished after growing\n");
            free(mg_new_trie);
            mg_snapshot_free(&snap);
            return -1;
        }
        if (mg_new_trie_size <= tsize) {
            /* Rebuilding from scratch, with everything minimally encoded, can
             * still fit the original space even though an in-place patch of
             * ONE entry's fixed width could not: nothing else in __LINKEDIT
             * moves. Original datasize is kept (not shrunk), with the unused
             * tail zeroed -- a trie's traversal is driven entirely by child
             * offsets from the root, so trailing zero bytes past the last
             * reachable node are simply never read. */
            memcpy(buf + toff, mg_new_trie, mg_new_trie_size);
            memset(buf + toff + mg_new_trie_size, 0, tsize - mg_new_trie_size);
        } else {
            /* Does not fit: __LINKEDIT itself must grow. Append the rebuilt
             * trie right after __LINKEDIT's current end. On every binary this
             * has been checked against (including a real code-signed 10.9
             * binary), that is also the end of the FILE -- __LINKEDIT is the
             * last segment, and a code signature (if any) lives INSIDE
             * __LINKEDIT's declared filesize, so `append_off != final_size`
             * below cannot happen just because a signature is present.
             * Refuse, rather than guess, if it turns out not so anyway.
             * (Appending here strands any existing signature before the new
             * end without extending it to cover the appended bytes, so the
             * signature no longer verifies -- but growing the header pad
             * ALREADY invalidates any signature, for the same reason every
             * other rewriter in this toolkit does: this transform edits file
             * bytes a signature covers. That is a pre-existing, documented
             * limitation (`-strip-lc codesig`), not something new here.) */
            uint8_t *lcp2 = buf + sizeof(struct mach_header_64);
            long linkedit_lc_off = -1;
            struct mach_header_64 *hh2 = (struct mach_header_64 *)buf;
            for (uint32_t i = 0; i < hh2->ncmds; i++) {
                struct load_command *lc2 = (struct load_command *)lcp2;
                if (lc2->cmd == LC_SEGMENT_64) {
                    struct segment_command_64 *seg2 = (struct segment_command_64 *)lcp2;
                    if (strcmp(seg2->segname, "__LINKEDIT") == 0) {
                        linkedit_lc_off = lcp2 - buf;
                        break;   /* first (and only well-formed) __LINKEDIT */
                    }
                }
                lcp2 += lc2->cmdsize;
            }
            /* The export LC is looked up through mg_find_trie_lc, the same
             * function mg_find_trie used just above to locate toff/tsize --
             * not a second hand-rolled scan that could disagree with it
             * about which load command "the" export trie means. */
            long export_lc_off = -1; uint32_t export_lc_cmd = 0;
            mg_find_trie_lc(buf, &export_lc_off, &export_lc_cmd);
            if (linkedit_lc_off < 0 || export_lc_off < 0) {
                fprintf(stderr, "macho_grow: no __LINKEDIT segment (or no export-trie load "
                                "command) to grow the rebuilt export trie into; refusing\n");
                free(mg_new_trie);
                mg_snapshot_free(&snap);
                return -1;
            }
            /* dataoff/export_off/datasize/export_size and this segment's
             * fileoff/filesize are all 32-bit fields; refuse rather than
             * silently wrap if this file is implausibly large. */
            struct segment_command_64 *linkedit =
                (struct segment_command_64 *)(buf + linkedit_lc_off);
            uint64_t append_off = linkedit->fileoff + linkedit->filesize;
            if (append_off != final_size) {
                fprintf(stderr, "macho_grow: export trie widened, but __LINKEDIT (ending at "
                                "%llu) is not the last thing in the file (file is %zu bytes); "
                                "appending would overwrite unknown data or leave a hole, so "
                                "refusing rather than guess\n",
                        (unsigned long long)append_off, final_size);
                free(mg_new_trie);
                mg_snapshot_free(&snap);
                return -1;
            }
            if (append_off > UINT32_MAX || mg_new_trie_size > UINT32_MAX - append_off) {
                fprintf(stderr, "macho_grow: rebuilt export trie would land past a 32-bit "
                                "file-offset field; refusing\n");
                free(mg_new_trie);
                mg_snapshot_free(&snap);
                return -1;
            }
            uint64_t new_total = final_size + mg_new_trie_size;
            uint8_t *g = (uint8_t *)realloc(buf, (size_t)new_total);
            if (!g) {
                fprintf(stderr, "macho_grow: realloc failed growing __LINKEDIT for the "
                                "rebuilt export trie\n");
                free(mg_new_trie);
                mg_snapshot_free(&snap);
                return -1;
            }
            buf = g; *pbuf = buf; hdr = (struct mach_header_64 *)buf;
            memset(buf + final_size, 0, mg_new_trie_size);
            memcpy(buf + append_off, mg_new_trie, mg_new_trie_size);
            final_size = (size_t)new_total;

            /* Re-derive every pointer from its saved BYTE OFFSET, not a raw
             * pointer taken before the realloc just above -- which may have
             * moved the buffer, same reasoning as the segment-patch loop's
             * own comment about this earlier in this function. */
            linkedit = (struct segment_command_64 *)(buf + linkedit_lc_off);
            uint64_t new_le_filesize = (append_off + mg_new_trie_size) - linkedit->fileoff;
            linkedit->filesize = new_le_filesize;
            uint64_t new_le_vmsize = (new_le_filesize + MG_PAGE - 1) & ~(MG_PAGE - 1);
            if (new_le_vmsize > linkedit->vmsize) linkedit->vmsize = new_le_vmsize;

            struct load_command *elc = (struct load_command *)(buf + export_lc_off);
            if (export_lc_cmd == LC_DYLD_INFO || export_lc_cmd == LC_DYLD_INFO_ONLY) {
                struct dyld_info_command *d = (struct dyld_info_command *)elc;
                d->export_off = (uint32_t)append_off;
                d->export_size = mg_new_trie_size;
            } else {
                struct linkedit_data_command *d = (struct linkedit_data_command *)elc;
                d->dataoff = (uint32_t)append_off;
                d->datasize = mg_new_trie_size;
            }
        }
    }
    free(mg_new_trie);
    mg_new_trie = NULL;
    if (mg_dice_walk(buf, final_size, grow, 1, 0, NULL, NULL, NULL, 0) != 0) {
        fprintf(stderr, "macho_grow: internal error re-basing LC_DATA_IN_CODE after passing "
                        "the pre-check\n");
        mg_snapshot_free(&snap);
        return -1;
    }
    if (mg_unwind_walk(buf, final_size, grow, 1, 0, NULL, NULL, NULL, 0) != 0) {
        fprintf(stderr, "macho_grow: internal error re-basing __TEXT,__unwind_info after "
                        "passing the pre-check\n");
        mg_snapshot_free(&snap);
        return -1;
    }
    if (mg_init_offsets_pass(buf, final_size, grow, 1) != 0) {
        fprintf(stderr, "macho_grow: internal error patching S_INIT_FUNC_OFFSETS after "
                        "passing the pre-check\n");
        mg_snapshot_free(&snap);
        return -1;
    }

    /* Re-encode the base-relative LC_FUNCTION_STARTS leading delta: the base
     * dropped by `grow`, so the first delta must gain `grow` to keep every
     * function's absolute address fixed. The blob moved with the memmove; it now
     * lives at fs_dataoff+grow. The pre-mutation check above already proved the
     * width is preserved, so this cannot widen — a nonzero return is a bug. */
    if (fs_dataoff && fs_datasize) {
        int r = mg_reencode_funcstarts_base(buf + fs_dataoff + grow, fs_datasize, grow);
        if (r != 1) {
            fprintf(stderr, "macho_grow: internal error re-encoding function-starts "
                            "leading delta (r=%d) after passing the width pre-check\n", r);
            mg_snapshot_free(&snap);
            return -1;
        }
    }

    /* Phase 4: prove it. Every base-relative structure must resolve exactly where
     * it did before. A mismatch means a handler did not run, ran twice, or ran
     * with the wrong delta -- all of which produce a binary that loads and is
     * wrong, so this is the last chance to catch it. */
    if (mg_verify(buf, final_size, &snap) != 0) {
        mg_snapshot_free(&snap);
        /* The buffer has been transformed and is NOT safe to write. *pbuf already
         * points at it (set right after the realloc) so the caller can free it;
         * the nonzero return says: discard it. */
        return -1;
    }
    mg_snapshot_free(&snap);

    /* And independently: do the results still name plausible targets? mg_verify
     * proves nothing MOVED, which is silent about a structure we never collected.
     * This asks a different question of the finished file -- do initializers and
     * unwind entries still land on function starts -- so the two fail for
     * different reasons. */
    if (mg_plausible(buf, final_size) != 0) {
        fprintf(stderr, "macho_grow: the grown image does not pass its own plausibility "
                        "check; refusing. Discard this buffer.\n");
        return -1;
    }

    *pfsize = final_size;   /* *pbuf was set right after the (last) realloc */
    return 0;
}

#endif /* MACHO_GROW_H */
