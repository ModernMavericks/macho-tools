/* grow.c -- see grow.h for the design and every function's contract.
 *
 * This was macho_grow.h, a header-only library, until this move: every
 * function below was `static` with internal linkage and a body sitting
 * directly in the header. Splitting into grow.h (declarations) + grow.c
 * (definitions) is why each one below lost its `static` -- external linkage
 * is what a declaration in a header now promises callers in other
 * translation units (change_dylib.c, cli/macho9.c, tests/grow_test.c).
 * Nothing else changed in this move; characterize and the (also-moved)
 * grow_test are the proof. */

#include "grow.h"

/* mg_first_sect_off's mi_each_lc callback: track the lowest LC_SEGMENT_64
 * section file offset seen so far in ctx->first. Always returns 0 (never
 * stops early) -- every command must be visited, there is no refusal here. */
static int mg_first_sect_cb(const struct load_command *lc, void *ctx_) {
    uint32_t *first = (uint32_t *)ctx_;
    if (lc->cmd == LC_SEGMENT_64) {
        const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
        const struct section_64 *sect =
            (const struct section_64 *)((const uint8_t *)lc + sizeof(*seg));
        for (uint32_t j = 0; j < seg->nsects; j++)
            if (sect[j].offset && sect[j].offset < *first) *first = sect[j].offset;
    }
    return 0;
}

uint32_t mg_first_sect_off(const uint8_t *buf, size_t fsize) {
    mi_image im;
    /* mi_wrap's own signature is necessarily non-const: mi_image.buf is
     * uint8_t* because OTHER callers (build_lcs, mg_grow_header itself) use
     * the same function to get a WRITABLE view. This function is not one of
     * them -- everything below reads im.buf/im.hdr and never writes through
     * either -- so the cast only works around mi_wrap's shared signature, it
     * does not let this function itself break the "never modifies buf"
     * contract its own `const uint8_t *buf` parameter promises callers. */
    if (mi_wrap((uint8_t *)buf, fsize, &im) != 0) {
        fprintf(stderr, "macho_grow: image fails validation (bad magic, or load commands "
                        "that don't fit); refusing to guess the header pad boundary\n");
        return UINT32_MAX;
    }

    uint32_t first = UINT32_MAX;
    mi_each_lc(&im, mg_first_sect_cb, &first);
    /* Still the sentinel: no section has file data. Say so; see grow.h for
     * why this is not a default offset. */
    return first == UINT32_MAX ? MG_NO_SECTION_DATA : first;
}

int mg_ensure_pad(uint8_t **pbuf, size_t *pfsize, uint32_t need_end,
                  int allow_grow, const char *label) {
    uint32_t first = mg_first_sect_off(*pbuf, *pfsize);
    if (first == UINT32_MAX) {
        fprintf(stderr, "ERROR: %s fails validation; refusing (see above)\n", label);
        return -1;
    }
    if (first == MG_NO_SECTION_DATA) {
        fprintf(stderr, "ERROR: %s: no section data bounds the header pad; "
                        "refusing rather than guess where it ends\n", label);
        return -1;
    }
    /* `first` bounds every write into the pad, and it comes straight from
     * the file (mi_wrap does not check section file ranges). Past the
     * buffer's end it is no bound: answering "fits" against it would let a
     * caller write past the end of the buffer. */
    if (first > *pfsize) {
        fprintf(stderr, "ERROR: %s: no section data within the image; refusing\n", label);
        return -1;
    }
    if (need_end <= first) return 0;

    const struct mach_header_64 *hdr = (const struct mach_header_64 *)*pbuf;
    uint32_t cur_lc_end = (uint32_t)sizeof *hdr + hdr->sizeofcmds;
    uint32_t pad_avail  = first > cur_lc_end ? first - cur_lc_end : 0;
    uint32_t new_lcs    = need_end - (uint32_t)sizeof *hdr;
    if (!allow_grow) {
        fprintf(stderr, "ERROR: %s: new LCs (%u bytes) don't fit in header pad (%u avail); "
                        "growing the header needs allow-grow\n", label, new_lcs, pad_avail);
        return -1;
    }

    uint32_t grow_req = need_end - first;
    printf("%s: load commands need %u more bytes than the %u-byte pad; growing header...\n",
           label, grow_req, pad_avail);
    if (mg_grow_header(pbuf, pfsize, grow_req) != 0) {
        fprintf(stderr, "ERROR: %s: new LCs (%u bytes) don't fit and header could not be grown\n",
                label, new_lcs);
        return -1;
    }
    first = mg_first_sect_off(*pbuf, *pfsize);
    if (first == UINT32_MAX) {
        fprintf(stderr, "ERROR: %s: header grow produced an image that fails validation\n", label);
        return -1;
    }
    /* Growth moves section data and never removes it, and this function
     * refused an image with none above. Checked anyway: `first` is printed
     * as the new boundary just below. */
    if (first == MG_NO_SECTION_DATA) {
        fprintf(stderr, "ERROR: %s: header grow left no section data to bound the pad\n", label);
        return -1;
    }
    printf("%s: grew header pad: first sect now at %u (%u bytes available)\n",
           label, first, first - cur_lc_end);
    return 0;
}

int mg_reencode_funcstarts_base(uint8_t *blob, uint32_t size, uint32_t grow) {
    if (size == 0) return -1;
    uint64_t d0; int n0 = mu_decode(blob, blob + size, &d0);
    if (n0 == 0) return -1;
    uint64_t nd = d0 + grow;
    if (mu_minlen(nd) > n0) return 0;          /* would widen -> caller refuses */
    return mu_encode_fixed(blob, nd, n0) ? 1 : 0;
}

int mg_funcstarts_decode(const uint8_t *blob, uint32_t size,
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

struct mg_init_offsets_ctx {
    uint8_t *buf;
    size_t fsize;
    uint32_t grow;
    int patch;
};

/* mg_init_offsets_pass's mi_each_lc callback: edits S_INIT_FUNC_OFFSETS
 * section CONTENT in place (patch=1) or just audits it (patch=0) -- never
 * lc->cmd/cmdsize/hdr->ncmds, so it stays inside mi_each_lc's mutation
 * contract. Returns non-zero to stop the walk on the first malformed/unsafe
 * section, exactly the refusal mg_init_offsets_pass itself used to return
 * early for. */
static int mg_init_offsets_cb(const struct load_command *lc, void *ctx_) {
    struct mg_init_offsets_ctx *ctx = (struct mg_init_offsets_ctx *)ctx_;
    if (lc->cmd != LC_SEGMENT_64) return 0;
    struct segment_command_64 *seg = (struct segment_command_64 *)lc;
    struct section_64 *sect = (struct section_64 *)(seg + 1);
    for (uint32_t j = 0; j < seg->nsects; j++) {
        if ((sect[j].flags & SECTION_TYPE) != S_INIT_FUNC_OFFSETS) continue;
        if (sect[j].size % 4) return -1;
        if ((uint64_t)sect[j].offset + sect[j].size > (uint64_t)ctx->fsize) return -1;
        uint32_t n = (uint32_t)(sect[j].size / 4);
        uint32_t *e = (uint32_t *)(ctx->buf + sect[j].offset);
        for (uint32_t k = 0; k < n; k++) {
            if (!ctx->patch) { if (e[k] > UINT32_MAX - ctx->grow) return -1; }
            else e[k] += ctx->grow;
        }
    }
    return 0;
}

int mg_init_offsets_pass(uint8_t *buf, size_t fsize, uint32_t grow, int patch) {
    mi_image im;
    if (mi_wrap(buf, fsize, &im) != 0) return -1;
    struct mg_init_offsets_ctx ctx = { buf, fsize, grow, patch };
    return mi_each_lc(&im, mg_init_offsets_cb, &ctx) ? 0 : -1;
}

int mg_trie_scan(const uint8_t *trie, uint32_t size, uint32_t off, int depth) {
    if (depth > MT_TRIE_MAX_DEPTH || off >= size) return -1;
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

struct mg_collect_ctx {
    const uint8_t *buf;
    size_t fsize;
    uint64_t base;
    uint64_t *out;
    uint8_t *kinds;
    uint32_t max;
    uint32_t n;
};

/* mg_collect's mi_each_lc callback: LC_FUNCTION_STARTS' leading (base-relative)
 * delta, and every S_INIT_FUNC_OFFSETS entry, both resolved to an absolute
 * address and appended to ctx->out. Read-only over the command chain; returns
 * non-zero to stop on the first malformed/overflowing structure, same as the
 * hand-rolled loop's early `return -1`. */
static int mg_collect_cb(const struct load_command *lc, void *ctx_) {
    struct mg_collect_ctx *ctx = (struct mg_collect_ctx *)ctx_;
    if (lc->cmd == LC_FUNCTION_STARTS) {
        const struct linkedit_data_command *d = (const struct linkedit_data_command *)lc;
        if (d->datasize) {
            if ((size_t)d->dataoff + d->datasize > ctx->fsize) return -1;
            uint64_t d0;
            if (mu_decode(ctx->buf + d->dataoff, ctx->buf + d->dataoff + d->datasize, &d0) == 0)
                return -1;
            if (ctx->n >= ctx->max) return -1;
            if (ctx->kinds) ctx->kinds[ctx->n] = MG_K_FUNC;   /* the first function's address */
            ctx->out[ctx->n++] = ctx->base + d0;
        }
    }
    if (lc->cmd == LC_SEGMENT_64) {
        const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
        const struct section_64 *sect = (const struct section_64 *)(seg + 1);
        for (uint32_t j = 0; j < seg->nsects; j++) {
            if ((sect[j].flags & SECTION_TYPE) != S_INIT_FUNC_OFFSETS) continue;
            if ((size_t)sect[j].offset + sect[j].size > ctx->fsize) return -1;
            const uint32_t *e = (const uint32_t *)(ctx->buf + sect[j].offset);
            uint64_t cnt = sect[j].size / sizeof(uint32_t);
            for (uint64_t k = 0; k < cnt; k++) {
                if (ctx->n >= ctx->max) return -1;
                if (ctx->kinds) ctx->kinds[ctx->n] = MG_K_FUNC;   /* initializers are functions */
                ctx->out[ctx->n++] = ctx->base + e[k];
            }
        }
    }
    return 0;
}

int mg_collect(const uint8_t *buf, size_t fsize, uint64_t *out, uint8_t *kinds,
                      uint32_t max, uint32_t *n_out) {
    mi_image im;
    if (mi_wrap((uint8_t *)buf, fsize, &im) != 0) return -1;
    /* image base first: __TEXT is the segment mapping the header (fileoff 0,
     * has content). mi_image_base, not mi_text_base: a dylib is linked at
     * base 0, and reading that 0 as mi_text_base's "no segment maps the
     * header" sentinel is what made this refuse every dylib outright. Only
     * the absence of a header-bearing segment is a refusal here. */
    uint64_t base;
    if (mi_image_base(&im, &base) != 0) return -1;

    struct mg_collect_ctx ctx = { buf, fsize, base, out, kinds, max, 0 };
    if (!mi_each_lc(&im, mg_collect_cb, &ctx)) return -1;
    uint32_t n = ctx.n;

    /* Compact unwind last, so element order is stable across before/after. The
     * cast is safe: with `out` non-NULL the walker only reads. */
    if (mg_trie_walk((uint8_t *)buf, fsize, 0, 0, base, out, kinds, &n, max) != 0) return -1;
    if (mg_dice_walk((uint8_t *)buf, fsize, 0, 0, base, out, kinds, &n, max) != 0) return -1;
    if (mg_unwind_walk((uint8_t *)buf, fsize, 0, 0, base, out, kinds, &n, max) != 0) return -1;
    *n_out = n;
    return 0;
}

int mg_snapshot_take(const uint8_t *buf, size_t fsize, mg_snapshot *s) {
    s->addr = (uint64_t *)malloc(MG_SNAP_MAX * sizeof(uint64_t));
    if (!s->addr) return -1;
    if (mg_collect(buf, fsize, s->addr, NULL, MG_SNAP_MAX, &s->n) != 0) {
        free(s->addr); s->addr = NULL; s->n = 0; return -1;
    }
    return 0;
}

void mg_snapshot_free(mg_snapshot *s) { free(s->addr); s->addr = NULL; s->n = 0; }

int mg_verify(const uint8_t *buf, size_t fsize, const mg_snapshot *before) {
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

int mg_uw_bump(uint8_t *p, uint32_t grow, int patch) {
    uint32_t v; memcpy(&v, p, sizeof v);
    if (v > 0xffffffffu - grow) return -1;      /* would overflow the 32-bit field */
    if (patch) { v += grow; memcpy(p, &v, sizeof v); }
    return 0;
}

struct mg_unwind_find_ctx {
    uint8_t *buf;
    size_t fsize;
    uint8_t *u;         /* result: section content, or NULL if not found */
    uint32_t usz;
    int zero_size;      /* matched a __unwind_info section, but its size is 0 */
    int overflow;       /* matched, but offset+size runs past fsize */
};

/* mg_unwind_walk's mi_each_lc callback: find the FIRST LC_SEGMENT_64 section
 * named "__unwind_info" and stop -- this is a find-first search, not an
 * iterate-everything walk, so it always returns 1 once it has visited a
 * segment with a matching-named section (whatever the outcome: found, zero
 * size, or overflow), matching the original loop's `&& !u` early exit. */
static int mg_unwind_find_cb(const struct load_command *lc, void *ctx_) {
    struct mg_unwind_find_ctx *ctx = (struct mg_unwind_find_ctx *)ctx_;
    if (lc->cmd != LC_SEGMENT_64) return 0;
    const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
    const struct section_64 *sect = (const struct section_64 *)(seg + 1);
    for (uint32_t j = 0; j < seg->nsects; j++) {
        if (strncmp(sect[j].sectname, "__unwind_info", sizeof sect[j].sectname)) continue;
        if (!sect[j].size) { ctx->zero_size = 1; return 1; }
        if ((uint64_t)sect[j].offset + sect[j].size > ctx->fsize) { ctx->overflow = 1; return 1; }
        ctx->u = ctx->buf + sect[j].offset; ctx->usz = (uint32_t)sect[j].size;
        return 1;
    }
    return 0;
}

int mg_unwind_walk(uint8_t *buf, size_t fsize, uint32_t grow, int patch,
                          uint64_t base, uint64_t *out, uint8_t *kinds,
                          uint32_t *n, uint32_t max) {
    mi_image im;
    if (mi_wrap(buf, fsize, &im) != 0) return -1;
    struct mg_unwind_find_ctx fctx = { buf, fsize, NULL, 0, 0, 0 };
    mi_each_lc(&im, mg_unwind_find_cb, &fctx);
    if (fctx.zero_size) return 0;
    if (fctx.overflow) return -1;
    uint8_t *u = fctx.u; uint32_t usz = fctx.usz;
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

struct mg_dice_ctx {
    uint8_t *buf;
    uint32_t grow;
    int patch;
    uint64_t base;
    uint64_t *out;
    uint8_t *kinds;
    uint32_t *n;
    uint32_t max;
    size_t fsize;
    int error;
};

/* mg_dice_walk's mi_each_lc callback: LC_DATA_IN_CODE is a find-first search
 * (there is at most one), so the callback does the ENTIRE original body --
 * validate, then either collect (out != NULL) or bump (patch) every entry --
 * and always returns 1 to stop once it has visited that command, success or
 * failure alike; ctx->error carries which. */
static int mg_dice_cb(const struct load_command *lc, void *ctx_) {
    struct mg_dice_ctx *ctx = (struct mg_dice_ctx *)ctx_;
    if (lc->cmd != LC_DATA_IN_CODE) return 0;
    const struct linkedit_data_command *d = (const struct linkedit_data_command *)lc;
    if (!d->datasize) return 1;
    if ((uint64_t)d->dataoff + d->datasize > ctx->fsize) { ctx->error = 1; return 1; }
    if (d->datasize % 8) { ctx->error = 1; return 1; }          /* not a whole number of entries */
    uint8_t *e = ctx->buf + d->dataoff;
    for (uint32_t k = 0; k < d->datasize; k += 8) {
        if (ctx->out) {
            if (*ctx->n >= ctx->max) { ctx->error = 1; return 1; }
            uint32_t v; memcpy(&v, e + k, sizeof v);
            if (ctx->kinds) ctx->kinds[*ctx->n] = MG_K_ANY;   /* jump tables sit mid-function */
            ctx->out[(*ctx->n)++] = ctx->base + v;
        } else if (mg_uw_bump(e + k, ctx->grow, ctx->patch) != 0) {
            ctx->error = 1; return 1;
        }
    }
    return 1;
}

int mg_dice_walk(uint8_t *buf, size_t fsize, uint32_t grow, int patch,
                        uint64_t base, uint64_t *out, uint8_t *kinds,
                        uint32_t *n, uint32_t max) {
    mi_image im;
    if (mi_wrap(buf, fsize, &im) != 0) return -1;
    struct mg_dice_ctx ctx = { buf, grow, patch, base, out, kinds, n, max, fsize, 0 };
    mi_each_lc(&im, mg_dice_cb, &ctx);
    return ctx.error ? -1 : 0;
}

int mg_trie_node(uint8_t *trie, uint32_t size, uint32_t off, int depth,
                        uint32_t grow, int patch, uint64_t base,
                        uint64_t *out, uint8_t *kinds, uint32_t *n, uint32_t max,
                        uint8_t *seen) {
    if (depth > MT_TRIE_MAX_DEPTH || off >= size) return -1;
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

/* Was left as a hand-rolled walk on the grounds that every call site already
 * held a validated buffer -- a code-review round found that reasoning
 * incomplete: this symbol is EXPORTED from grow.h, strides lc->cmdsize from
 * h->ncmds with NO bounds check of its own, and was therefore safe only by
 * coincidence -- exactly the "safe by coincidence" hazard src/image.h's own
 * header comment names as this module's reason to exist. Every one of the
 * five call sites (grow.c's own mg_find_trie below, mg_grow_header's
 * pre-realloc audit and post-realloc widen-append path, and
 * tests/grow_test.c's two) already had `fsize`/`final_size` in scope one
 * line away, so the fix costs one parameter, not a redesign. */
struct mg_trie_lc_ctx { const uint8_t *buf; long lc_off; uint32_t cmd; int found; };
static int mg_trie_lc_cb(const struct load_command *lc, void *ctx_) {
    struct mg_trie_lc_ctx *ctx = (struct mg_trie_lc_ctx *)ctx_;
    if (lc->cmd == LC_DYLD_INFO || lc->cmd == LC_DYLD_INFO_ONLY ||
        lc->cmd == LC_DYLD_EXPORTS_TRIE) {
        ctx->lc_off = (const uint8_t *)lc - ctx->buf;
        ctx->cmd = lc->cmd;
        ctx->found = 1;
        return 1;
    }
    return 0;
}

int mg_find_trie_lc(const uint8_t *buf, size_t fsize, long *lc_off, uint32_t *cmd) {
    mi_image im;
    if (mi_wrap((uint8_t *)buf, fsize, &im) != 0) return 0;
    struct mg_trie_lc_ctx ctx = { buf, 0, 0, 0 };
    mi_each_lc(&im, mg_trie_lc_cb, &ctx);
    if (!ctx.found) return 0;
    *lc_off = ctx.lc_off; *cmd = ctx.cmd;
    return 1;
}

int mg_find_trie(const uint8_t *buf, size_t fsize, uint32_t *off, uint32_t *size) {
    long lc_off; uint32_t cmd;
    if (!mg_find_trie_lc(buf, fsize, &lc_off, &cmd)) return 0;
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

int mg_trie_walk(uint8_t *buf, size_t fsize, uint32_t grow, int patch,
                        uint64_t base, uint64_t *out, uint8_t *kinds,
                        uint32_t *n, uint32_t max) {
    uint32_t off, size;
    if (!mg_find_trie(buf, fsize, &off, &size)) return 0;
    if (!off || !size) return 0;
    if ((uint64_t)off + size > fsize) return -1;
    uint8_t *seen = (uint8_t *)calloc(size, 1);
    if (!seen) return -1;
    int r = mg_trie_node(buf + off, size, 0, 0, grow, patch, base, out, kinds, n, max, seen);
    free(seen);
    return r;
}

/* mg_classify's mi_each_lc callback: classify one load command (and, for
 * LC_SEGMENT_64, every one of its sections), refusing to stop the walk the
 * instant something unclassified turns up. Every `return -1` here is exactly
 * the hand-rolled loop's early `return -1` -- "unknown means unsafe", never
 * widened to keep going. */
static int mg_classify_cb(const struct load_command *lc, void *ctx_) {
    (void)ctx_;
    const char *why = NULL;
    switch (lc->cmd) {
        /* Handled by a re-baser above. */
        case LC_FUNCTION_STARTS: case LC_DATA_IN_CODE:
        case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY: case LC_DYLD_EXPORTS_TRIE:
        case LC_SEGMENT_64:
            break;

        /* Inert under a base move: absolute addresses (which do not change),
         * file offsets (shifted by the walk), indices, strings, or build metadata.
         *
         * The seven case labels below this comment (LC_SYMTAB through
         * LC_ENCRYPTION_INFO_64) are generated from src/linkedit.h's
         * ML_PLAIN_OFFSET_LCS, not hand-typed here -- see that macro's own
         * comment for what this couples (this accept decision and
         * ml_bump_lc's matching case cannot disagree for this group; a
         * load command cannot be added here without also being added
         * there, checked at link time) and what it deliberately does not
         * (a load command needing content re-basing on top of its file
         * offset, or one ml_bump_lc has never been taught at all). */
#define GROW_PLAIN_OFFSET_CASE(cmd) case cmd:
        ML_PLAIN_OFFSET_LCS(GROW_PLAIN_OFFSET_CASE)
#undef GROW_PLAIN_OFFSET_CASE
        case LC_UUID:
        case LC_LOAD_DYLIB: case LC_ID_DYLIB: case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB: case LC_LAZY_LOAD_DYLIB: case LC_PREBOUND_DYLIB:
        case LC_LOAD_DYLINKER: case LC_ID_DYLINKER: case LC_DYLD_ENVIRONMENT:
        case LC_RPATH: case LC_MAIN: case LC_UNIXTHREAD: case LC_THREAD:
        case LC_VERSION_MIN_MACOSX: case LC_VERSION_MIN_IPHONEOS:
        case LC_SOURCE_VERSION: case LC_BUILD_VERSION: case LC_LINKER_OPTION:
        case LC_SUB_FRAMEWORK: case LC_SUB_UMBRELLA:
        case LC_SUB_CLIENT: case LC_SUB_LIBRARY:
        case LC_PREBIND_CKSUM: case LC_ROUTINES_64:
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
            why = "LC_DYLD_CHAINED_FIXUPS: chained pointers encode offsets from the "
                  "image base, which growing moves; convert them first (`fixups set "
                  "classic` in an edit script, or `macho9 declassify`)";
            break;
        /* LC_NOTE (note_command: a uint64_t offset/size pair, per publicly
         * documented ld64/dyld source) and LC_ATOM_INFO (reported elsewhere
         * as a plain linkedit_data_command) each carry a real file offset
         * that src/linkedit.h's table does NOT bump -- neither struct's
         * exact layout could be verified against any header available
         * while that module was written (both postdate the 10.9 SDK and
         * the modern host SDK on hand). Refuse rather than guess a shape
         * and silently leave that file offset `grow` bytes low -- the tool
         * would otherwise report success on a binary that will not load.
         * See src/linkedit.h's own top comment for the fuller note. */
        case LC_NOTE:
            why = "LC_NOTE carries a file offset (note_command.offset) this tool does not "
                  "verify or re-base";
            break;
        case LC_ATOM_INFO:
            why = "LC_ATOM_INFO carries a file offset (dataoff) this tool does not verify "
                  "or re-base";
            break;
        default:
            fprintf(stderr, "macho_grow: load command %#x is not classified, so it cannot be "
                            "shown safe to grow past. Unknown means unsafe: it may hold "
                            "offsets from the image base, as LC_DATA_IN_CODE does. Refusing.\n",
                    lc->cmd);
            return -1;
        }
        if (why) {
            fprintf(stderr, "macho_grow: %s. Refusing to grow. Reclaim header bytes "
                            "instead by deleting load commands (uuid, codesig).\n", why);
            return -1;
        }

        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
            const struct section_64 *sect = (const struct section_64 *)(seg + 1);
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
    return 0;
}

int mg_classify(const uint8_t *buf, size_t fsize) {
    mi_image im;
    /* Same reasoning as mg_first_sect_off's identical cast above: mi_wrap's
     * signature is non-const only because some OTHER caller needs a
     * writable view; mg_classify_cb only reads through `lc`, never writes,
     * so casting away const here does not let this function itself violate
     * its own `const uint8_t *buf` promise to callers. */
    if (mi_wrap((uint8_t *)buf, fsize, &im) != 0) {
        fprintf(stderr, "macho_grow: image fails validation (bad magic, or load commands "
                        "that don't fit); refusing to classify\n");
        return -1;
    }
    return mi_each_lc(&im, mg_classify_cb, NULL) ? 0 : -1;
}

int mg_addr_known(const uint64_t *sorted, int n, uint64_t a) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (sorted[mid] == a) return 1;
        if (sorted[mid] < a) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

struct mg_plausible_find_ctx {
    uint32_t fsoff, fssize;
};

/* mg_plausible's mi_each_lc callback: capture the LAST LC_FUNCTION_STARTS
 * seen (matching the original loop, which had no `break` and so kept
 * overwriting fsoff/fssize on every match -- a well-formed image has at
 * most one, so this only matters for a malformed one, and is preserved
 * exactly rather than "fixed" into a first-match). Never stops early: every
 * command must be visited, same as the original unconditional loop. */
static int mg_plausible_find_cb(const struct load_command *lc, void *ctx_) {
    struct mg_plausible_find_ctx *ctx = (struct mg_plausible_find_ctx *)ctx_;
    if (lc->cmd == LC_FUNCTION_STARTS) {
        const struct linkedit_data_command *d = (const struct linkedit_data_command *)lc;
        ctx->fsoff = d->dataoff; ctx->fssize = d->datasize;
    }
    return 0;
}

int mg_plausible(const uint8_t *buf, size_t fsize) {
    mi_image im;
    if (mi_wrap((uint8_t *)buf, fsize, &im) != 0) {
        fprintf(stderr, "macho_grow: implausible -- not a 64-bit Mach-O with a load-command "
                        "chain this can walk\n");
        return -1;
    }
    /* image base: the first segment mapping the header (fileoff 0 with
     * content). Via mi_image_base so a base of 0 -- every dylib -- is a
     * legitimate answer rather than the "not found" sentinel. Reading it as
     * the sentinel is what made this gate bail at its precondition on every
     * dylib on the machine, so the LC_FUNCTION_STARTS heuristic below never
     * ran and `verify` printed a verdict it had not reached.
     *
     * This path says so on stderr because the caller that matters prints
     * "FAILED (see above)" (cli/macho9.c's cmd_verify) and a silent -1 here
     * is what made that line contentless -- the exact fingerprint this bug
     * was finally identified by. The refusal is correct and now rare; it
     * should still be legible when it happens.
     *
     * That now holds for EVERY refusal this function can return: each -1
     * below is preceded by its own stderr line naming the fact that caused
     * it. Adding a refusal path here without one reintroduces the
     * contentless "FAILED (see above)". */
    uint64_t base;
    if (mi_image_base(&im, &base) != 0) {
        fprintf(stderr, "macho_grow: implausible -- no segment maps the header, so there "
                        "is no image base to resolve base-relative entries against\n");
        return -1;
    }

    struct mg_plausible_find_ctx fctx = { 0, 0 };
    mi_each_lc(&im, mg_plausible_find_cb, &fctx);
    uint32_t fsoff = fctx.fsoff, fssize = fctx.fssize;
    if (!fsoff || !fssize) return 0;                 /* nothing to check against */
    if ((uint64_t)fsoff + fssize > fsize) {
        fprintf(stderr, "macho_grow: implausible -- LC_FUNCTION_STARTS claims %u bytes at "
                        "offset %u, which runs past the end of the %llu-byte file\n",
                fssize, fsoff, (unsigned long long)fsize);
        return -1;
    }

    uint64_t *starts = (uint64_t *)malloc((size_t)fssize * sizeof(uint64_t));
    uint64_t *addr   = (uint64_t *)malloc(MG_SNAP_MAX * sizeof(uint64_t));
    uint8_t  *kinds  = (uint8_t  *)malloc(MG_SNAP_MAX);
    if (!starts || !addr || !kinds) {
        fprintf(stderr, "macho_grow: cannot check plausibility -- out of memory\n");
        free(starts); free(addr); free(kinds); return -1;
    }

    /* Three unrelated outcomes, three answers. `ns <= 0 || mg_collect(...)`
     * used to fold all of them into one silent -1, and the one that is not a
     * refusal at all was the one that bit: a dylib with NO CODE, whose
     * function-starts list is therefore empty. The trigger is __text size 0
     * -- then LC_FUNCTION_STARTS is datasize=8, all eight bytes zero, which
     * decodes to ns == 0. It was refused contentlessly through `verify`, and
     * through the rewrite path with a message about "offsets that name no
     * known function" when the image has no function starts to name anything.
     *
     * On the provenance of the instance that found this, because the first
     * two tries got it wrong: it was /usr/lib/swift/libswiftObjectiveC.dylib,
     * which is NOT stock 10.9 (Swift postdates 10.9 by a year) and is NOT
     * shipped by ModernMavericks swift-runtime either (that package's BOM
     * carries only libswiftCore and libswiftSwiftOnoneSupport). No receipt
     * owns it; it is an unreceipted development leftover. See
     * tests/mkimplausible.c for what a sweep of this host actually shows.
     *
     * mg_funcstarts_decode's contract (above): -1 ONLY when mu_decode fails,
     * i.e. a malformed ULEB. Any n >= 0 means the blob decoded. */
    int ns = mg_funcstarts_decode(buf + fsoff, fssize, base, starts, (int)fssize);
    uint32_t n = 0;
    int rc = 0;
    if (ns < 0) {
        /* the blob would not decode: a ULEB128 entry runs off its end. */
        fprintf(stderr, "macho_grow: implausible -- the %u-byte LC_FUNCTION_STARTS blob at "
                        "offset %u does not decode: a ULEB128 entry runs off its end\n",
                fssize, fsoff);
        rc = -1;
    } else if (ns == 0) {
        /* The blob decoded and declares no function starts -- the terminator
         * is the first thing in it. That is the same fact about the image as
         * having no LC_FUNCTION_STARTS at all, which the `!fsoff || !fssize`
         * line above answers with "nothing to check against". Same
         * fact, same answer: accept. rc stays 0 and the loop is skipped. */
    } else if (mg_collect(buf, fsize, addr, kinds, MG_SNAP_MAX, &n) != 0) {
        /* collection itself failed -- a malformed or overflowing structure
         * among the base-relative entries, or more of them than MG_SNAP_MAX.
         * Nothing was compared, so this is not a verdict about the offsets. */
        fprintf(stderr, "macho_grow: implausible -- the base-relative entries (initializers, "
                        "export-trie, data-in-code and unwind starts) could not be "
                        "collected, so nothing could be checked against "
                        "LC_FUNCTION_STARTS\n");
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

struct mg_fs_find_ctx { uint32_t dataoff, datasize; };

/* mg_grow_header's mi_each_lc callback: find the FIRST LC_FUNCTION_STARTS and
 * stop (`break` in the original loop -- first match wins, unlike
 * mg_plausible's deliberately-preserved last-match-wins). */
static int mg_fs_find_cb(const struct load_command *lc, void *ctx_) {
    struct mg_fs_find_ctx *ctx = (struct mg_fs_find_ctx *)ctx_;
    if (lc->cmd != LC_FUNCTION_STARTS) return 0;
    const struct linkedit_data_command *ld = (const struct linkedit_data_command *)lc;
    ctx->dataoff = ld->dataoff; ctx->datasize = ld->datasize;
    return 1;
}

struct mg_patch_ctx {
    uint32_t insert;
    uint32_t grow;
    int error;
};

/* mg_grow_header's mi_each_lc callback for the header-patch pass: edits
 * LC_SEGMENT_64's own fileoff/vmaddr/vmsize/filesize (and every section's
 * offset/reloff via ml_bump) plus LC_MAIN's entryoff -- never lc->cmd,
 * lc->cmdsize, or hdr->ncmds, so it stays inside mi_each_lc's mutation
 * contract. Returns non-zero to stop on the first refusal (an entryoff or
 * section offset/reloff that would overflow); ctx->error carries that so the
 * caller does ONE free(mg_new_trie)+mg_snapshot_free+return, not one copy per
 * refusal site the way the hand-rolled loop needed. */
static int mg_patch_cb(const struct load_command *lc_in, void *ctx_) {
    struct mg_patch_ctx *ctx = (struct mg_patch_ctx *)ctx_;
    struct load_command *lc = (struct load_command *)lc_in;
    switch (lc->cmd) {
    case LC_SEGMENT_64: {
        struct segment_command_64 *seg = (struct segment_command_64 *)lc;
        /* Identify segments by criteria, not by a saved pointer: the earlier
         * realloc may have moved the buffer, invalidating any pointer found
         * during validation. The header-bearing segment is the one mapped
         * at file offset 0 with content (i.e. __TEXT, not __PAGEZERO). */
        if (strcmp(seg->segname, "__PAGEZERO") == 0) {
            seg->vmsize -= ctx->grow;            /* donate space below __TEXT */
        } else if (seg->fileoff == 0 && seg->filesize > 0) {
            seg->vmaddr  -= ctx->grow;           /* lower the image base */
            seg->vmsize  += ctx->grow;
            seg->filesize += ctx->grow;          /* fileoff stays 0 */
        } else if (seg->fileoff >= ctx->insert) {
            seg->fileoff += ctx->grow;           /* later segment: file moves, vm fixed */
        }
        struct section_64 *sect = (struct section_64 *)(seg + 1);
        for (uint32_t j = 0; j < seg->nsects; j++) {
            /* ml_bump refuses (returns -1, prints why) rather than wrap
             * a section offset/reloff that sits within `grow` of
             * UINT32_MAX -- same guard as src/linkedit.h's table, same
             * reason: a wrapped file offset is a corrupt binary that
             * still looks plausible. */
            if (ml_bump(&sect[j].offset, ctx->insert, ctx->grow) != 0 ||   /* addr stays fixed */
                (sect[j].reloff && ml_bump(&sect[j].reloff, ctx->insert, ctx->grow) != 0)) {
                ctx->error = 1;
                return 1;
            }
        }
        break;
    }
    case LC_MAIN: {
        /* entryoff is a file offset within __TEXT; bumping it keeps the
         * entry's vm address fixed (base went down by the same amount).
         * entryoff is a uint64_t (entry_point_command), NOT uint32_t --
         * bumped and overflow-checked directly at its own width, rather
         * than through ml_bump's 32-bit-only guard, which would first
         * silently truncate any entryoff at or past 4GB before ever
         * checking anything. Real binaries never have an entryoff that
         * large (it is a file offset within __TEXT), but "refuse rather
         * than guess" means checking the real field, not an assumption
         * about its range. */
        struct entry_point_command *c = (struct entry_point_command *)lc;
        if (c->entryoff >= (uint64_t)ctx->insert) {
            if (c->entryoff > UINT64_MAX - (uint64_t)ctx->grow) {
                fprintf(stderr, "macho_grow: LC_MAIN's entryoff (%#llx) would overflow "
                                "a 64-bit field after growing by %#x; refusing rather "
                                "than wrap\n",
                        (unsigned long long)c->entryoff, ctx->grow);
                ctx->error = 1;
                return 1;
            }
            c->entryoff += ctx->grow;
        }
        break;
    }
    default:
        break;  /* everything else -- the __LINKEDIT-resident structures
                  * (LC_SYMTAB, LC_DYSYMTAB, LC_DYLD_INFO[_ONLY], and the
                  * linkedit_data_command family) plus anything carrying
                  * no file offset at all -- is ml_bump_all's job, below. */
    }
    return 0;
}

int mg_grow_header(uint8_t **pbuf, size_t *pfsize, uint32_t grow_req) {
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
        fprintf(stderr, "macho_grow: only MH_EXECUTE can be grown (filetype=%u): growing "
                        "lowers the image base into __PAGEZERO, and a dylib or bundle has "
                        "none. This tool cannot grow a dylib or bundle.\n", hdr->filetype);
        return -1;
    }
    if (!(hdr->flags & MH_PIE)) {
        fprintf(stderr, "macho_grow: executable is not PIE (flags=0x%x); lowering the "
                        "image base would require fixing absolute relocations, which "
                        "this tool does not do\n", hdr->flags);
        return -1;
    }

    uint32_t insert = mg_first_sect_off(buf, fsize);
    if (insert == UINT32_MAX) return -1;   /* already explained itself on stderr */
    if (insert == MG_NO_SECTION_DATA) {
        fprintf(stderr, "macho_grow: no section data bounds the header pad; refusing to "
                        "grow it rather than guess where it ends\n");
        return -1;
    }

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

    /* Locate the donor (__PAGEZERO) and confirm a header-bearing segment
     * (__TEXT) exists, via the same finders every other converted walk in
     * this toolkit uses instead of a third hand-rolled copy of the search.
     * Documented gap from a review round: mi_find_segment returns the FIRST
     * "__PAGEZERO"-named segment; the original hand-rolled loop had no
     * `break` on a pagezero match, so it kept the LAST. mi_text_base (used
     * just below for `text`) has the identical first-vs-last change --
     * matches unconditionally return on the first hit, where the original
     * loop's `else if` also had no `break`. Neither is exercised by any
     * fixture (none carries more than one __PAGEZERO or more than one
     * fileoff==0-with-content segment); the commit that made this
     * conversion documented neither at the time. Recorded here now. */
    mi_image find_im;
    if (mi_wrap(buf, fsize, &find_im) != 0) {
        fprintf(stderr, "macho_grow: internal error -- the header no longer validates\n");
        return -1;
    }
    struct segment_command_64 *pagezero = mi_find_segment(&find_im, "__PAGEZERO");
    if (!mi_text_base(&find_im)) {
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
        struct mg_fs_find_ctx fctx = { 0, 0 };
        /* find_im (above) still validly wraps this same buf/fsize -- neither
         * has changed since -- so it is reused rather than re-wrapped. */
        mi_each_lc(&find_im, mg_fs_find_cb, &fctx);
        fs_dataoff = fctx.dataoff; fs_datasize = fctx.datasize;
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
            if (!mg_find_trie(buf, fsize, &toff, &tsize) || !toff || !tsize) {
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
     * touch them; mg_patch_cb (below) adjusts only file-offset fields, plus the
     * three VM fields that keep every address fixed -- never lc->cmd,
     * lc->cmdsize, or hdr->ncmds, so this stays inside mi_each_lc's mutation
     * contract despite editing several fields in place per command. */
    {
        mi_image patch_im;
        if (mi_wrap(buf, final_size, &patch_im) != 0) {
            fprintf(stderr, "macho_grow: internal error -- the header we just moved no "
                            "longer validates\n");
            free(mg_new_trie);
            mg_snapshot_free(&snap);
            return -1;
        }
        struct mg_patch_ctx pctx = { insert, grow, 0 };
        mi_each_lc(&patch_im, mg_patch_cb, &pctx);
        if (pctx.error) {
            /* mg_patch_cb already printed why (LC_MAIN's entryoff would
             * overflow, or ml_bump refused a section offset/reloff). Fields on
             * commands visited before the one that failed are already patched
             * in place -- not rolled back, same as every other internal
             * failure path in this function: the caller must discard this
             * buffer, never write it out. */
            free(mg_new_trie);
            mg_snapshot_free(&snap);
            return -1;
        }
    }

    /* The __LINKEDIT offset-bump table (src/linkedit.h): symtab, strtab,
     * indirect symbols, dyld-info streams, function starts, data-in-code,
     * code signature and siblings. A second pass over the same load-command
     * chain the loop above just walked -- disjoint switch cases, so running
     * them in either order or in one merged switch produces identical bytes.
     * Re-wrapped rather than reusing a stale mi_image: buf/hdr above may be
     * the realloc'd pointer from the __LINKEDIT-grow path earlier in this
     * function, and cmdsize/ncmds/nsects are exactly what the loop just
     * walked without changing, so this wrap can only re-confirm what is
     * already true. */
    {
        mi_image im;
        if (mi_wrap(buf, final_size, &im) != 0) {
            fprintf(stderr, "macho_grow: internal error -- the header we just patched "
                            "no longer validates\n");
            /* mg_new_trie is still live here when mg_trie_needs_rebuild --
             * it is not freed until the trie-rebasing block below runs.
             * Every other post-realloc failure path in this function frees
             * it; this one must too. free(NULL) is a no-op when it wasn't
             * allocated. */
            free(mg_new_trie);
            mg_snapshot_free(&snap);
            return -1;
        }
        if (ml_bump_all(&im, insert, grow) != 0) {
            /* ml_bump/ml_bump_all already printed why (an offset would
             * overflow a 32-bit field); nothing more to add. Some fields on
             * commands walked before the one that overflowed are already
             * bumped in place -- not rolled back, same as every other
             * internal failure path here: the caller must discard this
             * buffer rather than write it out. */
            free(mg_new_trie);
            mg_snapshot_free(&snap);
            return -1;
        }
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
        if (!mg_find_trie(buf, final_size, &toff, &tsize)) {
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
            long linkedit_lc_off = -1;
            {
                /* mi_find_segment already IS "first (and only well-formed)
                 * match" -- same finder every other converted walk in this
                 * toolkit uses. Its offset (not a pointer: the realloc just
                 * below can move buf) is what this path actually needs. */
                mi_image le_im;
                if (mi_wrap(buf, final_size, &le_im) == 0) {
                    struct segment_command_64 *le = mi_find_segment(&le_im, "__LINKEDIT");
                    if (le) linkedit_lc_off = (uint8_t *)le - buf;
                }
            }
            /* The export LC is looked up through mg_find_trie_lc, the same
             * function mg_find_trie used just above to locate toff/tsize --
             * not a second hand-rolled scan that could disagree with it
             * about which load command "the" export trie means. */
            long export_lc_off = -1; uint32_t export_lc_cmd = 0;
            mg_find_trie_lc(buf, final_size, &export_lc_off, &export_lc_cmd);
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

