/*
 * mr_ -- the dylib/rpath/load-command rewriter, shared by cli/machotool.c, by
 * src/edit.c's edit scripts (through mr_apply_image), and by the old
 * change_dylib grammar that reaches it through compat/change_dylib.sh.
 * See rewrite.h for the operation set and why this is a library function
 * rather than one tool's main().
 *
 * A rewrite here does three things, in this order, and refuses before the
 * first byte reaches disk if any of them cannot be done:
 *
 *   1. build a brand-new load-command table (mr_build_lcs), sized to fit in
 *      the header pad between the last load command and the first section's
 *      file data -- or, with allow_grow, after mg_grow_header has enlarged
 *      that pad (src/grow.h; only a PIE executable can be grown at all);
 *   2. renumber every library ordinal an insert or a delete shifted
 *      (src/ordinals.h), cross-checking the map against the table actually
 *      emitted rather than against its own arithmetic;
 *   3. re-run mg_plausible over the finished image, because this rewriter is
 *      the last stage of the wrapper's chain (patch_macho -> add_version_min
 *      -> change_dylib) and so is the last chance to catch a cumulative
 *      mistake before a single byte is written.
 *
 * Nothing here moves a byte of file data; only the load commands are
 * rewritten, and only within the header pad. The one exception is -grow,
 * which is mg_grow_header's own reviewed exception, not a new one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>       /* offsetof, for the mr_ops layout tripwire below */
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>

#include "rewrite.h"
#include "image.h"
#include "segname.h"
#include "grow.h"
#include "ordinals.h"
#include "fat.h"
#include "atomic_write.h"
#include "mach_compat.h"
#include "lc_kinds.h"

/* mo_map_build's is_deleted callback: true if `name` matches a deletion in
 * `ops`. This is the SAME test mr_build_lcs uses (via `matched`/`new_path ==
 * NULL`) to decide which LC_LOAD_DYLIB commands to drop, which is what keeps
 * the load-command rewrite and the ordinal map from being able to disagree
 * about which dylib went away. The context is the mr_ops itself, so there is
 * nothing for the two to disagree ABOUT. */
static int mr_is_deleted(const char *name, void *ctx_) {
    const mr_ops *ops = ctx_;
    for (int c = 0; c < ops->n_dylib_changes; c++)
        if (ops->dylib_changes[c].new_path == NULL &&
            strcmp(name, ops->dylib_changes[c].old_path) == 0)
            return 1;
    return 0;
}

/* Emit one LC_LOAD_DYLIB naming `path` at `dst`; returns its cmdsize. */
static uint32_t mr_emit_dylib_lc(uint8_t *dst, const char *path) {
    size_t plen = strlen(path) + 1;
    uint32_t cs = (uint32_t)((sizeof(struct dylib_command) + plen + 7) & ~7UL);
    struct dylib_command *ndc = (struct dylib_command *)dst;
    memset(ndc, 0, cs);
    ndc->cmd = LC_LOAD_DYLIB;
    ndc->cmdsize = cs;
    ndc->dylib.name.offset = sizeof(struct dylib_command);
    ndc->dylib.timestamp = 2;            /* conventional (matches install_name_tool) */
    ndc->dylib.current_version = 0;
    ndc->dylib.compatibility_version = 0;
    strcpy((char *)ndc + sizeof(struct dylib_command), path);
    return cs;
}

/* Emit one LC_RPATH naming `path` at `dst`; returns its cmdsize. An
 * rpath_command followed by the NUL-terminated path, padded to 8 bytes --
 * the same shape -insert and -append both need, which is why it is a
 * function rather than being written out at each of the two sites. */
static uint32_t mr_emit_rpath_lc(uint8_t *dst, const char *path) {
    size_t plen = strlen(path) + 1;
    uint32_t cs = (uint32_t)((sizeof(struct rpath_command) + plen + 7) & ~7UL);
    struct rpath_command *nrc = (struct rpath_command *)dst;
    memset(nrc, 0, cs);
    nrc->cmd = LC_RPATH;
    nrc->cmdsize = cs;
    nrc->path.offset = sizeof(struct rpath_command);
    strcpy((char *)nrc + sizeof(struct rpath_command), path);
    return cs;
}

/* mr_build_lcs's per-load-command work, as an mi_each_lc callback. Every
 * mutable local the old hand-rolled loop threaded through each iteration
 * (new_off, ncmds, mods, placed_inserts) lives in this ctx instead; the
 * appends are reached through `ops` but not used here, because mr_build_lcs
 * only needs them AFTER the walk (appended past the existing table), so they
 * never had to be part of the per-command state.
 *
 * Returns 0 to continue, 1 to stop the walk -- the two cases below where a
 * dylib or LC_RPATH command's own name offset is out of bounds for its
 * cmdsize (mo_lc_str_at, ordinals.h). Stopping here is exactly what the
 * stop-capable mi_each_lc exists for: without it, the walk would keep
 * calling this callback for every later load command after the refusal
 * fires, each one still writing into new_lcs -- corrupting/overrunning a
 * buffer mr_build_lcs's caller believes was never touched, instead of leaving
 * the input provably unmodified the way a refusal here must. */
struct mr_build_lcs_ctx {
    const mr_ops *ops;
    uint8_t *new_lcs;
    uint32_t new_off;
    uint32_t ncmds;
    int mods;
    int renames;      /* how many LC_SEGMENT_64s the rename actually matched */
    int placed_inserts;
    int placed_rpath_inserts;
    int verbose;
    /* Per-operation hit counts, or NULL to not count (the sizing pass --
     * see mr_build_lcs's own comment). Caller-owned and caller-zeroed,
     * threaded down from mr_apply_file through mr_build_lcs so it can name,
     * after every slice has run, the operations that matched nothing. */
    int *hit_dylib;
    int *hit_rpath;
    int *hit_strip;
};

static int mr_build_lcs_lc(const struct load_command *lc, void *ctx_) {
    struct mr_build_lcs_ctx *ctx = ctx_;
    uint32_t cmdsize = lc->cmdsize;
    uint32_t write_size = cmdsize;
    int matched = -1;
    int deleted = 0;

    /* Dropping a command reclaims its bytes for the rest of the table.
     * Any __LINKEDIT payload it referenced simply stops being reachable;
     * nothing moves, so no offset anywhere needs fixing up. */
    /* No `break`: a load command of this kind can be named by more than one
     * -strip-lc argument (a duplicate, e.g. two "-strip-lc uuid"), and every
     * one of them DID find a match here, not just the first. Stopping at the
     * first match would credit that argument's hit array slot and leave
     * every later duplicate's slot at zero -- reported as "no load command
     * of kind uuid to delete" about an image that HAD one, which this
     * function just dropped. `stripped` only needs to become true once. */
    int stripped = 0;
    for (int s = 0; s < ctx->ops->n_strip_cmds; s++)
        if (lc->cmd == ctx->ops->strip_cmds[s]) {
            stripped = 1;
            if (ctx->hit_strip) ctx->hit_strip[s]++;
        }
    if (stripped) {
        if (ctx->verbose) printf("  Strip [%u bytes]: load command 0x%x\n", cmdsize, lc->cmd);
        ctx->ncmds--;
        ctx->mods++;
        return 0;
    }

    /* -insert goes immediately before the first ordinal-bearing dylib LC, so
     * the inserted libraries become ordinals 1..n and load (and initialize)
     * ahead of everything the image already depended on. Nothing strippable
     * bears an ordinal, so the strip pass above cannot move this boundary. */
    if (!ctx->placed_inserts && ctx->ops->n_dylib_inserts && mo_is_ordinal_lc(lc->cmd)) {
        for (int s = 0; s < ctx->ops->n_dylib_inserts; s++) {
            uint32_t cs = mr_emit_dylib_lc(ctx->new_lcs + ctx->new_off, ctx->ops->dylib_inserts[s]);
            ctx->new_off += cs;
            ctx->ncmds++;
            ctx->mods++;
            if (ctx->verbose) printf("  Insert [%u bytes]: LC_LOAD_DYLIB %s (now ordinal %d)\n",
                                cs, ctx->ops->dylib_inserts[s], s + 1);
        }
        ctx->placed_inserts = 1;
    }

    /* -insert for an rpath goes immediately before the first LC_RPATH the
     * image already has, so dyld -- which takes the FIRST rpath that resolves
     * -- searches the new one ahead of every existing one. That is the entire
     * point of the operation: appending can only ever put it last. Unlike a
     * dylib insert this shifts nothing, because LC_RPATH carries no library
     * ordinal (mo_is_ordinal_lc lists the four commands that do, and LC_RPATH
     * is not one), so no renumbering is needed or done.
     *
     * A -delete-rpath of that first LC_RPATH does not move this boundary: the
     * insert is emitted at the position the deleted command occupied, which is
     * still ahead of every rpath that survives. Nor does -strip-lc, for the
     * same reason it cannot move the dylib boundary above: LC_RPATH is not in
     * LC_STRIP_KINDS (src/lc_kinds.c), so the strip pass's early return can
     * never fire on the very command this placement keys off. */
    if (!ctx->placed_rpath_inserts && ctx->ops->n_rpath_inserts && lc->cmd == LC_RPATH) {
        for (int s = 0; s < ctx->ops->n_rpath_inserts; s++) {
            uint32_t cs = mr_emit_rpath_lc(ctx->new_lcs + ctx->new_off, ctx->ops->rpath_inserts[s]);
            ctx->new_off += cs;
            ctx->ncmds++;
            ctx->mods++;
            if (ctx->verbose) printf("  Insert [%u bytes]: LC_RPATH %s (now searched first)\n",
                                cs, ctx->ops->rpath_inserts[s]);
        }
        ctx->placed_rpath_inserts = 1;
    }

    /* mo_is_ordinal_lc() here (rather than a locally re-listed set) is
     * what keeps this "which dylib LCs can be matched/renamed/deleted"
     * set in sync with mo_map_build's "which dylib LCs carry an ordinal"
     * set -- they used to disagree about LC_LOAD_UPWARD_DYLIB. LC_ID_DYLIB
     * is added back in because it names the image itself: it has to be
     * recognized as dylib-shaped so `dc`/`name` below are valid, but it's
     * excluded from matching just below, same as before.
     *
     * compat/fix_macho.c's `-change` used to implement this exact same
     * question ("which dylib LCs can `-change` rewrite?") independently, and
     * it hand-listed {LOAD, WEAK, ID, REEXPORT} -- silently missing
     * LC_LOAD_UPWARD_DYLIB, so `fix_macho -change` reported "No changes
     * needed" (exit 0) on exactly the input this function rewrites. It was
     * pointed at mo_is_ordinal_lc() so it could not drift again, and has
     * since been retired outright: fix_macho is a /bin/sh wrapper and its
     * `-change` reaches THIS predicate. There is now exactly one answer to
     * the question, which is the end state that fix was aiming at.
     * tests/change_dylib_test.sh case 8b still pins it from that side. */
    if (mo_is_ordinal_lc(lc->cmd) || lc->cmd == LC_ID_DYLIB) {
        const struct dylib_command *dc = (const struct dylib_command *)lc;
        if (lc->cmd != LC_ID_DYLIB) {  /* never rewrite this dylib's own identity */
            const char *name = mo_lc_str_at(lc, dc->dylib.name.offset);
            if (!name) {
                /* UNREACHABLE through either front-end today: mr_process_thin
                 * calls mo_map_build (src/ordinals.c) on this same buffer
                 * BEFORE mr_build_lcs ever runs, and mo_map_build performs this
                 * identical mo_lc_str_at check against every ordinal-bearing
                 * dylib LC -- byte-identical message included -- so it
                 * always refuses first. Confirmed by marker-patching this
                 * format string and observing mo_map_build's message come
                 * out instead. Kept anyway: it is still the correct check
                 * for anyone calling mr_build_lcs directly (it is `static`, but
                 * nothing enforces that mr_process_thin is its only caller
                 * forever), and removing it on the assumption that
                 * mo_map_build always runs first would be exactly the kind
                 * of "two places deciding one thing, only one tested"
                 * coupling this codebase keeps getting bitten by. Don't
                 * delete it as dead, and don't trust it as covered -- the
                 * LC_RPATH refusal just below is the one change_dylib_test.sh
                 * case 19 actually exercises. */
                fprintf(stderr, "ERROR: malformed dylib load command (name offset %u "
                                "exceeds cmdsize %u); refusing\n",
                        dc->dylib.name.offset, cmdsize);
                return 1;
            }
            /* No `break`: `matched` still ends up as the FIRST entry naming
             * this path (that is what decides which change gets applied,
             * just below), but every entry naming it is counted as a hit,
             * not only the first. A -delete and a -change can legitimately
             * name the SAME old_path -- see mr_is_deleted just below, which
             * makes a -delete win over a conflicting -change regardless of
             * argument order -- so `machotool dylib f -replace X N -delete X`
             * has two dylib_changes entries sharing old_path X. Breaking
             * here would count only the -replace's entry as a hit and
             * report the -delete's entry as "matched nothing", which is
             * false: it is the operation that removed the load command.
             * n_dylib_changes is capped at MR_MAX_OPS (rewrite.h), so
             * scanning the whole array unconditionally costs nothing
             * observable. */
            for (int c = 0; c < ctx->ops->n_dylib_changes; c++)
                if (strcmp(name, ctx->ops->dylib_changes[c].old_path) == 0) {
                    if (ctx->hit_dylib) ctx->hit_dylib[c]++;
                    if (matched < 0) matched = c;
                }
            /* Deletion is decided by mr_is_deleted -- the SAME predicate
             * passed to mo_map_build below -- not by whichever `changes`
             * entry happens to match first. Without this, a path named
             * in both a -change and a -delete could be kept here while
             * mo_map_build's map (which scans every -delete, not just the
             * first match) marks it gone: exactly the "renumberer and
             * emitter disagree" bug this module exists to rule out. A
             * -delete anywhere in the arguments now always wins, no
             * matter where it falls relative to a conflicting -change. */
            deleted = mr_is_deleted(name, (void *)ctx->ops);
        }
        if (!deleted && matched >= 0 && ctx->ops->dylib_changes[matched].new_path != NULL) {
            size_t base = dc->dylib.name.offset;
            size_t new_len = strlen(ctx->ops->dylib_changes[matched].new_path) + 1;
            uint32_t needed = (uint32_t)((base + new_len + 7) & ~7UL);
            if (needed < cmdsize) needed = cmdsize;
            write_size = needed;
        }
    }

    /* LC_RPATH carries a single lc_str exactly like a dylib command, so
     * the same grow-the-command-and-rewrite-in-place logic applies. */
    int rmatched = -1;
    if (lc->cmd == LC_RPATH) {
        const struct rpath_command *rc = (const struct rpath_command *)lc;
        const char *rp = mo_lc_str_at(lc, rc->path.offset);
        if (!rp) {
            fprintf(stderr, "ERROR: malformed LC_RPATH command (path offset %u "
                            "exceeds cmdsize %u); refusing\n",
                    rc->path.offset, cmdsize);
            return 1;
        }
        for (int c = 0; c < ctx->ops->n_rpath_changes; c++)
            if (strcmp(rp, ctx->ops->rpath_changes[c].old_path) == 0) {
                rmatched = c;
                if (ctx->hit_rpath) ctx->hit_rpath[c]++;
                break;
            }
        if (rmatched >= 0 && ctx->ops->rpath_changes[rmatched].new_path != NULL) {
            size_t base = rc->path.offset;
            size_t new_len = strlen(ctx->ops->rpath_changes[rmatched].new_path) + 1;
            uint32_t needed = (uint32_t)((base + new_len + 7) & ~7UL);
            if (needed < cmdsize) needed = cmdsize;
            write_size = needed;
        }
    }

    if (rmatched >= 0) {
        if (ctx->ops->rpath_changes[rmatched].new_path == NULL) {
            if (ctx->verbose) printf("  Delete rpath [%u bytes]: %s\n", cmdsize,
                                ctx->ops->rpath_changes[rmatched].old_path);
            ctx->ncmds--;
        } else {
            memcpy(ctx->new_lcs + ctx->new_off, lc, cmdsize);
            struct rpath_command *nrc = (struct rpath_command *)(ctx->new_lcs + ctx->new_off);
            nrc->cmdsize = write_size;
            size_t base = nrc->path.offset;
            memset(ctx->new_lcs + ctx->new_off + base, 0, write_size - base);
            strcpy((char *)(ctx->new_lcs + ctx->new_off + base), ctx->ops->rpath_changes[rmatched].new_path);
            if (ctx->verbose)
                printf("  Change rpath [%u->%u bytes]: %s -> %s\n", cmdsize, write_size,
                       ctx->ops->rpath_changes[rmatched].old_path, ctx->ops->rpath_changes[rmatched].new_path);
            ctx->new_off += write_size;
        }
        ctx->mods++;
    } else if (deleted) {
        if (ctx->verbose) printf("  Delete [%u bytes]: %s\n", cmdsize, ctx->ops->dylib_changes[matched].old_path);
        ctx->ncmds--;
        ctx->mods++;
    } else {
        memcpy(ctx->new_lcs + ctx->new_off, lc, cmdsize);
        /* A segment rename edits segname/sectname CONTENT only -- never cmd,
         * never cmdsize -- so it can be applied to the command already copied
         * into the new table, after the copy, without changing this command's
         * size or the table's shape. mseg_rename_lc (src/segname.h) is the
         * one function every segment rename in this repo goes through, so no
         * two front-ends can disagree about what a rename is; only getting
         * here through mr_apply_file is what additionally gives this one fat
         * containers and an atomically written output. */
        /* BOTH pointers, matching rewrite.h's "Both NULL means no rename was
         * requested": a half-filled pair would otherwise reach
         * mseg_rename_lc's strncpy with a NULL source. No caller sets one
         * without the other today (cmd_segment sets both), which is exactly
         * why the guard has to say what the contract says rather than what
         * today's only caller happens to do. */
        if (ctx->ops->segment_rename_old && ctx->ops->segment_rename_new &&
            mseg_rename_lc((struct load_command *)(ctx->new_lcs + ctx->new_off),
                           ctx->ops->segment_rename_old, ctx->ops->segment_rename_new)) {
            if (ctx->verbose)
                printf("  Rename segment: %s -> %s\n",
                       ctx->ops->segment_rename_old, ctx->ops->segment_rename_new);
            ctx->mods++;
            ctx->renames++;
        }
        if (matched >= 0) {
            struct dylib_command *ndc = (struct dylib_command *)(ctx->new_lcs + ctx->new_off);
            ndc->cmdsize = write_size;
            if (ctx->ops->dylib_changes[matched].reexport) {
                ndc->cmd = LC_REEXPORT_DYLIB;
                if (ctx->verbose) printf("  Reexport: %s\n", ctx->ops->dylib_changes[matched].old_path);
            }
            if (ctx->ops->dylib_changes[matched].new_path[0] != '\0') {
                size_t base = ndc->dylib.name.offset;
                memset(ctx->new_lcs + ctx->new_off + base, 0, write_size - base);
                strcpy((char *)(ctx->new_lcs + ctx->new_off + base), ctx->ops->dylib_changes[matched].new_path);
                if (ctx->verbose)
                    printf("  Change [%u->%u bytes]: %s -> %s\n", cmdsize, write_size,
                           ctx->ops->dylib_changes[matched].old_path, ctx->ops->dylib_changes[matched].new_path);
            }
            ctx->mods++;
        }
        ctx->new_off += write_size;
    }
    return 0;
}

/*
 * Build the new load-command table into `new_lcs` from the current header
 * `im` wraps. Returns the new total size (sizeofcmds) via *out_off, the new
 * command count via *out_ncmds, and how many changes applied via *out_mods.
 * Does NOT mutate `im`'s buffer, so it is safe to call more than once (e.g.
 * again after the header pad has been grown, against a freshly mi_wrap'd `im`
 * over the relocated buffer -- see mr_process_thin's second call site). `verbose`
 * prints the per-change diagnostics once.
 *
 * Returns 0 on success, -1 (message already on stderr, via mr_build_lcs_lc) if a
 * dylib or LC_RPATH command's name offset is out of bounds for its own
 * cmdsize -- see mo_lc_str_at (ordinals.h). out_off/out_ncmds/out_mods are
 * unspecified on failure; the caller must not use them. The per-command work
 * is mr_build_lcs_lc, walked via the stop-capable mi_each_lc so that refusal
 * can abort before writing another byte into new_lcs; everything below
 * (placing not-yet-placed inserts, then -add/-add-rpath) runs only once, in
 * whatever state the walk left ncmds/new_off/mods, so it stays hand-written
 * here rather than folded into the per-command callback.
 *
 * hit_dylib/hit_rpath/hit_strip: per-operation hit counts, so mr_apply_file
 * can name the operations that matched nothing. Caller-owned and
 * caller-zeroed, sized MR_MAX_OPS / MR_MAX_STRIP -- the same bound mr_ops's
 * own arrays are already required to respect (mr_apply_file's own comment,
 * rewrite.h, states the precondition; cli/machotool.c is the one caller that
 * enforces it today). NULL is legal and means "do not count" -- the sizing
 * pass passes NULL, because counting a dry run would double every hit (see
 * mr_process_thin's two call sites). */
static int mr_build_lcs(const mi_image *im, const mr_ops *ops,
                        uint8_t *new_lcs, uint32_t *out_off, uint32_t *out_ncmds,
                        int *out_mods, int *out_renames, int verbose,
                        int *hit_dylib, int *hit_rpath, int *hit_strip) {
    struct mr_build_lcs_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ops = ops;
    ctx.new_lcs = new_lcs;
    ctx.ncmds = im->hdr->ncmds;
    ctx.verbose = verbose;
    ctx.hit_dylib = hit_dylib;
    ctx.hit_rpath = hit_rpath;
    ctx.hit_strip = hit_strip;

    if (!mi_each_lc(im, mr_build_lcs_lc, &ctx)) return -1;   /* refused; see mr_build_lcs_lc */

    uint32_t new_off = ctx.new_off, ncmds = ctx.ncmds;
    int mods = ctx.mods;

    /* An image with no dylib load commands at all still honours -insert; there
     * was simply nothing to insert in front of. */
    if (!ctx.placed_inserts) {
        for (int s = 0; s < ops->n_dylib_inserts; s++) {
            uint32_t cs = mr_emit_dylib_lc(new_lcs + new_off, ops->dylib_inserts[s]);
            new_off += cs;
            ncmds++;
            mods++;
            if (verbose) printf("  Insert [%u bytes]: LC_LOAD_DYLIB %s\n", cs, ops->dylib_inserts[s]);
        }
    }

    /* Append brand-new LC_LOAD_DYLIB commands (-add). Appending is ordinal-safe:
     * it only hands out indices past the existing ones. */
    for (int a = 0; a < ops->n_dylib_appends; a++) {
        uint32_t cs = mr_emit_dylib_lc(new_lcs + new_off, ops->dylib_appends[a]);
        new_off += cs;
        ncmds++;
        mods++;
        if (verbose) printf("  Add [%u bytes]: LC_LOAD_DYLIB %s\n", cs, ops->dylib_appends[a]);
    }

    /* An image with no LC_RPATH at all still honours an rpath -insert; there
     * was simply nothing to place it in front of. Emitting them HERE, before
     * the appends below rather than after, is what keeps "-insert then -append
     * into an image with no rpaths" producing the inserted one first -- the
     * only order in which the two operations still mean what they say. */
    if (!ctx.placed_rpath_inserts) {
        for (int s = 0; s < ops->n_rpath_inserts; s++) {
            uint32_t cs = mr_emit_rpath_lc(new_lcs + new_off, ops->rpath_inserts[s]);
            new_off += cs;
            ncmds++;
            mods++;
            if (verbose) printf("  Insert [%u bytes]: LC_RPATH %s\n", cs, ops->rpath_inserts[s]);
        }
    }

    /* Append brand-new LC_RPATH commands (-add-rpath), searched last. */
    for (int a = 0; a < ops->n_rpath_appends; a++) {
        uint32_t cs = mr_emit_rpath_lc(new_lcs + new_off, ops->rpath_appends[a]);
        new_off += cs;
        ncmds++;
        mods++;
        if (verbose) printf("  Add [%u bytes]: LC_RPATH %s\n", cs, ops->rpath_appends[a]);
    }

    *out_off = new_off;
    *out_ncmds = ncmds;
    *out_mods = mods;
    *out_renames = ctx.renames;
    return 0;
}

struct mr_cgb_ctx {
    uint32_t growth;
    const mr_ops *ops;
};

static int mr_cgb_lc(const struct load_command *lc, void *ctx_) {
    struct mr_cgb_ctx *ctx = ctx_;
    uint32_t cmdsize = lc->cmdsize;

    if (mo_is_ordinal_lc(lc->cmd)) {
        const struct dylib_command *dc = (const struct dylib_command *)lc;
        const char *name = mo_lc_str_at(lc, dc->dylib.name.offset);
        if (name) {
            for (int c = 0; c < ctx->ops->n_dylib_changes; c++) {
                if (strcmp(name, ctx->ops->dylib_changes[c].old_path) != 0) continue;
                if (ctx->ops->dylib_changes[c].new_path != NULL) {
                    size_t base = dc->dylib.name.offset;
                    size_t new_len = strlen(ctx->ops->dylib_changes[c].new_path) + 1;
                    uint32_t needed = (uint32_t)((base + new_len + 7) & ~7UL);
                    if (needed > cmdsize) ctx->growth += needed - cmdsize;
                }
                break;
            }
        }
    } else if (lc->cmd == LC_RPATH) {
        const struct rpath_command *rc = (const struct rpath_command *)lc;
        const char *rp = mo_lc_str_at(lc, rc->path.offset);
        if (rp) {
            for (int c = 0; c < ctx->ops->n_rpath_changes; c++) {
                if (strcmp(rp, ctx->ops->rpath_changes[c].old_path) != 0) continue;
                if (ctx->ops->rpath_changes[c].new_path != NULL) {
                    size_t base = rc->path.offset;
                    size_t new_len = strlen(ctx->ops->rpath_changes[c].new_path) + 1;
                    uint32_t needed = (uint32_t)((base + new_len + 7) & ~7UL);
                    if (needed > cmdsize) ctx->growth += needed - cmdsize;
                }
                break;
            }
        }
    }
    return 0;   /* pure accumulation; never needs to stop early */
}

/*
 * Exact (or, in one rare case, safely over-) budget for the extra bytes
 * mr_build_lcs's -change/-change-rpath growth will write beyond each matched
 * command's ORIGINAL cmdsize -- computed by walking the real load commands
 * in `im` the same way mr_build_lcs's own matching loop does, instead of
 * assuming "one grown command per -change/-change-rpath argument".
 *
 * That assumption was the bug: more than one load command can carry the same
 * install name (or rpath), and mr_build_lcs grows EVERY command that matches, so
 * a binary with two LC_LOAD_DYLIBs naming the same library and a single
 * -change for it needs budget for two grown commands, not one. Reproduced
 * with two synthetic LC_LOAD_DYLIBs sharing an install name and a -change
 * whose replacement path is ~9000 chars: the old per-argument budget sized
 * new_lcs for one growth, mr_build_lcs wrote two, and the second write ran past
 * the allocation -- a heap overflow confirmed under libgmalloc (SIGSEGV; without
 * it, memory corruption with exit 1).
 *
 * This performs the identical sizing mr_build_lcs performs -- round8(base +
 * strlen(new_path) + 1), kept only if it exceeds the original cmdsize -- over
 * every matching command rather than once per argument, and mirrors
 * mr_build_lcs's own "first match in `changes`/`rchanges` wins" rule so it
 * agrees with what mr_build_lcs will actually do for the ordinary case where a
 * name is named once. It is not bit-exact in one edge case: if the SAME old
 * path appears in both a -change and a separate -delete, mr_build_lcs's
 * `mr_is_deleted` check (not visible to a single first-match walk) makes
 * that command a deletion with no growth, while this still counts it as
 * growing. That only over-budgets -- harmless slack in new_lcs -- never
 * under-budgets, which is the property that matters here. Writes nothing;
 * reads bounds-checked via mo_lc_str_at, and silently does not count a
 * malformed command as a match -- mr_build_lcs performs the same check and
 * refuses the whole operation before it would ever act on that command, so
 * excluding it here cannot lead to writing past what was budgeted.
 *
 * Pure read-only accumulation into one running total, unlike mr_build_lcs's own
 * walk just below (which writes a whole new load-command table and threads
 * several more locals through the loop) -- that is what makes this one a
 * plain mi_each_lc conversion and mr_build_lcs not, see its comment.
 */
static uint32_t mr_change_growth_bytes(const mi_image *im, const mr_ops *ops) {
    struct mr_cgb_ctx ctx = { 0, ops };
    mi_each_lc(im, mr_cgb_lc, &ctx);
    return ctx.growth;
}

/* mr_process_thin's return codes. MR_SKIP is not an error: it means `label`
 * is not a (recognizable) 64-bit Mach-O, so this rewriter has nothing to do
 * to it -- the caller's job is to leave those bytes exactly as it found them.
 * That is what lets a fat binary carrying a slice this rewriter cannot
 * understand (32-bit, or any other format) still get its OTHER slices rewritten, matching how
 * fix_macho's per-arch loop already treats an unrecognized slice: skip it,
 * don't fail the whole file. MR_ERROR is a real failure -- the label WAS a
 * 64-bit Mach-O but the requested edit could not be made -- and the caller
 * must treat that as fatal to the whole operation (see mr_process_fat below):
 * partially rewriting a multi-arch binary would leave its slices disagreeing
 * about the edit, which is worse than refusing outright. */
#define MR_SKIP  (-2)
#define MR_ERROR (-1)

/* True if the only thing this operation set asks for is a segment rename:
 * no dylib or rpath change, append or insert, no load command to strip, and
 * no header growth. It exists for one decision -- see the mg_plausible gate
 * in mr_process_thin.
 *
 * WRITTEN AS "EVERYTHING ELSE IS EMPTY", not as "a rename is requested",
 * because the two differ for an operation set this function has never heard
 * of. But C gives that no force on its own: a field added to mr_ops and not
 * added to the conjunction below leaves this returning TRUE for
 * {rename, that new operation}, which is exactly the silent widening the
 * shape is meant to prevent. So the coupling is a BUILD failure, the same
 * device commit 247d09d used for mg_classify/ml_bump_lc: add a field to
 * mr_ops and this file stops compiling until someone comes here, reads the
 * paragraph above, and decides whether the new field belongs in the
 * conjunction.
 *
 * 152 and 148 are sizeof(mr_ops) and offsetof(mr_ops, allow_grow) -- the LAST
 * declared field -- on the only architecture this project builds (CMakeLists.txt
 * pins CMAKE_OSX_ARCHITECTURES to x86_64), so literals are stable here. They
 * are a tripwire, not a portability claim: on some other target the fix is to
 * re-derive both numbers AND re-read this function, which is the whole point.
 * Negative-array-size typedef rather than _Static_assert, which is C11 and
 * this project sets no -std=.
 *
 * What the typedef below actually checks, and no more: it fails to compile
 * exactly when an edit to mr_ops moves sizeof(mr_ops) or moves the offset of
 * allow_grow. That is the whole of the mechanism. An edit that changes the
 * struct while leaving both of those numbers where they are compiles clean
 * and is invisible to it -- so a change that alters what mr_is_rename_only
 * above should mean, while happening to preserve the struct's layout, still
 * needs a human to come here and re-read the conjunction; nothing forces
 * that to happen. There is no stronger C-level mechanism available: an
 * offsetof assertion per field would have the identical blind spot, since a
 * member that fits an existing hole moves no later field either, and a
 * memcmp-against-zero probe is unreliable, because struct padding is
 * indeterminate after assignment.
 *
 * For example -- one instance, not an inventory -- mr_ops is seven
 * (pointer, int n_*) pairs and so has seven interior four-byte padding
 * holes on this ABI; a new member of four bytes or fewer placed into one of
 * those holes moves neither number and compiles clean. (There used to be an
 * EIGHTH hole too, trailing after allow_grow to reach the 144-byte aligned
 * size. fatal_unmatched was deliberately declared BEFORE allow_grow, not
 * after -- see that field's own comment in rewrite.h -- which put
 * fatal_unmatched in allow_grow's OLD slot and pushed allow_grow itself
 * into what used to be that trailing hole, consuming it. Had
 * fatal_unmatched instead been declared after allow_grow, IT would have
 * landed in that hole, moving neither sizeof(mr_ops) nor
 * offsetof(allow_grow), and this typedef would have compiled clean over an
 * edit it exists to catch. With the trailing hole gone, a future
 * four-byte-or-smaller member appended AFTER allow_grow would now move
 * sizeof(mr_ops) and trip this check too; the seven interior holes are what
 * remains of the blind spot.) This paragraph has previously gone through
 * several versions, each naming a specific set of edits that get past this
 * check; each was wrong in a new way, because that set is "every edit that
 * preserves both numbers," which is unbounded and cannot be enumerated
 * correctly. This version names one member of it as an example of what
 * "invisible to it" means in practice, and stops there on purpose. */
typedef char mr_ops_layout_is_still_what_mr_is_rename_only_checks[
    (sizeof(mr_ops) == 152 && offsetof(mr_ops, allow_grow) == 148) ? 1 : -1];

static int mr_is_rename_only(const mr_ops *ops) {
    return ops->segment_rename_old != NULL && ops->segment_rename_new != NULL &&
           ops->n_dylib_changes == 0 && ops->n_dylib_appends == 0 &&
           ops->n_dylib_inserts == 0 && ops->n_rpath_changes == 0 &&
           ops->n_rpath_appends == 0 && ops->n_rpath_inserts == 0 &&
           ops->n_strip_cmds == 0 && ops->allow_grow == 0 &&
           /* fatal_unmatched governs whether mr_apply_file refuses when a
            * dylib_changes/rpath_changes/strip_cmds entry matched nothing --
            * and a rename-only ops has none of those (every count above is
            * already required to be 0), so fatal_unmatched has nothing to
            * act on here regardless of its value. A rename-only run WITH
            * fatal_unmatched set is still rename-only for the purpose of
            * this predicate. Named explicitly anyway (as a tautology, not a
            * `== 0` requirement) so that decision is visible in the
            * conjunction itself rather than being an omission a future
            * reader has to notice on their own -- which is exactly what the
            * layout tripwire above exists to force. */
           (ops->fatal_unmatched == 0 || ops->fatal_unmatched != 0);
}

/*
 * Apply every requested change to the single (thin) 64-bit Mach-O in
 * *pbuf, *pfsize, in place except that mg_grow_header may realloc *pbuf (its
 * usual contract: on success *pbuf, *pfsize are updated to the new buffer/size
 * and the caller owns it; on failure of the grow itself the caller still owns
 * whatever *pbuf now points to). `label` names this image only for diagnostic
 * printf's -- a file path for the thin case, an "arch N (cputype ...)" string
 * for a fat slice.
 *
 * Returns MR_SKIP if *pbuf is not a 64-bit Mach-O at all (buffer untouched),
 * MR_ERROR if it is one but the edit failed (message already printed on
 * stderr; buffer contents are unspecified beyond "still the caller's to
 * free"), or 0 on success with *out_modified reporting whether anything
 * actually changed.
 *
 * hit_dylib/hit_rpath/hit_strip: caller-owned per-operation hit counts
 * (mr_apply_file owns and zeroes them once), ADDED to here -- never
 * assigned -- so a fat file's multiple slices (mr_process_fat calls this
 * once per slice, sharing one set of arrays) accumulate across all of them;
 * an operation that matched in one slice and not another has matched.
 * Passed straight through to mr_build_lcs, which does the actual counting.
 */
static int mr_process_thin(uint8_t **pbuf, size_t *pfsize, const char *label,
                           const mr_ops *ops, int *out_modified,
                           int *hit_dylib, int *hit_rpath, int *hit_strip) {
    *out_modified = 0;
    uint8_t *buf = *pbuf;
    size_t fsize = *pfsize;

    mi_image im;
    if (mi_wrap(buf, fsize, &im) != 0) return MR_SKIP;
    struct mach_header_64 *hdr = im.hdr;
    /* mi_wrap never allocates or takes ownership (im.owned == 0), so there is
     * nothing to release here -- buf/fsize above already ARE the buffer. */

    uint32_t first_sect_off = mg_first_sect_off(buf, fsize);
    if (first_sect_off == UINT32_MAX) {
        fprintf(stderr, "ERROR: %s fails validation; refusing (see above)\n", label);
        return MR_ERROR;
    }
    /* first_sect_off bounds the commit's memset below. With no section data
     * there is no bound at all: mg_first_sect_off used to answer 4096 here,
     * and the memset cleared up to it -- past the end of a 104-byte image
     * (SIGSEGV under libgmalloc), and through real data in an 8192-byte one
     * (exit 0, 3904 bytes changed; see tests/leaf-tool-crashes.sh). Refused
     * before anything uses it. */
    if (first_sect_off == MG_NO_SECTION_DATA) {
        fprintf(stderr, "ERROR: %s: no section data bounds the header pad; "
                        "refusing to rewrite its load commands\n", label);
        return MR_ERROR;
    }
    /* Otherwise it is read from the file (mi_wrap does not check section
     * file ranges), and past the buffer's end it bounds nothing. */
    if (first_sect_off > fsize) {
        fprintf(stderr, "ERROR: %s: the first section's file offset (%u) lies past the end "
                        "of the image (%zu bytes); refusing\n", label, first_sect_off, fsize);
        return MR_ERROR;
    }
    uint32_t cur_lc_end = sizeof(struct mach_header_64) + hdr->sizeofcmds;
    uint32_t pad_avail = first_sect_off > cur_lc_end ? first_sect_off - cur_lc_end : 0;
    printf("%s: header pad %u bytes available (LC end=%u, first sect=%u)\n",
           label, pad_avail, cur_lc_end, first_sect_off);

    /* Upper bound on bytes the -add/-insert commands contribute, so the scratch
     * buffer can hold the full new table even before the header pad is grown. */
    uint32_t add_bytes = 0;
    for (int a = 0; a < ops->n_dylib_appends; a++)
        add_bytes += (uint32_t)((sizeof(struct dylib_command) + strlen(ops->dylib_appends[a]) + 1 + 7) & ~7UL);
    for (int s = 0; s < ops->n_dylib_inserts; s++)
        add_bytes += (uint32_t)((sizeof(struct dylib_command) + strlen(ops->dylib_inserts[s]) + 1 + 7) & ~7UL);
    for (int a = 0; a < ops->n_rpath_appends; a++)
        add_bytes += (uint32_t)((sizeof(struct rpath_command) + strlen(ops->rpath_appends[a]) + 1 + 7) & ~7UL);
    for (int s = 0; s < ops->n_rpath_inserts; s++)
        add_bytes += (uint32_t)((sizeof(struct rpath_command) + strlen(ops->rpath_inserts[s]) + 1 + 7) & ~7UL);
    /* -change/-change-rpath can ALSO grow a command past its original
     * cmdsize -- mr_build_lcs's `matched`/`rmatched` branches size the rewritten
     * command as (base + strlen(new_path) + 1), rounded up, keeping whichever
     * is larger of that or the original cmdsize (see the "if (needed <
     * cmdsize) needed = cmdsize;" lines in mr_build_lcs). The real bound is
     * "sum, over every EXISTING LOAD COMMAND that will actually match, of
     * that command's own growth" -- not "one grown command per -change
     * argument". A single -change argument can match more than one load
     * command (two LC_LOAD_DYLIBs can legitimately carry the same install
     * name), and each one grows independently, so a per-argument budget
     * undercounts whenever that happens: this was a real heap buffer
     * overflow (confirmed under libgmalloc: SIGSEGV; without libgmalloc,
     * silent corruption then exit 1), reproduced with two synthetic
     * LC_LOAD_DYLIBs sharing an install name and a -change whose replacement
     * path is ~9000 chars. mr_change_growth_bytes computes the real bound by
     * walking the actual load commands the same way mr_build_lcs's matching
     * loop does -- see its own comment for the one (safe, over- not
     * under-) approximation it still makes. */
    add_bytes += mr_change_growth_bytes(&im, ops);

    /* Map each existing 1-based library ordinal to its new value (0 = deleted),
     * built once by mo_map_build so this rewrite and the ordinal renumbering
     * below (mo_map_apply) can't independently disagree about which dylib
     * landed where -- see ordinals.h. Inserts take 1..ninserts, so every
     * survivor shifts up by that much; each deletion shifts the ones after it
     * back down. */
    int ord_map[MO_MAX_DYLIBS + 1];
    uint32_t ord_cmd[MO_MAX_DYLIBS + 1];
    memset(ord_map, 0, sizeof ord_map);
    memset(ord_cmd, 0, sizeof ord_cmd);
    mo_map omap = { ord_map, 0, ord_cmd };
    int nnew;
    if (mo_map_build(buf, hdr->ncmds, ops->n_dylib_inserts, mr_is_deleted, (void *)ops, &omap, &nnew) != 0)
        return MR_ERROR;
    int nold = omap.n;
    /* A survivor count below nold means at least one dylib was deleted; that
     * and any -insert are the only reasons a rewrite needs to renumber. */
    int needs_renumber = (ops->n_dylib_inserts > 0) || (nnew - ops->n_dylib_inserts < nold);
    if (nnew + ops->n_dylib_appends > MO_MAX_DYLIBS) {
        fprintf(stderr, "ERROR: %s: result would exceed %d dylibs\n", label, MO_MAX_DYLIBS);
        return MR_ERROR;
    }

    /* Build the new table once to learn its size (and print diagnostics).
     * This is the counting pass: hit_dylib/hit_rpath/hit_strip are real
     * here, and whatever this call finds is what gets reported, whether or
     * not a header grow later replaces the TABLE this call built (the SET
     * of load commands -- and so which operations match -- does not change
     * when the header grows; only file offsets elsewhere in the image do). */
    uint8_t *new_lcs = calloc(1, first_sect_off + add_bytes + 64);
    uint32_t new_off, new_ncmds; int modifications; int renames;
    if (mr_build_lcs(&im, ops, new_lcs, &new_off, &new_ncmds, &modifications, &renames, 1,
                      hit_dylib, hit_rpath, hit_strip) != 0) {
        free(new_lcs);
        return MR_ERROR;
    }

    if (modifications == 0) { printf("%s: nothing to change.\n", label); free(new_lcs); return 0; }

    /* The new table must fit before the first section's data. The boundary is
     * sizeof(mach_header_64) + sizeofcmds; using new_off alone would understate
     * it by the 32-byte header and allow a 16-byte overlap into the section. */
    uint32_t need_end = (uint32_t)sizeof(struct mach_header_64) + new_off;
    if (need_end > first_sect_off) {
        /* Whether there is room, and whether to grow, is mg_ensure_pad's
         * decision (src/grow.h) -- one place, shared with version-min. It
         * prints the grow path's stdout lines itself, unchanged. */
        if (mg_ensure_pad(&buf, &fsize, need_end, ops->allow_grow, label) != 0) {
            *pbuf = buf; *pfsize = fsize;   /* growth may have realloc'd before failing */
            free(new_lcs);
            return MR_ERROR;
        }
        *pbuf = buf; *pfsize = fsize;       /* mg_ensure_pad may have realloc'd */
        hdr = (struct mach_header_64 *)buf;
        first_sect_off = mg_first_sect_off(buf, fsize);
        if (first_sect_off == UINT32_MAX) {
            fprintf(stderr, "ERROR: %s: header grow produced an image that fails validation\n", label);
            free(new_lcs);
            return MR_ERROR;
        }
        /* Unreachable: growth only ran because the image had section data,
         * which it moves and never removes. Refused anyway, the same way as
         * above, because first_sect_off bounds the commit memset below. */
        if (first_sect_off == MG_NO_SECTION_DATA) {
            fprintf(stderr, "ERROR: %s: no section data bounds the header pad; "
                            "refusing to rewrite its load commands\n", label);
            free(new_lcs);
            return MR_ERROR;
        }
        /* growth (mg_ensure_pad -> mg_grow_header) reallocs the raw buffer,
         * not through image.h, so the `im` wrapped at the top of this
         * function is stale here (it still points at whatever `buf` was
         * before the realloc). Re-wrap it over the relocated buffer --
         * mi_wrap never allocates or frees (im.owned stays 0), so
         * overwriting `im` in place is safe, and this re-wrap cannot fail in
         * practice: mg_first_sect_off just above ran the same mi_wrap
         * validation against this exact buf/fsize and already returned
         * success. Handled defensively anyway, same as every other "provably
         * unreachable, checked anyway" spot in this codebase. */
        if (mi_wrap(buf, fsize, &im) != 0) {
            fprintf(stderr, "ERROR: %s: grown header fails validation; refusing\n", label);
            free(new_lcs);
            return MR_ERROR;
        }
        /* Rebuild against the relocated header so segment/linkedit offsets in
         * the copied load commands reflect the shift. NULL counters here,
         * deliberately: this walks the SAME load commands the call above
         * already counted (the grow moved offsets elsewhere in the image,
         * not which command matches which operation), so passing the real
         * arrays a second time would double-count every hit into a false
         * "matched twice" that this operation only did once. */
        free(new_lcs);
        new_lcs = calloc(1, first_sect_off + add_bytes + 64);
        if (mr_build_lcs(&im, ops, new_lcs, &new_off, &new_ncmds, &modifications, &renames, 0,
                          NULL, NULL, NULL) != 0) {
            free(new_lcs);
            return MR_ERROR;
        }
    }

    /* Check the map against what mr_build_lcs actually emitted -- not just
     * against its own arithmetic -- before committing anything. This is the
     * cross-check that catches the map and the load-command rewrite having
     * independently disagreed about which dylib survived, which the map's
     * own internal consistency (checked inside mo_map_build) cannot: that
     * only proves the map is self-consistent, not that it matches reality. */
    if (mo_map_validate(&omap, ops->n_dylib_inserts, nnew, ops->n_dylib_appends, new_lcs, new_ncmds) != 0) {
        fprintf(stderr, "ERROR: %s left unmodified\n", label);
        free(new_lcs);
        return MR_ERROR;
    }

    /* Commit: zero the whole LC area, write the new table, fix up the header. */
    memset(buf + sizeof(struct mach_header_64), 0, first_sect_off - sizeof(struct mach_header_64));
    memcpy(buf + sizeof(struct mach_header_64), new_lcs, new_off);
    hdr->ncmds = new_ncmds;
    hdr->sizeofcmds = new_off;
    free(new_lcs);

    /* Ordinals last, against the committed table — and before any write, so a
     * refusal leaves the input untouched rather than half-rewritten. */
    mo_counts ord_counts;
    memset(&ord_counts, 0, sizeof ord_counts);
    if (needs_renumber && mo_map_apply(buf, fsize, &omap, 1, &ord_counts) != 0) {
        fprintf(stderr, "ERROR: %s left unmodified\n", label);
        return MR_ERROR;
    }

    /* Last gate before the bytes reach disk. This rewrite is the FINAL stage
     * of the wrapper's chain (patch_macho -> add_version_min -> change_dylib),
     * so a check here covers the cumulative end state of all of them --
     * including patch_macho's chained-fixups conversion, which has ~94,900
     * rebases and no self-check of its own. It needs no "before" image, which
     * is what makes it usable across process boundaries.
     *
     * This is the difference between "the wrapper installed that over your
     * binary, re-download that version" and "patch refused, nothing lost" --
     * this rewriter writes a NEW file now, but the compat wrappers still mv it
     * over the caller's, so the gate protects the same thing it always did.
     * MACHO_NO_VERIFY=1 opts out.
     *
     * NOT RUN FOR A RENAME-ONLY OPERATION SET, and that is a statement about
     * what mg_plausible checks rather than a concession. It asks whether the
     * image's initializers and compact-unwind entries still name functions
     * LC_FUNCTION_STARTS knows about (src/grow.h) -- an OFFSET question. A
     * segment rename writes characters into segname/sectname fields and moves
     * nothing: mseg_rename_lc touches neither cmd nor cmdsize (src/segname.h),
     * mr_build_lcs applies it to a command it has already copied, and no
     * offset in the image changes. So the gate cannot catch anything a rename
     * did; it can only re-decide a property the INPUT already had, and refuse
     * a file the caller never asked it to judge.
     *
     * That is not hypothetical -- but the evidence originally recorded here
     * for it was. This comment used to say mg_plausible's heuristic has false
     * positives on real, untouched 10.9 system dylibs, naming
     * libSystem.B.dylib, libc++.1.dylib, libicucore.A.dylib and libz.1.dylib
     * as refused by `macho9 lc -delete uuid` where /bin/ls, /bin/cat,
     * /usr/bin/grep and /usr/bin/awk passed, and 14 of the 16 thin binaries
     * in a 120-file /usr/lib corpus (tests/differential.sh) as refused by
     * `macho9 segment`. That split -- every dylib refused, every executable
     * passed -- was not the heuristic at all: mg_plausible read its image
     * base from mi_text_base, whose 0 means BOTH "no segment maps the header"
     * and "the base is 0", and a dylib is linked at base 0. It bailed at the
     * precondition and never ran the heuristic. mi_image_base tells those
     * apart now (src/image.h), and all four named dylibs pass. The corpus
     * count was not re-measured; assume it was the same bug.
     *
     * Do not read a claim about false positives back into this. The only
     * input in this tree the gate is demonstrated to refuse is
     * tests/mkimplausible.c's fixture, and that is a TRUE positive: it is
     * built with an __init_offsets entry at 0x999 when the sole function
     * start is base + 0x400, which is exactly the un-re-based-offset
     * signature src/grow.c's check exists to catch. As of this commit NO
     * false positive of mg_plausible on a real image is demonstrated
     * anywhere here.
     *
     * The scoping below never depended on how common false positives are,
     * which is why it stands unchanged on the corrected facts: a rename moves
     * no offset, so the gate cannot catch anything a rename did, only
     * re-decide a property the input already had.
     *
     * Scoped by mr_is_rename_only, not by an environment variable: an env var
     * would switch the gate off for the whole machotool invocation, would keep
     * covering any operation a caller later added to the same command line,
     * and would read like someone disabling a safety check. This says the one
     * true thing instead, at the one site where it is true. Every operation
     * that CAN move an offset still meets the gate exactly as before. */
    if (!mr_is_rename_only(ops) && !getenv("MACHO_NO_VERIFY") &&
        mg_plausible(buf, fsize) != 0) {
        fprintf(stderr, "ERROR: refusing to modify %s -- it would carry base-relative "
                        "offsets that name no known function. Left unmodified.\n", label);
        return MR_ERROR;
    }

    /* Report the match count only now, past every gate: a refused rewrite
     * renamed nothing on disk, and a front-end that reports "renamed 2" for a
     * file it did not write would be the silent-success shape this codebase
     * refuses. ADDED, not assigned, because mr_process_fat calls this once per
     * slice and the caller's total is across all of them; within one slice the
     * value is whatever the LAST mr_build_lcs produced (a rebuild after a
     * header grow replaces it rather than doubling it). */
    if (ops->segment_renamed) *ops->segment_renamed += renames;

    /* The renumbering, on the same terms and for the same reason: a report
     * of the map this rewrite built and the counts its one mo_map_apply
     * walk kept, copied out, not re-derived. */
    if (needs_renumber && ops->renumbering) {
        mr_renumbering *r = ops->renumbering;
        r->n = omap.n;
        r->inserted = ops->n_dylib_inserts;
        memcpy(r->old_to_new, ord_map, sizeof r->old_to_new);
        memcpy(r->old_cmd, ord_cmd, sizeof r->old_cmd);
        r->counts = ord_counts;
        r->done = 1;
    }

    *pbuf = buf; *pfsize = fsize;
    *out_modified = 1;
    printf("%s: updated (sizeofcmds=%u, %zu bytes)\n", label, new_off, fsize);
    return 0;
}

/* The verb path's slice callback: the thin rewrite, under the label and with
 * the stdout lines `machotool dylib`/`change_dylib` have always printed for a
 * fat file. */
typedef struct {
    const mr_ops *ops;
    int *hit_dylib, *hit_rpath, *hit_strip;
} mr_fat_ctx;

static int mr_fat_slice(uint8_t **pbuf, size_t *psize, const mfat_arch *a,
                        uint32_t index, int *changed, void *ctx_) {
    mr_fat_ctx *c = (mr_fat_ctx *)ctx_;
    char label[64];
    snprintf(label, sizeof label, "arch %u (cputype 0x%x)", index, a->cputype);
    int mod = 0;
    int rc = mr_process_thin(pbuf, psize, label, c->ops, &mod,
                             c->hit_dylib, c->hit_rpath, c->hit_strip);
    if (rc == MR_SKIP) {
        printf("%s: not a 64-bit Mach-O; leaving this slice unchanged\n", label);
        return 0;
    }
    if (rc == MR_ERROR) {
        fprintf(stderr, "ERROR: %s: refusing the whole fat file -- a partial "
                        "rewrite would leave its slices inconsistent\n", label);
        /* MR_REFUSED even when the slice's MR_ERROR came from an allocation
         * failure inside mg_grow_header or mg_plausible: the same deliberate
         * fold as the thin path's, whose comment at its own
         * MR_ERROR->MR_REFUSED translation (mr_apply_image) says why. */
        /* MR_REFUSED, never MR_ERROR itself: MR_SKIP (-2) and MR_ERROR (-1)
         * are numerically MFAT_MALFORMED and MFAT_IO_ERROR, both in scope in
         * this file now; fat.h requires a POSITIVE code from a slice callback
         * for exactly that reason -- one forwarding MR_ERROR raw would be
         * read as mfat_rewrite's own allocation failure. */
        return MR_REFUSED;
    }
    *changed = mod;
    return 0;
}

static void mr_fat_placed(const mfat_arch *a, uint32_t index,
                          uint64_t off, uint64_t size, void *ctx) {
    (void)a; (void)ctx;
    printf("arch %u: placed at %llu (%llu bytes)\n", index,
           (unsigned long long)off, (unsigned long long)size);
}

/*
 * Apply every requested change to every slice of a fat (universal) binary in
 * *pbuf, *pfsize, reassembling the fat container afterward. This is what
 * closes the fat gap in the rewrite path: fix_macho already walks fat/thin,
 * change_dylib only understood thin until it grew this.
 *
 * A slice this rewriter cannot understand (anything mr_process_thin reports
 * MR_SKIP for -- today that means anything but a 64-bit Mach-O; 32-bit stays
 * deliberately unsupported, see src/grow.h) is passed through byte-for-byte
 * unchanged, exactly like fix_macho's own per-arch loop already does ("Not
 * 64-bit Mach-O ... Skipping arch"). A slice that IS a 64-bit Mach-O but
 * where the requested edit itself fails (MR_ERROR) aborts the WHOLE
 * operation: a fat binary's slices are all meant to carry the same edit
 * (the same -change, the same -insert, ...), and writing some of them but
 * not others would leave the result internally inconsistent -- worse than
 * refusing outright, and not what a "delete succeeded, or nothing was
 * touched" contract can allow. See mr_process_thin's own comment for why
 * MR_SKIP and MR_ERROR need different treatment.
 *
 * Sizes: without -grow, or when growth was not needed, every slice keeps its
 * original size, and this reassembly places every slice back at its ORIGINAL
 * file offset -- so a fat binary edited without size changes ends up with
 * exactly the same layout it started with. Only once some earlier slice's
 * size actually changes does a later slice's offset get recomputed, packed
 * tightly against the slice before it at that slice's own (preserved)
 * alignment. That is what keeps an unmodified multi-arch binary's on-disk
 * shape untouched while still supporting the resize -grow needs.
 *
 * hit_dylib/hit_rpath/hit_strip: the SAME three caller-owned arrays are
 * passed to every slice's mr_process_thin call, so hits accumulate ACROSS
 * slices rather than being reported per slice -- an operation that matched
 * in one fat slice and not another has matched, and a per-slice report would
 * wrongly call that a miss on every slice but one.
 */
static int mr_process_fat(uint8_t **pbuf, size_t *pfsize,
                          const mr_ops *ops, int *out_modified,
                          int *hit_dylib, int *hit_rpath, int *hit_strip) {
    *out_modified = 0;
    /* mfat_parse (src/fat.c) is the ONE place both this rewriter and
     * fix_macho validate a fat file's arch table -- magic, the table fitting
     * inside the file, every entry's offset+size in bounds and not
     * overlapping the header/table region itself, AND no two declared
     * slices overlapping EACH OTHER (a fat file whose own arch table already
     * aliases two slices is malformed on the read side, before this rewrite
     * ever computes a single new offset). Before this, each tool had its own
     * hand-rolled walk and they disagreed about validation (fix_macho
     * trusted an arch's offset/size outright); see fat.h's file header for
     * the fuller story. */
    /* mr_process_fat's return value flows straight into mr_apply_file's own
     * `rc` (its one caller assigns it directly), so it is bound by the same
     * MR_REFUSED/MR_FAIL split as every return there -- see the comment on
     * mr_apply_file itself, in rewrite.h, for the dividing line. */
    uint32_t narch; int swap;
    int fp_rc = mfat_parse(*pbuf, *pfsize, &narch, &swap);
    if (fp_rc == MFAT_IO_ERROR) {
        fprintf(stderr, "ERROR: out of memory validating the fat arch table\n");
        return MR_FAIL;
    }
    if (fp_rc != 0) {
        fprintf(stderr, "ERROR: malformed fat file (bad magic, arch table past the end, "
                        "a slice overlapping the header, or two slices overlapping "
                        "each other)\n");
        return MR_REFUSED;
    }
    mr_fat_ctx ctx = { ops, hit_dylib, hit_rpath, hit_strip };
    int rc = mfat_rewrite(pbuf, pfsize, narch, swap, mr_fat_slice, mr_fat_placed,
                          &ctx, out_modified);
    if (rc == MFAT_IO_ERROR) return MR_FAIL;
    if (rc == MFAT_MALFORMED) return MR_REFUSED;
    if (rc != 0) return rc;
    if (!*out_modified) printf("Nothing to change.\n");
    return 0;
}

/* The other half of "silent success": a -replace/-delete/-strip-lc naming
 * something the image never had matched nothing, and until this function
 * existed said so nowhere -- exit 0, and the miss simply wasn't mentioned.
 * docs/PROPOSAL.md's "verify" section is the argument for closing this: every
 * defect found in this codebase has been exactly this shape.
 *
 * Reported once per FILE, after every slice has run -- not per slice. An
 * operation that matched in one fat slice and not another has matched; a
 * per-slice report would call that a miss on every slice but one. Appends
 * and inserts are never checked here: they always act (there is nothing in
 * the image for them to fail to find), so they have no hit array and cannot
 * be reported as missed.
 *
 * A slice mr_process_thin reports MR_SKIP for (mr_process_fat: not a 64-bit
 * Mach-O, passed through byte-for-byte) is never examined by mr_build_lcs at
 * all, so it contributes no hits, the same as if it did not exist. "No load
 * command of kind codesig to delete" is therefore a fact about the slices
 * THIS TOOL UNDERSTOOD, not necessarily about the whole image -- a fat file
 * with one 64-bit slice this rewriter skips for some other reason and one it
 * rewrites can still report a miss for a kind that exists only in the
 * skipped slice. That is the same boundary mr_process_thin/mr_process_fat's
 * own MR_SKIP contract already draws everywhere else in this file; this
 * report does not attempt to see past it.
 *
 * On stderr, deliberately. Six compat/ wrapper shell scripts wrap this
 * binary for six historical tool names, and every one of them has a stdout
 * contract that tests/known-callers.sh and tests/wrapper_test.sh pin. The
 * contract is not the same for all six -- five must reproduce their tool's
 * stdout byte for byte, while fix_macho's is deliberately NOT byte-identical
 * (compat/fix_macho.sh's DELIBERATE DIVERGENCES block says which lines moved
 * and why) -- but that difference does not weaken the reason for stderr, it
 * strengthens it: fix_macho's stdout is pinned to a shape the repo CHOSE,
 * one assertion at a time, and an unmatched report appearing on it would
 * break those assertions exactly as it would break the byte-identical five.
 * What every wrapper has in common is that its stdout is somebody's
 * contract; none of them has ever had to reproduce a stderr line. So stderr
 * is where a per-operation diagnostic can be added without moving anything
 * six wrappers' worth of tests are holding still.
 *
 * Returns the number of entries reported as unmatched, so
 * mr_unmatched_verdict (below) can turn this report into a refusal
 * (ops->fatal_unmatched) without re-scanning the hit arrays itself. */
static int mr_report_unmatched(const mr_ops *ops, const int *hit_dylib,
                                const int *hit_rpath, const int *hit_strip) {
    /* The "machotool: " prefix on the three lines below is DELIBERATE and is
     * the one program-specific string in this file -- every other diagnostic
     * here is program-neutral ("ERROR: ..."), because this library does not
     * otherwise know which front end is running it. It names machotool
     * because the report names operations in MACHOTOOL'S grammar ("-replace
     * X matched nothing" is about a `machotool dylib` operation, not about
     * whatever argv the caller typed), and every compat/ wrapper's job is to
     * teach that grammar: each prints the equivalent machotool command line
     * before running it, so a caller who sees "machotool: ..." on stderr has
     * just been shown the machotool command it is talking about. That is
     * also why the prefix had to move when the binary was renamed: it is
     * the grammar's name, and the grammar is machotool's now. Changing it to
     * argv[0] instead would make the wrapper case name the old C tool and so
     * name a grammar these operations are not written in.
     *
     * Coupled to: the wrappers' teaching output, and the text asserted in
     * tests/wrapper_test.sh, tests/cli_test.sh and
     * tests/change_dylib_test.sh. Only wrapper_test.sh's two assertions
     * anchor on the prefix itself; the other two match the part after it. */
    int n = 0;
    for (int i = 0; i < ops->n_dylib_changes; i++)
        if (hit_dylib[i] == 0) {
            fprintf(stderr, "machotool: %s matched nothing\n",
                    ops->dylib_changes[i].old_path);
            n++;
        }
    for (int i = 0; i < ops->n_rpath_changes; i++)
        if (hit_rpath[i] == 0) {
            fprintf(stderr, "machotool: rpath %s matched nothing\n",
                    ops->rpath_changes[i].old_path);
            n++;
        }
    for (int i = 0; i < ops->n_strip_cmds; i++)
        if (hit_strip[i] == 0) {
            fprintf(stderr, "machotool: no load command of kind %s to delete\n",
                    lc_kind_name(ops->strip_cmds[i]));
            n++;
        }
    return n;
}

/* See rewrite.h. The report, then the one decision fatal_unmatched makes
 * about it -- in one place, so mr_apply_file (after its write) and
 * src/edit.c (after each statement) cannot disagree about what "matched
 * nothing" promotes to. */
int mr_unmatched_verdict(const mr_ops *ops, const mr_hits *hits) {
    int nunmatched = mr_report_unmatched(ops, hits->dylib, hits->rpath, hits->strip);
    /* MR_REFUSED, not a bare 1, so the callers and this library cannot drift
     * about what number means "fatal_unmatched fired" -- see MR_REFUSED's own
     * comment in rewrite.h for why it is safe to forward verbatim. */
    return (ops->fatal_unmatched && nunmatched > 0) ? MR_REFUSED : 0;
}

/* See rewrite.h: mr_process_thin, with its private per-slice codes turned
 * into the MR_REFUSED/0 every caller outside this file speaks. */
int mr_apply_image(uint8_t **pbuf, size_t *pfsize, const char *label,
                   const mr_ops *ops, int *out_modified, mr_hits *hits) {
    int po = mr_process_thin(pbuf, pfsize, label, ops, out_modified,
                              hits->dylib, hits->rpath, hits->strip);
    if (po == MR_SKIP) {
        /* Unreachable from mr_apply_file: its mi_open already validated this
         * exact buffer with the identical algorithm mr_process_thin's own
         * mi_wrap runs on it, so mi_wrap cannot disagree. From src/edit.c
         * the buffer is whatever the previous statement left, and not every
         * operation validates what it hands back (mv_add_version_min_image,
         * mswift_retag_image and md_declassify_buf do not). What keeps this
         * unreachable there in practice is that each of those writes
         * well-formed load commands, and that this very mi_wrap -- or the
         * next statement's, in src/edit.c's me_view -- re-validates the
         * image before anything walks it. That is not the same proof, which
         * is one more reason this stays a refusal rather than an assertion. The
         * detailed three-way diagnostic lives at mr_apply_file's mi_open
         * failure site, where it is actually reachable. */
        fprintf(stderr, "%s: not a 64-bit Mach-O (rejected during processing)\n", label);
        return MR_REFUSED;
    }
    /* MR_ERROR here means mr_process_thin (or a primitive it called -- see
     * the list in mr_apply_file's own rewrite.h comment) examined the bytes
     * and declined, so this is MR_REFUSED, never MR_FAIL -- with one
     * folded-in exception, deliberate, not an oversight: mg_grow_header and
     * mg_plausible each return the same -1 for an allocation failure as for
     * their content checks. mg_grow_header's include its two reallocations
     * of the WHOLE image (grow.c:1075's `realloc(buf, fsize + grow)`, and
     * grow.c:1268's, growing __LINKEDIT for a rebuilt export trie) as well as
     * side tables (its address snapshot, grow.c:223 via :1067; the
     * export-trie walk's scratch table, grow.c:525 via :1042 and :1174; the
     * trie rebuilder's, src/trie.c; mg_verify's, grow.c:234) and every
     * allocation of the mg_plausible it runs last. mg_plausible's own are
     * side tables only (grow.c:719-721, and grow.c:525 again through
     * mg_collect). mr_process_thin's single MR_ERROR return from either one
     * cannot tell that failure apart from every other reason those two
     * functions refuse. Splitting it would mean widening mg_grow_header's
     * and mg_plausible's own return contracts (both currently a flat "0 or
     * -1") to say which -- a change later work already plans to make when
     * it restructures those two functions, not one to fold in here as a
     * side effect. So, plainly: an allocation failure inside either one
     * exits 1 (MR_REFUSED), not 2, same as every other reason
     * mg_grow_header or mg_plausible refuses -- and that is not confined to
     * growing. mg_grow_header is reached only with allow_grow, but
     * mr_process_thin runs mg_plausible on every rewrite that is not a pure
     * segment rename (see mr_is_rename_only) unless MACHO_NO_VERIFY is set,
     * so an ordinary dylib/rpath/lc edit reaches it too. mr_process_fat
     * makes the identical translation for a fat slice. */
    return (po == MR_ERROR) ? MR_REFUSED : 0;
}

int mr_apply_file(const char *path, const char *out, const mr_ops *ops) {
    /* Per-operation hit counts for mr_unmatched_verdict below. Owned and
     * zeroed here, once, so a fat file's slices (each processed by its own
     * mr_process_thin call, via mr_process_fat) all accumulate into the SAME
     * arrays -- see mr_process_fat's own comment for why that matters. Sized
     * MR_MAX_OPS / MR_MAX_STRIP, the same bound `ops`'s own arrays are
     * required to respect -- an UNENFORCED precondition on this function's
     * caller; see this function's own declaration in rewrite.h for the
     * detail (only cli/machotool.c enforces it today, and only because it is
     * the sole caller, not because anything here checks). */
    mr_hits hits;
    memset(&hits, 0, sizeof hits);

    /* O_RDONLY, and closed again at once: `path` is an INPUT now -- the result
     * goes to `out`, through wa_write_new below -- so nothing is ever written
     * through this descriptor, and an unwritable `path` is no longer this
     * function's business to refuse. (It used to open O_RDWR precisely so that
     * an unwritable file failed before any analysis. Reproducing that refusal
     * for the historical tools, which really did edit their argument, is the
     * compat wrappers' job now: mw_prepare, compat/machotool-compat.sh.) The fd
     * is not used for the THIN read either: only to learn the size and to peek
     * the magic, since a fat file's magic isn't MH_MAGIC_64 and mi_open (thin
     * only) would refuse it outright. This is the one place that has to tell
     * fat from thin apart before choosing how to read the rest. */
    /* Every return in this function is MR_REFUSED or MR_FAIL, matching the
     * dividing line this function's own comment in rewrite.h draws: MR_FAIL
     * for open/fstat/read/write/malloc itself failing (this function's own,
     * directly below; mfat_rewrite's checked ones; or mi_open's/
     * mfat_parse's, one level down), MR_REFUSED for everything that
     * examined the bytes (even "too small to be a Mach-O", which never gets
     * as far as reading load commands) and declined. This does not cover
     * every allocation reachable from this function. mg_grow_header's and
     * mg_plausible's own are the deliberate exception, folded into
     * MR_REFUSED instead; see the comment in mr_apply_image, above, where
     * mr_process_thin's MR_ERROR becomes MR_REFUSED, for why. And two
     * callocs in this file are not checked at all -- both of
     * mr_process_thin's new_lcs tables -- so their failure reaches neither
     * code. */
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return MR_FAIL; }

    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return MR_FAIL; }
    if (st.st_size < 4) {
        fprintf(stderr, "%s: too small to be a Mach-O\n", path);
        close(fd);
        return MR_REFUSED;
    }

    uint32_t magic;
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, &magic, sizeof magic) != (ssize_t)sizeof magic) {
        perror("read"); close(fd); return MR_FAIL;
    }

    uint8_t *buf;
    size_t fsize;
    int modified = 0;
    int rc;

    /* A 64-bit fat container (fat_arch_64 -- wide offsets, used for arm64e /
     * watchOS-style slices) genuinely IS a Mach-O; this rewriter just doesn't
     * speak that variant, only the classic 32-bit-offset fat_arch one. Say
     * so explicitly rather than falling through to the thin path's "not a
     * readable 64-bit Mach-O", which reads as "this isn't Mach-O at all". */
    if (magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64) {
        fprintf(stderr, "%s: 64-bit fat Mach-O (fat_arch_64); not supported -- only the "
                        "32-bit-offset fat_arch container is\n", path);
        close(fd);
        return MR_REFUSED;
    }

    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        /* Fat: mi_open only understands a thin 64-bit Mach-O, so this is the
         * one shape it cannot serve -- read the raw bytes ourselves.
         * mfat_parse (inside mr_process_fat) does this format's own
         * validation. */
        fsize = (size_t)st.st_size;
        buf = (uint8_t *)malloc(fsize);
        if (!buf) { fprintf(stderr, "out of memory\n"); close(fd); return MR_FAIL; }
        if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, buf, fsize) != (ssize_t)fsize) {
            perror("read"); close(fd); free(buf); return MR_FAIL;
        }
        close(fd);
        rc = mr_process_fat(&buf, &fsize, ops, &modified, hits.dylib, hits.rpath, hits.strip);
    } else {
        /* Thin (or not a Mach-O at all): mi_open does the actual read and
         * full validation -- cmdsize bounds/alignment and LC_SEGMENT_64/
         * nsects agreement, none of which the magic-only peek above looked
         * at. mi_release hands this function ownership of the buffer,
         * needed because mg_grow_header (inside mr_process_thin, via
         * allow_grow) reallocs it -- an mi_image left pointing at the old
         * allocation would be a dangling pointer waiting for a mi_close that
         * never comes. */
        close(fd);
        mi_image im;
        int mo_rc = mi_open(path, &im);
        if (mo_rc == MI_IO_ERROR) {
            /* Not only a TOCTOU race, though that is one way here: this
             * function's own open/fstat/read above, just before this
             * branch, only proved the path opens and its first 4 bytes
             * read -- mi_open's independent, SECOND open reads the WHOLE
             * file into a fresh malloc, either of which (the read, or the
             * allocation) can fail on its own even with nothing racing.
             * Either way this is a real environment failure, never a
             * considered refusal, so MR_FAIL. */
            fprintf(stderr, "%s: cannot open or read\n", path);
            return MR_FAIL;
        }
        if (mo_rc != 0) {
            /* mi_open's only other failure is MI_NOT_MACHO, which reports
             * pass/fail only -- on failure "*out is untouched and nothing is
             * allocated" (its own contract), so there is no buffer here to
             * inspect for WHY. Reconstruct the three-way too-short/bad-magic/
             * malformed diagnostic change_dylib has always given from what's
             * already in hand instead: st.st_size (the real file size, from
             * the fstat above) and magic (the 4-byte peek above -- valid
             * here since the fat-magic branch above already ruled out both
             * fat magics). */
            if ((size_t)st.st_size < sizeof(struct mach_header_64)) {
                fprintf(stderr, "%s: too short to be a 64-bit Mach-O (%lld bytes, need at "
                                "least %zu)\n", path, (long long)st.st_size,
                                sizeof(struct mach_header_64));
            } else if (magic != MH_MAGIC_64) {
                fprintf(stderr, "%s: not a 64-bit Mach-O (magic 0x%x)\n", path, magic);
            } else {
                fprintf(stderr, "%s: malformed 64-bit Mach-O (load commands fail "
                                "validation -- truncated, misaligned, or out of bounds; "
                                "see any earlier message)\n", path);
            }
            return MR_REFUSED;
        }
        fsize = im.size;
        buf = mi_release(&im);

        /* The rewrite itself, and its MR_SKIP/MR_ERROR -> MR_REFUSED
         * translation (including the allocation fold that comes with it),
         * are mr_apply_image's -- see its comment above. */
        rc = mr_apply_image(&buf, &fsize, path, ops, &modified, &hits);
    }

    /* THE MISS REPORT AND ITS VERDICT COME BEFORE THE WRITE, and the order is
     * load-bearing. `rc` here is still exactly what mr_process_thin /
     * mr_process_fat returned, which is the gate this report needs: a refused
     * rewrite errored out for its own reason, possibly before mr_build_lcs ever
     * ran a single comparison, so its hit arrays mean nothing and reporting
     * them would risk calling an operation "matched nothing" that never got a
     * chance to match anything at all.
     *
     * Running it AFTER the write -- where it used to be, so that it read as a
     * summary of a result already on disk -- would now mean creating `out` and
     * then returning MR_REFUSED for a --fatal-warnings miss: a refused run that
     * left an output behind. That was harmless while the write was conditional
     * on `modified`, because a run where every operation missed changed nothing
     * and so wrote nothing; with `out` written unconditionally it is not. The
     * report itself is on stderr (mr_report_unmatched), so moving it costs
     * nothing in the stdout a successful run produces. */
    if (rc == 0) rc = mr_unmatched_verdict(ops, &hits);

    /* Written even when nothing changed: a 0 exit means `out` IS the answer, so
     * it has to exist either way -- an identical copy of `path` when no
     * operation matched. wa_write_new creates it afresh from `path`'s mode,
     * owner and xattrs and never touches `path`; WA_IS_INPUT can only happen if
     * a path changed under us, since cli/machotool.c refuses `out` == `path` up
     * front (see mr_apply_file's PRECONDITION in rewrite.h). `modified`, filled
     * in by the drivers above, no longer decides anything here. */
    if (rc == 0) {
        int wr = wa_write_new(path, out, buf, fsize);
        if (wr != 0) {
            fprintf(stderr, "ERROR: %s not written\n", out);
            rc = MR_FAIL;
        } else {
            printf("Wrote %s (%zu bytes)\n", out, fsize);
        }
    }

    free(buf);
    return rc;
}
