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

/* One function per member of linkedit.h's ML_PLAIN_OFFSET_LCS, named
 * ml_bump_<cmd> by the SAME macro that names it in grow.c's accept bucket
 * (see linkedit.h's own comment on the macro for why). Forward-declared
 * here via the macro too: add a member to ML_PLAIN_OFFSET_LCS without
 * defining its ml_bump_<cmd> body below and the build fails at link time
 * (undefined symbol) -- not silently, and not merely a test someone could
 * forget to run. That is what makes this coupling real rather than
 * advisory: grow.c's accept-bucket case labels for this group (see
 * mg_classify_cb) are generated from this same list, so a load command
 * cannot join grow.c's "safe, plain offset" bucket without a matching,
 * present ml_bump_<cmd> definition existing right here. */
#define ML_DECLARE(cmd) static int ml_bump_##cmd(struct load_command *m, struct ml_bump_ctx *ctx);
ML_PLAIN_OFFSET_LCS(ML_DECLARE)
#undef ML_DECLARE

static int ml_bump_LC_SYMTAB(struct load_command *m, struct ml_bump_ctx *ctx) {
    struct symtab_command *c = (struct symtab_command *)m;
    int r = 0;
    r |= ml_bump(&c->symoff, ctx->insert, ctx->grow);
    r |= ml_bump(&c->stroff, ctx->insert, ctx->grow);
    return r;
}

static int ml_bump_LC_DYSYMTAB(struct load_command *m, struct ml_bump_ctx *ctx) {
    struct dysymtab_command *c = (struct dysymtab_command *)m;
    int r = 0;
    r |= ml_bump(&c->tocoff, ctx->insert, ctx->grow);
    r |= ml_bump(&c->modtaboff, ctx->insert, ctx->grow);
    r |= ml_bump(&c->extrefsymoff, ctx->insert, ctx->grow);
    r |= ml_bump(&c->indirectsymoff, ctx->insert, ctx->grow);
    r |= ml_bump(&c->extreloff, ctx->insert, ctx->grow);
    r |= ml_bump(&c->locreloff, ctx->insert, ctx->grow);
    return r;
}

static int ml_bump_LC_CODE_SIGNATURE(struct load_command *m, struct ml_bump_ctx *ctx) {
    struct linkedit_data_command *c = (struct linkedit_data_command *)m;
    return ml_bump(&c->dataoff, ctx->insert, ctx->grow);
}

static int ml_bump_LC_DYLIB_CODE_SIGN_DRS(struct load_command *m, struct ml_bump_ctx *ctx) {
    struct linkedit_data_command *c = (struct linkedit_data_command *)m;
    return ml_bump(&c->dataoff, ctx->insert, ctx->grow);
}

static int ml_bump_LC_TWOLEVEL_HINTS(struct load_command *m, struct ml_bump_ctx *ctx) {
    struct twolevel_hints_command *c = (struct twolevel_hints_command *)m;
    return ml_bump(&c->offset, ctx->insert, ctx->grow);
}

static int ml_bump_LC_ENCRYPTION_INFO(struct load_command *m, struct ml_bump_ctx *ctx) {
    struct encryption_info_command *c = (struct encryption_info_command *)m;
    return ml_bump(&c->cryptoff, ctx->insert, ctx->grow);
}

static int ml_bump_LC_ENCRYPTION_INFO_64(struct load_command *m, struct ml_bump_ctx *ctx) {
    struct encryption_info_command_64 *c = (struct encryption_info_command_64 *)m;
    return ml_bump(&c->cryptoff, ctx->insert, ctx->grow);
}

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
    /* Case labels generated from linkedit.h's ML_PLAIN_OFFSET_LCS, dispatch
     * to the like-named ml_bump_<cmd> functions defined above -- see that
     * macro's own comment for what this couples and what it deliberately
     * does not. */
#define ML_CASE(cmd) case cmd: r |= ml_bump_##cmd(m, ctx); break;
    ML_PLAIN_OFFSET_LCS(ML_CASE)
#undef ML_CASE
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
    case LC_SEGMENT_SPLIT_INFO:
    case LC_LINKER_OPTIMIZATION_HINT:
    case LC_DYLD_EXPORTS_TRIE:
    case LC_DYLD_CHAINED_FIXUPS: {
        struct linkedit_data_command *c = (struct linkedit_data_command *)m;
        r |= ml_bump(&c->dataoff, ctx->insert, ctx->grow);
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
