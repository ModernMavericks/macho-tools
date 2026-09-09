/* linkedit.c — see linkedit.h for the contract. */

#include "linkedit.h"

#include <stdint.h>
#include <stdio.h>

#include "mach_compat.h"

int ml_bump(uint32_t *off, uint32_t insert, uint32_t grow) {
    if (*off < insert) return 0;
    if (*off > UINT32_MAX - grow) {
        fprintf(stderr, "macho_grow: a __LINKEDIT file offset (%#x) would overflow a "
                        "32-bit field after growing by %#x; refusing rather than wrap\n",
                *off, grow);
        return -1;
    }
    *off += grow;
    return 0;
}

struct ml_bump_ctx {
    uint32_t insert;
    uint32_t grow;
};

/* mi_each_lc callback: bump the __LINKEDIT-resident offset field(s) of one
 * load command. Returns 0 to keep walking, or 1 to stop the walk the
 * instant any ml_bump call refuses (overflow) -- once one field cannot be
 * trusted, there is no reason to keep patching the rest of this command's
 * fields into what will be a discarded, refused buffer anyway. */
static int ml_bump_lc(const struct load_command *lc, void *vctx) {
    struct ml_bump_ctx *ctx = (struct ml_bump_ctx *)vctx;
    /* Cast away const to write through the command's own fields: permitted
     * by mi_each_lc's contract (see image.h) for anything except
     * cmd/cmdsize/hdr->ncmds, none of which any case below touches. */
    struct load_command *m = (struct load_command *)lc;
    int r = 0;

    switch (m->cmd) {
    case LC_SYMTAB: {
        struct symtab_command *c = (struct symtab_command *)m;
        r |= ml_bump(&c->symoff, ctx->insert, ctx->grow);
        r |= ml_bump(&c->stroff, ctx->insert, ctx->grow);
        break;
    }
    case LC_DYSYMTAB: {
        struct dysymtab_command *c = (struct dysymtab_command *)m;
        r |= ml_bump(&c->tocoff, ctx->insert, ctx->grow);
        r |= ml_bump(&c->modtaboff, ctx->insert, ctx->grow);
        r |= ml_bump(&c->extrefsymoff, ctx->insert, ctx->grow);
        r |= ml_bump(&c->indirectsymoff, ctx->insert, ctx->grow);
        r |= ml_bump(&c->extreloff, ctx->insert, ctx->grow);
        r |= ml_bump(&c->locreloff, ctx->insert, ctx->grow);
        break;
    }
    case LC_DYLD_INFO:
    case LC_DYLD_INFO_ONLY: {
        struct dyld_info_command *c = (struct dyld_info_command *)m;
        r |= ml_bump(&c->rebase_off, ctx->insert, ctx->grow);
        r |= ml_bump(&c->bind_off, ctx->insert, ctx->grow);
        r |= ml_bump(&c->weak_bind_off, ctx->insert, ctx->grow);
        r |= ml_bump(&c->lazy_bind_off, ctx->insert, ctx->grow);
        r |= ml_bump(&c->export_off, ctx->insert, ctx->grow);
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
        struct linkedit_data_command *c = (struct linkedit_data_command *)m;
        r |= ml_bump(&c->dataoff, ctx->insert, ctx->grow);
        break;
    }
    case LC_TWOLEVEL_HINTS: {
        struct twolevel_hints_command *c = (struct twolevel_hints_command *)m;
        r |= ml_bump(&c->offset, ctx->insert, ctx->grow);
        break;
    }
    case LC_ENCRYPTION_INFO: {
        struct encryption_info_command *c = (struct encryption_info_command *)m;
        r |= ml_bump(&c->cryptoff, ctx->insert, ctx->grow);
        break;
    }
    case LC_ENCRYPTION_INFO_64: {
        struct encryption_info_command_64 *c = (struct encryption_info_command_64 *)m;
        r |= ml_bump(&c->cryptoff, ctx->insert, ctx->grow);
        break;
    }
    default:
        break;  /* not a field this table knows about (segments and
                  * LC_MAIN stay macho_grow.h's own job; LC_NOTE and
                  * LC_ATOM_INFO are refused before this ever runs -- see
                  * linkedit.h) */
    }
    return r != 0;   /* mi_each_lc: non-zero stops the walk */
}

int ml_bump_all(mi_image *im, uint32_t insert, uint32_t grow) {
    struct ml_bump_ctx ctx = { insert, grow };
    return mi_each_lc(im, ml_bump_lc, &ctx) ? 0 : -1;
}
