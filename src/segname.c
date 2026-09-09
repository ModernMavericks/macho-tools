/*
 * mseg_ -- see segname.h. This is compat/rename_segment.c's former
 * rs_rename_lc, unchanged in what it does to a load command; only the driver
 * (open, walk, write back, decide an exit code) stayed behind in that tool.
 */
#include <string.h>
#include <stdint.h>
#include <mach-o/loader.h>

#include "segname.h"
#include "image.h"

int mseg_name_fits(const char *name) {
    return strlen(name) <= MSEG_NAME_MAX;
}

int mseg_rename_lc(struct load_command *lc, const char *oldname, const char *newname) {
    if (lc->cmd != LC_SEGMENT_64) return 0;
    struct segment_command_64 *seg = (struct segment_command_64 *)lc;
    if (strncmp(seg->segname, oldname, MSEG_NAME_MAX) != 0) return 0;

    /* memset-then-strncpy, not strncpy alone: a name shorter than the one it
     * replaces must not leave the old name's tail bytes behind, and a name of
     * exactly MSEG_NAME_MAX bytes must fill the field with no terminator.
     * strncpy gives both -- it pads with NULs and truncates at the field
     * width -- and the memset makes that explicit rather than incidental. */
    memset(seg->segname, 0, MSEG_NAME_MAX);
    strncpy(seg->segname, newname, MSEG_NAME_MAX);
    /* Each section repeats its segment's name; getsectiondata matches on the
     * section's copy, so it has to change too. */
    struct section_64 *sects = (struct section_64 *)(seg + 1);
    for (uint32_t s = 0; s < seg->nsects; s++) {
        memset(sects[s].segname, 0, MSEG_NAME_MAX);
        strncpy(sects[s].segname, newname, MSEG_NAME_MAX);
    }
    return 1;
}

struct mseg_ctx {
    const char *oldname;
    const char *newname;
    int renamed;
};

static int mseg_rename_cb(const struct load_command *lc, void *ctx_) {
    struct mseg_ctx *ctx = ctx_;
    /* Casting away const to write through `lc` is exactly what image.h's
     * mi_each_lc comment sanctions (it names this very rename as the
     * example), and mseg_rename_lc touches neither cmd nor cmdsize. */
    ctx->renamed += mseg_rename_lc((struct load_command *)lc, ctx->oldname, ctx->newname);
    return 0;   /* renames every match; never needs to stop early */
}

int mseg_rename_image(const mi_image *im, const char *oldname, const char *newname) {
    struct mseg_ctx ctx = { oldname, newname, 0 };
    mi_each_lc(im, mseg_rename_cb, &ctx);
    return ctx.renamed;
}
