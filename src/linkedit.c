/* linkedit.c — see linkedit.h for the contract. */

#include "linkedit.h"

#include "mach_compat.h"

void ml_bump(uint32_t *off, uint32_t insert, uint32_t grow) {
    if (*off >= insert) *off += grow;
}

struct ml_bump_ctx {
    uint32_t insert;
    uint32_t grow;
};

/* mi_each_lc callback: bump the __LINKEDIT-resident offset field(s) of one
 * load command. Verbatim (mechanically re-shaped) copy of macho_grow.h's
 * former switch cases for these same commands. Always returns 0 -- every
 * command here is independent, so there is nothing to refuse partway
 * through and no reason to stop the walk early. */
static int ml_bump_lc(const struct load_command *lc, void *vctx) {
    struct ml_bump_ctx *ctx = (struct ml_bump_ctx *)vctx;
    /* Cast away const to write through the command's own fields: permitted
     * by mi_each_lc's contract (see image.h) for anything except
     * cmd/cmdsize/hdr->ncmds, none of which any case below touches. */
    struct load_command *m = (struct load_command *)lc;

    switch (m->cmd) {
    case LC_SYMTAB: {
        struct symtab_command *c = (struct symtab_command *)m;
        ml_bump(&c->symoff, ctx->insert, ctx->grow);
        ml_bump(&c->stroff, ctx->insert, ctx->grow);
        break;
    }
    case LC_DYSYMTAB: {
        struct dysymtab_command *c = (struct dysymtab_command *)m;
        ml_bump(&c->tocoff, ctx->insert, ctx->grow);
        ml_bump(&c->modtaboff, ctx->insert, ctx->grow);
        ml_bump(&c->extrefsymoff, ctx->insert, ctx->grow);
        ml_bump(&c->indirectsymoff, ctx->insert, ctx->grow);
        ml_bump(&c->extreloff, ctx->insert, ctx->grow);
        ml_bump(&c->locreloff, ctx->insert, ctx->grow);
        break;
    }
    case LC_DYLD_INFO:
    case LC_DYLD_INFO_ONLY: {
        struct dyld_info_command *c = (struct dyld_info_command *)m;
        ml_bump(&c->rebase_off, ctx->insert, ctx->grow);
        ml_bump(&c->bind_off, ctx->insert, ctx->grow);
        ml_bump(&c->weak_bind_off, ctx->insert, ctx->grow);
        ml_bump(&c->lazy_bind_off, ctx->insert, ctx->grow);
        ml_bump(&c->export_off, ctx->insert, ctx->grow);
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
        ml_bump(&c->dataoff, ctx->insert, ctx->grow);
        break;
    }
    default:
        break;  /* not a field this table knows about (segments and
                  * LC_MAIN stay macho_grow.h's own job; see linkedit.h) */
    }
    return 0;
}

void ml_bump_all(mi_image *im, uint32_t insert, uint32_t grow) {
    struct ml_bump_ctx ctx = { insert, grow };
    mi_each_lc(im, ml_bump_lc, &ctx);
}
