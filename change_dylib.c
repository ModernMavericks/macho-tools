/*
 * Rewrite LC_LOAD_DYLIB and LC_RPATH paths.
 *
 * By default the new load commands must fit in the header padding between the
 * last load command and the first section's file data; if they don't, the tool
 * fails (unchanged behavior). Pass -grow to opt in to enlarging that padding
 * first (see macho_grow.h) — that resize only works on a PIE executable and is
 * rejected otherwise.
 *
 * Usage: change_dylib input [-grow] [-change old new] [-delete path]
 *                     [-reexport path] [-add path] [-insert path]
 *                     [-strip-lc name] [-change-rpath old new]
 *                     [-delete-rpath path] [-add-rpath path]...
 *
 * -strip-lc drops a whole load command by kind (uuid, codesig, source-version,
 * build-version, code-sign-drs). It reclaims header padding without moving any
 * file data, so unlike -grow it never disturbs image-base-relative structures.
 * Prefer it when a longer path needs a few more bytes.
 *
 * -add appends a brand-new LC_LOAD_DYLIB naming `path` (combine with -grow to
 * guarantee header room). Used to bake a dependency — e.g. libavxemu.dylib —
 * into the binary itself, so only that binary loads it (not children inheriting
 * DYLD_INSERT_LIBRARIES). See HEADER_PAD_GROWTH.md.
 *
 * -insert is -add's front-loading twin: it places the new LC_LOAD_DYLIB *before*
 * every existing one, which makes dyld load and INITIALIZE it first. That matters
 * when the injected library has to be live before anything else runs a
 * constructor — an emulator installing a SIGILL handler, say.
 *
 * The -*-rpath forms do the same three operations on LC_RPATH. A binary whose
 * @rpath/ dependencies must resolve somewhere new needs its search paths moved
 * as well as its load paths, and the two travel together often enough that
 * splitting them across two tools is a nuisance. LC_RPATH carries no library
 * ordinal, so unlike the dylib commands it can be added or dropped freely.
 *
 * LIBRARY ORDINALS. In a two-level-namespace image every undefined symbol
 * records which dylib it comes from, as a 1-based index into the dylib load
 * commands in load order. The index lives in two places: the nlist n_desc of
 * each undefined symbol, and the SET_DYLIB_ORDINAL opcodes of the LC_DYLD_INFO
 * bind/weak/lazy streams. Appending (-add) is safe because it only hands out new
 * indices, but INSERTING or DELETING shifts every later one.
 *
 * Leaving them stale does not produce a subtle bug so much as an unloadable
 * binary: the highest ordinal usually belongs to libSystem (dyld_stub_binder),
 * so after a deletion dyld rejects the image with "library ordinal (N) too big".
 * Where the shifted index does stay in range it is worse, because it silently
 * names a different library. Either way the rewrite has to renumber, so -insert
 * and -delete do, and -delete refuses outright if any symbol still binds to the
 * dylib being removed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include "image.h"
#include "macho_grow.h"
#include "ordinals.h"
#include "fat.h"
#include "lc_kinds.h"
#include "atomic_write.h"
#include "mach_compat.h"
#include <mach-o/fat.h>

/* The -strip-lc KIND vocabulary lives in src/lc_kinds.c now, shared with
 * cli/macho9.c's `lc -delete` and its --capabilities output -- see that
 * file's comment for why. `strippable` was this table's name here before;
 * kept as a local alias so the rest of this file (and its usage text) don't
 * all need renaming for a table that hasn't changed shape. */
#define strippable LC_STRIP_KINDS

/* Caps on how many times one option may repeat. Each option accumulates into a
 * fixed-size array; nothing reads a length back, so an unchecked write past the
 * end corrupts whatever follows instead of failing. Check every one. */
#define CD_MAX_OPS   32
#define CD_MAX_STRIP 16
#define CD_ROOM(n, max, flag)                                            \
    do {                                                                 \
        if ((n) == (max)) {                                              \
            fprintf(stderr, "too many %s (max %d)\n", (flag), (max));    \
            return 1;                                                    \
        }                                                                \
    } while (0)

struct change {
    const char *old_path;
    const char *new_path;   /* NULL = delete; "" = in-place, no path change */
    int reexport;           /* 1 = promote LC_LOAD_DYLIB -> LC_REEXPORT_DYLIB */
};

/* mo_map_build's is_deleted callback: true if `name` matches a -delete in
 * `changes`. This is the SAME test build_lcs uses (via `matched`/`new_path ==
 * NULL`) to decide which LC_LOAD_DYLIB commands to drop, which is what keeps
 * the load-command rewrite and the ordinal map from being able to disagree
 * about which dylib went away. */
struct ord_delete_ctx { const struct change *changes; int n; };
static int ord_is_deleted(const char *name, void *ctx_) {
    const struct ord_delete_ctx *ctx = ctx_;
    for (int c = 0; c < ctx->n; c++)
        if (ctx->changes[c].new_path == NULL && strcmp(name, ctx->changes[c].old_path) == 0)
            return 1;
    return 0;
}

/* Emit one LC_LOAD_DYLIB naming `path` at `dst`; returns its cmdsize. */
static uint32_t emit_dylib_lc(uint8_t *dst, const char *path) {
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

/* build_lcs's per-load-command work, as an mi_each_lc callback. Every mutable
 * local the old hand-rolled loop threaded through each iteration (new_off,
 * ncmds, mods, placed_inserts) lives in this ctx instead; adds/radds are NOT
 * here because build_lcs only needs them AFTER the walk (appended past the
 * existing table), so they never had to be part of the per-command state.
 *
 * Returns 0 to continue, 1 to stop the walk -- the two cases below where a
 * dylib or LC_RPATH command's own name offset is out of bounds for its
 * cmdsize (mo_lc_str_at, ordinals.h). Stopping here is exactly what the
 * stop-capable mi_each_lc exists for: without it, the walk would keep
 * calling this callback for every later load command after the refusal
 * fires, each one still writing into new_lcs -- corrupting/overrunning a
 * buffer build_lcs's caller believes was never touched, instead of leaving
 * the input provably unmodified the way a refusal here must. */
struct build_lcs_ctx {
    const struct change *changes; int nchanges;
    const char *const *inserts; int ninserts;
    const uint32_t *strip; int nstrip;
    const struct change *rchanges; int nrchanges;
    uint8_t *new_lcs;
    uint32_t new_off;
    uint32_t ncmds;
    int mods;
    int placed_inserts;
    int verbose;
};

static int build_lcs_lc(const struct load_command *lc, void *ctx_) {
    struct build_lcs_ctx *ctx = ctx_;
    uint32_t cmdsize = lc->cmdsize;
    uint32_t write_size = cmdsize;
    int matched = -1;
    int deleted = 0;

    /* Dropping a command reclaims its bytes for the rest of the table.
     * Any __LINKEDIT payload it referenced simply stops being reachable;
     * nothing moves, so no offset anywhere needs fixing up. */
    int stripped = 0;
    for (int s = 0; s < ctx->nstrip; s++)
        if (lc->cmd == ctx->strip[s]) { stripped = 1; break; }
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
    if (!ctx->placed_inserts && ctx->ninserts && mo_is_ordinal_lc(lc->cmd)) {
        for (int s = 0; s < ctx->ninserts; s++) {
            uint32_t cs = emit_dylib_lc(ctx->new_lcs + ctx->new_off, ctx->inserts[s]);
            ctx->new_off += cs;
            ctx->ncmds++;
            ctx->mods++;
            if (ctx->verbose) printf("  Insert [%u bytes]: LC_LOAD_DYLIB %s (now ordinal %d)\n",
                                cs, ctx->inserts[s], s + 1);
        }
        ctx->placed_inserts = 1;
    }

    /* mo_is_ordinal_lc() here (rather than a locally re-listed set) is
     * what keeps this "which dylib LCs can be matched/renamed/deleted"
     * set in sync with mo_map_build's "which dylib LCs carry an ordinal"
     * set -- they used to disagree about LC_LOAD_UPWARD_DYLIB. LC_ID_DYLIB
     * is added back in because it names the image itself: it has to be
     * recognized as dylib-shaped so `dc`/`name` below are valid, but it's
     * excluded from matching just below, same as before. */
    if (mo_is_ordinal_lc(lc->cmd) || lc->cmd == LC_ID_DYLIB) {
        const struct dylib_command *dc = (const struct dylib_command *)lc;
        if (lc->cmd != LC_ID_DYLIB) {  /* never rewrite this dylib's own identity */
            const char *name = mo_lc_str_at(lc, dc->dylib.name.offset);
            if (!name) {
                fprintf(stderr, "ERROR: malformed dylib load command (name offset %u "
                                "exceeds cmdsize %u); refusing\n",
                        dc->dylib.name.offset, cmdsize);
                return 1;
            }
            for (int c = 0; c < ctx->nchanges; c++)
                if (strcmp(name, ctx->changes[c].old_path) == 0) { matched = c; break; }
            /* Deletion is decided by ord_is_deleted -- the SAME predicate
             * passed to mo_map_build below -- not by whichever `changes`
             * entry happens to match first. Without this, a path named
             * in both a -change and a -delete could be kept here while
             * mo_map_build's map (which scans every -delete, not just the
             * first match) marks it gone: exactly the "renumberer and
             * emitter disagree" bug this module exists to rule out. A
             * -delete anywhere in the arguments now always wins, no
             * matter where it falls relative to a conflicting -change. */
            deleted = ord_is_deleted(name, &(struct ord_delete_ctx){ ctx->changes, ctx->nchanges });
        }
        if (!deleted && matched >= 0 && ctx->changes[matched].new_path != NULL) {
            size_t base = dc->dylib.name.offset;
            size_t new_len = strlen(ctx->changes[matched].new_path) + 1;
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
        for (int c = 0; c < ctx->nrchanges; c++)
            if (strcmp(rp, ctx->rchanges[c].old_path) == 0) { rmatched = c; break; }
        if (rmatched >= 0 && ctx->rchanges[rmatched].new_path != NULL) {
            size_t base = rc->path.offset;
            size_t new_len = strlen(ctx->rchanges[rmatched].new_path) + 1;
            uint32_t needed = (uint32_t)((base + new_len + 7) & ~7UL);
            if (needed < cmdsize) needed = cmdsize;
            write_size = needed;
        }
    }

    if (rmatched >= 0) {
        if (ctx->rchanges[rmatched].new_path == NULL) {
            if (ctx->verbose) printf("  Delete rpath [%u bytes]: %s\n", cmdsize,
                                ctx->rchanges[rmatched].old_path);
            ctx->ncmds--;
        } else {
            memcpy(ctx->new_lcs + ctx->new_off, lc, cmdsize);
            struct rpath_command *nrc = (struct rpath_command *)(ctx->new_lcs + ctx->new_off);
            nrc->cmdsize = write_size;
            size_t base = nrc->path.offset;
            memset(ctx->new_lcs + ctx->new_off + base, 0, write_size - base);
            strcpy((char *)(ctx->new_lcs + ctx->new_off + base), ctx->rchanges[rmatched].new_path);
            if (ctx->verbose)
                printf("  Change rpath [%u->%u bytes]: %s -> %s\n", cmdsize, write_size,
                       ctx->rchanges[rmatched].old_path, ctx->rchanges[rmatched].new_path);
            ctx->new_off += write_size;
        }
        ctx->mods++;
    } else if (deleted) {
        if (ctx->verbose) printf("  Delete [%u bytes]: %s\n", cmdsize, ctx->changes[matched].old_path);
        ctx->ncmds--;
        ctx->mods++;
    } else {
        memcpy(ctx->new_lcs + ctx->new_off, lc, cmdsize);
        if (matched >= 0) {
            struct dylib_command *ndc = (struct dylib_command *)(ctx->new_lcs + ctx->new_off);
            ndc->cmdsize = write_size;
            if (ctx->changes[matched].reexport) {
                ndc->cmd = LC_REEXPORT_DYLIB;
                if (ctx->verbose) printf("  Reexport: %s\n", ctx->changes[matched].old_path);
            }
            if (ctx->changes[matched].new_path[0] != '\0') {
                size_t base = ndc->dylib.name.offset;
                memset(ctx->new_lcs + ctx->new_off + base, 0, write_size - base);
                strcpy((char *)(ctx->new_lcs + ctx->new_off + base), ctx->changes[matched].new_path);
                if (ctx->verbose)
                    printf("  Change [%u->%u bytes]: %s -> %s\n", cmdsize, write_size,
                           ctx->changes[matched].old_path, ctx->changes[matched].new_path);
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
 * over the relocated buffer -- see process_one's second call site). `verbose`
 * prints the per-change diagnostics once.
 *
 * Returns 0 on success, -1 (message already on stderr, via build_lcs_lc) if a
 * dylib or LC_RPATH command's name offset is out of bounds for its own
 * cmdsize -- see mo_lc_str_at (ordinals.h). out_off/out_ncmds/out_mods are
 * unspecified on failure; the caller must not use them. The per-command work
 * is build_lcs_lc, walked via the stop-capable mi_each_lc so that refusal
 * can abort before writing another byte into new_lcs; everything below
 * (placing not-yet-placed inserts, then -add/-add-rpath) runs only once, in
 * whatever state the walk left ncmds/new_off/mods, so it stays hand-written
 * here rather than folded into the per-command callback. */
static int build_lcs(const mi_image *im, const struct change *changes, int nchanges,
                      const char *const *adds, int nadds,
                      const char *const *inserts, int ninserts,
                      const uint32_t *strip, int nstrip,
                      const struct change *rchanges, int nrchanges,
                      const char *const *radds, int nradds,
                      uint8_t *new_lcs, uint32_t *out_off, uint32_t *out_ncmds,
                      int *out_mods, int verbose) {
    struct build_lcs_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.changes = changes;   ctx.nchanges = nchanges;
    ctx.inserts = inserts;   ctx.ninserts = ninserts;
    ctx.strip = strip;       ctx.nstrip = nstrip;
    ctx.rchanges = rchanges; ctx.nrchanges = nrchanges;
    ctx.new_lcs = new_lcs;
    ctx.ncmds = im->hdr->ncmds;
    ctx.verbose = verbose;

    if (!mi_each_lc(im, build_lcs_lc, &ctx)) return -1;   /* refused; see build_lcs_lc */

    uint32_t new_off = ctx.new_off, ncmds = ctx.ncmds;
    int mods = ctx.mods;

    /* An image with no dylib load commands at all still honours -insert; there
     * was simply nothing to insert in front of. */
    if (!ctx.placed_inserts) {
        for (int s = 0; s < ninserts; s++) {
            uint32_t cs = emit_dylib_lc(new_lcs + new_off, inserts[s]);
            new_off += cs;
            ncmds++;
            mods++;
            if (verbose) printf("  Insert [%u bytes]: LC_LOAD_DYLIB %s\n", cs, inserts[s]);
        }
    }

    /* Append brand-new LC_LOAD_DYLIB commands (-add). Appending is ordinal-safe:
     * it only hands out indices past the existing ones. */
    for (int a = 0; a < nadds; a++) {
        uint32_t cs = emit_dylib_lc(new_lcs + new_off, adds[a]);
        new_off += cs;
        ncmds++;
        mods++;
        if (verbose) printf("  Add [%u bytes]: LC_LOAD_DYLIB %s\n", cs, adds[a]);
    }

    /* Append brand-new LC_RPATH commands (-add-rpath): an rpath_command
     * followed by the NUL-terminated path, padded to 8 bytes. */
    for (int a = 0; a < nradds; a++) {
        size_t plen = strlen(radds[a]) + 1;
        uint32_t cs = (uint32_t)((sizeof(struct rpath_command) + plen + 7) & ~7UL);
        struct rpath_command *nrc = (struct rpath_command *)(new_lcs + new_off);
        memset(nrc, 0, cs);
        nrc->cmd = LC_RPATH;
        nrc->cmdsize = cs;
        nrc->path.offset = sizeof(struct rpath_command);
        strcpy((char *)nrc + sizeof(struct rpath_command), radds[a]);
        new_off += cs;
        ncmds++;
        mods++;
        if (verbose) printf("  Add [%u bytes]: LC_RPATH %s\n", cs, radds[a]);
    }

    *out_off = new_off;
    *out_ncmds = ncmds;
    *out_mods = mods;
    return 0;
}

struct cgb_ctx {
    uint32_t growth;
    const struct change *changes;  int nchanges;
    const struct change *rchanges; int nrchanges;
};

static int cgb_lc(const struct load_command *lc, void *ctx_) {
    struct cgb_ctx *ctx = ctx_;
    uint32_t cmdsize = lc->cmdsize;

    if (mo_is_ordinal_lc(lc->cmd)) {
        const struct dylib_command *dc = (const struct dylib_command *)lc;
        const char *name = mo_lc_str_at(lc, dc->dylib.name.offset);
        if (name) {
            for (int c = 0; c < ctx->nchanges; c++) {
                if (strcmp(name, ctx->changes[c].old_path) != 0) continue;
                if (ctx->changes[c].new_path != NULL) {
                    size_t base = dc->dylib.name.offset;
                    size_t new_len = strlen(ctx->changes[c].new_path) + 1;
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
            for (int c = 0; c < ctx->nrchanges; c++) {
                if (strcmp(rp, ctx->rchanges[c].old_path) != 0) continue;
                if (ctx->rchanges[c].new_path != NULL) {
                    size_t base = rc->path.offset;
                    size_t new_len = strlen(ctx->rchanges[c].new_path) + 1;
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
 * build_lcs's -change/-change-rpath growth will write beyond each matched
 * command's ORIGINAL cmdsize -- computed by walking the real load commands
 * in `im` the same way build_lcs's own matching loop does, instead of
 * assuming "one grown command per -change/-change-rpath argument".
 *
 * That assumption was the bug: more than one load command can carry the same
 * install name (or rpath), and build_lcs grows EVERY command that matches, so
 * a binary with two LC_LOAD_DYLIBs naming the same library and a single
 * -change for it needs budget for two grown commands, not one. Reproduced
 * with two synthetic LC_LOAD_DYLIBs sharing an install name and a -change
 * whose replacement path is ~9000 chars: the old per-argument budget sized
 * new_lcs for one growth, build_lcs wrote two, and the second write ran past
 * the allocation -- a heap overflow confirmed under libgmalloc (SIGSEGV; without
 * it, memory corruption with exit 1).
 *
 * This performs the identical sizing build_lcs performs -- round8(base +
 * strlen(new_path) + 1), kept only if it exceeds the original cmdsize -- over
 * every matching command rather than once per argument, and mirrors
 * build_lcs's own "first match in `changes`/`rchanges` wins" rule so it
 * agrees with what build_lcs will actually do for the ordinary case where a
 * name is named once. It is not bit-exact in one edge case: if the SAME old
 * path appears in both a -change and a separate -delete, build_lcs's
 * `ord_is_deleted` check (not visible to a single first-match walk) makes
 * that command a deletion with no growth, while this still counts it as
 * growing. That only over-budgets -- harmless slack in new_lcs -- never
 * under-budgets, which is the property that matters here. Writes nothing;
 * reads bounds-checked via mo_lc_str_at, and silently does not count a
 * malformed command as a match -- build_lcs performs the same check and
 * refuses the whole operation before it would ever act on that command, so
 * excluding it here cannot lead to writing past what was budgeted.
 *
 * Pure read-only accumulation into one running total, unlike build_lcs's own
 * walk just below (which writes a whole new load-command table and threads
 * several more locals through the loop) -- that is what makes this one a
 * plain mi_each_lc conversion and build_lcs not, see its comment.
 */
static uint32_t change_growth_bytes(const mi_image *im,
                                     const struct change *changes, int nchanges,
                                     const struct change *rchanges, int nrchanges) {
    struct cgb_ctx ctx = { 0, changes, nchanges, rchanges, nrchanges };
    mi_each_lc(im, cgb_lc, &ctx);
    return ctx.growth;
}

/* process_one's return codes. PO_SKIP is not an error: it means `label` is not
 * a (recognizable) 64-bit Mach-O, so this rewriter has nothing to do to it --
 * the caller's job is to leave those bytes exactly as it found them. That is
 * what lets a fat binary carrying a slice this tool cannot understand (32-bit,
 * or any other format) still get its OTHER slices rewritten, matching how
 * fix_macho's per-arch loop already treats an unrecognized slice: skip it,
 * don't fail the whole file. PO_ERROR is a real failure -- the label WAS a
 * 64-bit Mach-O but the requested edit could not be made -- and the caller
 * must treat that as fatal to the whole operation (see main()'s fat path):
 * partially rewriting a multi-arch binary would leave its slices disagreeing
 * about the edit, which is worse than refusing outright. */
#define PO_SKIP  (-2)
#define PO_ERROR (-1)

/*
 * Apply every requested change to the single (thin) 64-bit Mach-O in
 * *pbuf, *pfsize, in place except that mg_grow_header may realloc *pbuf (its
 * usual contract: on success *pbuf, *pfsize are updated to the new buffer/size
 * and the caller owns it; on failure of the grow itself the caller still owns
 * whatever *pbuf now points to). `label` names this image only for diagnostic
 * printf's -- a file path for the thin case, an "arch N (cputype ...)" string
 * for a fat slice.
 *
 * Returns PO_SKIP if *pbuf is not a 64-bit Mach-O at all (buffer untouched),
 * PO_ERROR if it is one but the edit failed (message already printed on
 * stderr; buffer contents are unspecified beyond "still the caller's to
 * free"), or 0 on success with *out_modified reporting whether anything
 * actually changed.
 */
static int process_one(uint8_t **pbuf, size_t *pfsize, const char *label,
                        const struct change *changes, int nchanges,
                        const char *const *adds, int nadds,
                        const char *const *inserts, int ninserts,
                        const uint32_t *strip, int nstrip,
                        const struct change *rchanges, int nrchanges,
                        const char *const *radds, int nradds,
                        int allow_grow, int *out_modified) {
    *out_modified = 0;
    uint8_t *buf = *pbuf;
    size_t fsize = *pfsize;

    mi_image im;
    if (mi_wrap(buf, fsize, &im) != 0) return PO_SKIP;
    struct mach_header_64 *hdr = im.hdr;
    /* mi_wrap never allocates or takes ownership (im.owned == 0), so there is
     * nothing to release here -- buf/fsize above already ARE the buffer. */

    uint32_t first_sect_off = mg_first_sect_off(buf, fsize);
    if (first_sect_off == UINT32_MAX) {
        fprintf(stderr, "ERROR: %s fails validation; refusing (see above)\n", label);
        return PO_ERROR;
    }
    uint32_t cur_lc_end = sizeof(struct mach_header_64) + hdr->sizeofcmds;
    uint32_t pad_avail = first_sect_off > cur_lc_end ? first_sect_off - cur_lc_end : 0;
    printf("%s: header pad %u bytes available (LC end=%u, first sect=%u)\n",
           label, pad_avail, cur_lc_end, first_sect_off);

    /* Upper bound on bytes the -add/-insert commands contribute, so the scratch
     * buffer can hold the full new table even before the header pad is grown. */
    uint32_t add_bytes = 0;
    for (int a = 0; a < nadds; a++)
        add_bytes += (uint32_t)((sizeof(struct dylib_command) + strlen(adds[a]) + 1 + 7) & ~7UL);
    for (int s = 0; s < ninserts; s++)
        add_bytes += (uint32_t)((sizeof(struct dylib_command) + strlen(inserts[s]) + 1 + 7) & ~7UL);
    for (int a = 0; a < nradds; a++)
        add_bytes += (uint32_t)((sizeof(struct rpath_command) + strlen(radds[a]) + 1 + 7) & ~7UL);
    /* -change/-change-rpath can ALSO grow a command past its original
     * cmdsize -- build_lcs's `matched`/`rmatched` branches size the rewritten
     * command as (base + strlen(new_path) + 1), rounded up, keeping whichever
     * is larger of that or the original cmdsize (see the "if (needed <
     * cmdsize) needed = cmdsize;" lines in build_lcs). The real bound is
     * "sum, over every EXISTING LOAD COMMAND that will actually match, of
     * that command's own growth" -- not "one grown command per -change
     * argument". A single -change argument can match more than one load
     * command (two LC_LOAD_DYLIBs can legitimately carry the same install
     * name), and each one grows independently, so a per-argument budget
     * undercounts whenever that happens: this was a real heap buffer
     * overflow (confirmed under libgmalloc: SIGSEGV; without libgmalloc,
     * silent corruption then exit 1), reproduced with two synthetic
     * LC_LOAD_DYLIBs sharing an install name and a -change whose replacement
     * path is ~9000 chars. change_growth_bytes computes the real bound by
     * walking the actual load commands the same way build_lcs's matching
     * loop does -- see its own comment for the one (safe, over- not
     * under-) approximation it still makes. */
    add_bytes += change_growth_bytes(&im, changes, nchanges, rchanges, nrchanges);

    /* Map each existing 1-based library ordinal to its new value (0 = deleted),
     * built once by mo_map_build so this rewrite and the ordinal renumbering
     * below (mo_map_apply) can't independently disagree about which dylib
     * landed where -- see ordinals.h. Inserts take 1..ninserts, so every
     * survivor shifts up by that much; each deletion shifts the ones after it
     * back down. */
    int ord_map[MO_MAX_DYLIBS + 1];
    memset(ord_map, 0, sizeof ord_map);
    mo_map omap = { ord_map, 0 };
    struct ord_delete_ctx dctx = { changes, nchanges };
    int nnew;
    if (mo_map_build(buf, hdr->ncmds, ninserts, ord_is_deleted, &dctx, &omap, &nnew) != 0)
        return PO_ERROR;
    int nold = omap.n;
    /* A survivor count below nold means at least one dylib was deleted; that
     * and any -insert are the only reasons a rewrite needs to renumber. */
    int needs_renumber = (ninserts > 0) || (nnew - ninserts < nold);
    if (nnew + nadds > MO_MAX_DYLIBS) {
        fprintf(stderr, "ERROR: %s: result would exceed %d dylibs\n", label, MO_MAX_DYLIBS);
        return PO_ERROR;
    }

    /* Build the new table once to learn its size (and print diagnostics). */
    uint8_t *new_lcs = calloc(1, first_sect_off + add_bytes + 64);
    uint32_t new_off, new_ncmds; int modifications;
    if (build_lcs(&im, changes, nchanges, adds, nadds, inserts, ninserts, strip, nstrip,
                   rchanges, nrchanges, radds, nradds,
                   new_lcs, &new_off, &new_ncmds, &modifications, 1) != 0) {
        free(new_lcs);
        return PO_ERROR;
    }

    if (modifications == 0) { printf("%s: nothing to change.\n", label); free(new_lcs); return 0; }

    /* The new table must fit before the first section's data. The boundary is
     * sizeof(mach_header_64) + sizeofcmds; using new_off alone would understate
     * it by the 32-byte header and allow a 16-byte overlap into the section. */
    uint32_t need_end = (uint32_t)sizeof(struct mach_header_64) + new_off;
    if (need_end > first_sect_off) {
        if (!allow_grow) {
            /* Default, unchanged behavior: refuse rather than resize. */
            fprintf(stderr, "ERROR: %s: new LCs (%u bytes) don't fit in header pad (%u avail); "
                            "pass -grow to enlarge it\n", label, new_off, pad_avail);
            free(new_lcs);
            return PO_ERROR;
        }
        uint32_t grow_req = need_end - first_sect_off;
        printf("%s: load commands need %u more bytes than the %u-byte pad; growing header...\n",
               label, grow_req, pad_avail);
        if (mg_grow_header(&buf, &fsize, grow_req) != 0) {
            fprintf(stderr, "ERROR: %s: new LCs (%u bytes) don't fit and header could not be grown\n",
                    label, new_off);
            *pbuf = buf; *pfsize = fsize;
            free(new_lcs);
            return PO_ERROR;
        }
        *pbuf = buf; *pfsize = fsize;   /* mg_grow_header may have realloc'd */
        hdr = (struct mach_header_64 *)buf;
        first_sect_off = mg_first_sect_off(buf, fsize);
        if (first_sect_off == UINT32_MAX) {
            fprintf(stderr, "ERROR: %s: header grow produced an image that fails validation\n", label);
            free(new_lcs);
            return PO_ERROR;
        }
        printf("%s: grew header pad: first sect now at %u (%u bytes available)\n",
               label, first_sect_off, first_sect_off - cur_lc_end);
        /* mg_grow_header reallocs the raw buffer, not through image.h, so the
         * `im` wrapped at the top of this function is stale here (it still
         * points at whatever `buf` was before the realloc). Re-wrap it over
         * the relocated buffer -- mi_wrap never allocates or frees (im.owned
         * stays 0), so overwriting `im` in place is safe, and this re-wrap
         * cannot fail in practice: mg_first_sect_off just above ran the same
         * mi_wrap validation against this exact buf/fsize and already
         * returned success. Handled defensively anyway, same as every other
         * "provably unreachable, checked anyway" spot in this codebase. */
        if (mi_wrap(buf, fsize, &im) != 0) {
            fprintf(stderr, "ERROR: %s: grown header fails validation; refusing\n", label);
            free(new_lcs);
            return PO_ERROR;
        }
        /* Rebuild against the relocated header so segment/linkedit offsets in
         * the copied load commands reflect the shift. */
        free(new_lcs);
        new_lcs = calloc(1, first_sect_off + add_bytes + 64);
        if (build_lcs(&im, changes, nchanges, adds, nadds, inserts, ninserts, strip, nstrip,
                       rchanges, nrchanges, radds, nradds,
                       new_lcs, &new_off, &new_ncmds, &modifications, 0) != 0) {
            free(new_lcs);
            return PO_ERROR;
        }
    }

    /* Check the map against what build_lcs actually emitted -- not just
     * against its own arithmetic -- before committing anything. This is the
     * cross-check that catches the map and the load-command rewrite having
     * independently disagreed about which dylib survived, which the map's
     * own internal consistency (checked inside mo_map_build) cannot: that
     * only proves the map is self-consistent, not that it matches reality. */
    if (mo_map_validate(&omap, ninserts, nnew, nadds, new_lcs, new_ncmds) != 0) {
        fprintf(stderr, "ERROR: %s left unmodified\n", label);
        free(new_lcs);
        return PO_ERROR;
    }

    /* Commit: zero the whole LC area, write the new table, fix up the header. */
    memset(buf + sizeof(struct mach_header_64), 0, first_sect_off - sizeof(struct mach_header_64));
    memcpy(buf + sizeof(struct mach_header_64), new_lcs, new_off);
    hdr->ncmds = new_ncmds;
    hdr->sizeofcmds = new_off;
    free(new_lcs);

    /* Ordinals last, against the committed table — and before any write, so a
     * refusal leaves the input untouched rather than half-rewritten. */
    if (needs_renumber && mo_map_apply(buf, fsize, &omap, 1) != 0) {
        fprintf(stderr, "ERROR: %s left unmodified\n", label);
        return PO_ERROR;
    }

    /* Last gate before the bytes reach disk. change_dylib is the FINAL stage of
     * the wrapper's chain (patch_macho -> add_version_min -> change_dylib), so a
     * check here covers the cumulative end state of all of them -- including
     * patch_macho's chained-fixups conversion, which has ~94,900 rebases and no
     * self-check of its own. It needs no "before" image, which is what makes it
     * usable across process boundaries.
     *
     * This is the difference between "binary replaced, re-download that version"
     * and "patch refused, nothing lost". MACHO_NO_VERIFY=1 opts out. */
    if (!getenv("MACHO_NO_VERIFY") && mg_plausible(buf, fsize) != 0) {
        fprintf(stderr, "ERROR: refusing to modify %s -- it would carry base-relative "
                        "offsets that name no known function. Left unmodified.\n", label);
        return PO_ERROR;
    }

    *pbuf = buf; *pfsize = fsize;
    *out_modified = 1;
    printf("%s: updated (sizeofcmds=%u, %zu bytes)\n", label, new_off, fsize);
    return 0;
}

static uint32_t cd_swap32(uint32_t v) {
    return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) |
           ((v & 0xff0000u) >> 8) | ((v >> 24) & 0xffu);
}

/*
 * Apply every requested change to every slice of a fat (universal) binary in
 * *pbuf, *pfsize, reassembling the fat container afterward. This is what
 * closes the fat gap in the rewrite path: fix_macho already walks fat/thin,
 * change_dylib until now only understood thin.
 *
 * A slice this tool cannot understand (anything process_one reports PO_SKIP
 * for -- today that means anything but a 64-bit Mach-O; 32-bit stays
 * deliberately unsupported, see macho_grow.h) is passed through byte-for-byte
 * unchanged, exactly like fix_macho's own per-arch loop already does ("Not
 * 64-bit Mach-O ... Skipping arch"). A slice that IS a 64-bit Mach-O but
 * where the requested edit itself fails (PO_ERROR) aborts the WHOLE
 * operation: a fat binary's slices are all meant to carry the same edit
 * (the same -change, the same -insert, ...), and writing some of them but
 * not others would leave the result internally inconsistent -- worse than
 * refusing outright, and not what a "delete succeeded, or nothing was
 * touched" contract can allow. See process_one's own comment for why PO_SKIP
 * and PO_ERROR need different treatment.
 *
 * Sizes: without -grow, or when growth was not needed, every slice keeps its
 * original size, and this reassembly places every slice back at its ORIGINAL
 * file offset -- so a fat binary edited without size changes ends up with
 * exactly the same layout it started with. Only once some earlier slice's
 * size actually changes does a later slice's offset get recomputed, packed
 * tightly against the slice before it at that slice's own (preserved)
 * alignment. That is what keeps an unmodified multi-arch binary's on-disk
 * shape untouched while still supporting the resize -grow needs.
 */
static int process_fat(uint8_t **pbuf, size_t *pfsize,
                        const struct change *changes, int nchanges,
                        const char *const *adds, int nadds,
                        const char *const *inserts, int ninserts,
                        const uint32_t *strip, int nstrip,
                        const struct change *rchanges, int nrchanges,
                        const char *const *radds, int nradds,
                        int allow_grow, int *out_modified) {
    *out_modified = 0;
    uint8_t *buf = *pbuf;
    size_t fsize = *pfsize;

    /* mfat_parse (src/fat.c) is the ONE place both change_dylib and
     * fix_macho validate a fat file's arch table -- magic, the table fitting
     * inside the file, every entry's offset+size in bounds and not
     * overlapping the header/table region itself, AND no two declared
     * slices overlapping EACH OTHER (a fat file whose own arch table already
     * aliases two slices is malformed on the read side, before this rewrite
     * ever computes a single new offset). Before this, each tool had its own
     * hand-rolled walk and they disagreed about validation (fix_macho
     * trusted an arch's offset/size outright); see fat.h's file header for
     * the fuller story. */
    uint32_t narch; int swap;
    if (mfat_parse(buf, fsize, &narch, &swap) != 0) {
        fprintf(stderr, "ERROR: malformed fat file (bad magic, arch table past the end, "
                        "a slice overlapping the header, or two slices overlapping "
                        "each other)\n");
        return 1;
    }

    /* Per-slice working state, gathered up front so a mid-loop failure can
     * free exactly what has been allocated so far. */
    uint8_t **sbuf   = calloc(narch, sizeof(uint8_t *));
    size_t   *ssize  = calloc(narch, sizeof(size_t));
    uint64_t *ooff   = calloc(narch, sizeof(uint64_t));
    uint64_t *osize  = calloc(narch, sizeof(uint64_t));
    uint32_t *cputype = calloc(narch, sizeof(uint32_t));
    uint32_t *cpusubtype = calloc(narch, sizeof(uint32_t));
    uint32_t *align  = calloc(narch, sizeof(uint32_t));
    if (narch && (!sbuf || !ssize || !ooff || !osize || !cputype || !cpusubtype || !align)) {
        fprintf(stderr, "ERROR: out of memory\n");
        free(sbuf); free(ssize); free(ooff); free(osize);
        free(cputype); free(cpusubtype); free(align);
        return 1;
    }

    int aborted = 0;
    uint32_t i;
    for (i = 0; i < narch; i++) {
        /* mfat_parse above already proved offset+size is in bounds and
         * outside the header/table region for every entry up to narch, so
         * mfat_get needs no further checking here. */
        mfat_arch a;
        mfat_get(buf, swap, i, &a);
        uint32_t o = a.offset, s = a.size, ct = a.cputype, cs = a.cpusubtype, al = a.align;
        ooff[i] = o; osize[i] = s;
        cputype[i] = ct; cpusubtype[i] = cs; align[i] = al;

        sbuf[i] = malloc(s ? s : 1);
        if (!sbuf[i]) { fprintf(stderr, "ERROR: out of memory\n"); aborted = 1; break; }
        memcpy(sbuf[i], buf + o, s);
        ssize[i] = s;

        char label[64];
        snprintf(label, sizeof label, "arch %u (cputype 0x%x)", i, ct);

        int mod = 0;
        int rc = process_one(&sbuf[i], &ssize[i], label, changes, nchanges,
                              adds, nadds, inserts, ninserts, strip, nstrip,
                              rchanges, nrchanges, radds, nradds, allow_grow, &mod);
        if (rc == PO_SKIP) {
            printf("%s: not a 64-bit Mach-O; leaving this slice unchanged\n", label);
            /* sbuf[i]/ssize[i] already hold the untouched original bytes. */
        } else if (rc == PO_ERROR) {
            fprintf(stderr, "ERROR: %s: refusing the whole fat file -- a partial "
                            "rewrite would leave its slices inconsistent\n", label);
            i++;   /* this slice's buffer was still allocated; free it too */
            aborted = 1;
            break;
        } else if (mod) {
            *out_modified = 1;
        }
    }

    if (aborted) {
        for (uint32_t j = 0; j < i; j++) free(sbuf[j]);
        free(sbuf); free(ssize); free(ooff); free(osize);
        free(cputype); free(cpusubtype); free(align);
        return 1;
    }

    if (!*out_modified) {
        for (uint32_t j = 0; j < narch; j++) free(sbuf[j]);
        free(sbuf); free(ssize); free(ooff); free(osize);
        free(cputype); free(cpusubtype); free(align);
        printf("Nothing to change.\n");
        return 0;
    }

    /* Reassemble: each slice keeps its original offset until some earlier
     * slice's size actually changed; from then on later slices pack
     * sequentially, honoring each slice's own (preserved) alignment.
     *
     * `cursor` tracks where the NEXT slice may start, which only means
     * "the end of the file" when the arch table happens to be in ascending
     * offset order -- nothing in the fat format requires that (lipo merely
     * happens to emit it that way). A fat file with, say, arch[0] at a
     * HIGHER offset than arch[1] is legal and both entries can independently
     * pass the offset+size-in-bounds check in mfat_parse. Sizing the output
     * buffer from `cursor` (the LAST slice processed) instead of the
     * MAXIMUM end across every slice undersizes the allocation whenever the
     * table isn't ascending, and the memcpy below then writes past it --
     * heap corruption in the best case, and an exit-0 write of a truncated,
     * silently-corrupted file in the worst, since main() would then write
     * exactly `newbuf`'s (too-small) size back over the real input. Track
     * the true maximum explicitly so the allocation is never smaller than
     * every slice it has to hold, regardless of table order. */
    uint64_t *noff = calloc(narch, sizeof(uint64_t));
    int shift = 0;
    uint64_t cursor = 0;
    uint64_t max_end = 0;
    for (uint32_t j = 0; j < narch; j++) {
        uint64_t want;
        if (!shift) {
            want = ooff[j];
        } else {
            uint32_t shift_amt = align[j] > 31 ? 31 : align[j];  /* hostile input guard */
            uint64_t a = (uint64_t)1 << shift_amt;
            want = (cursor + a - 1) & ~(a - 1);
        }
        noff[j] = want;
        cursor = want + ssize[j];
        if (cursor > max_end) max_end = cursor;
        if (ssize[j] != osize[j]) shift = 1;
    }

    /* Refuse rather than guess: an unshifted slice keeps its ORIGINAL offset
     * unconditionally (see above), but a later, SHIFTED slice's sequential
     * packing has no idea where that still-fixed slice sits -- on a
     * non-ascending table it can walk a shifted slice's new range right on
     * top of a still-fixed one's. That is silent data loss with an exit 0
     * (the final memcpy below would just overwrite one slice's bytes with
     * another's) -- exactly the failure class the previous fix closed the
     * memory-safety half of; this closes the correctness half. Check every
     * pair -- not just neighbors in table order, since the colliding pair
     * need not be adjacent -- BEFORE allocating or writing anything, so a
     * refusal here leaves the input completely untouched. */
    for (uint32_t a = 0; a < narch; a++) {
        uint64_t a0 = noff[a], a1 = a0 + ssize[a];
        for (uint32_t b = a + 1; b < narch; b++) {
            uint64_t b0 = noff[b], b1 = b0 + ssize[b];
            if (a0 < b1 && b0 < a1) {
                fprintf(stderr, "ERROR: reassembly would place arch %u [%llu,%llu) and "
                                "arch %u [%llu,%llu) at overlapping offsets; refusing "
                                "rather than guess a different layout\n",
                        a, (unsigned long long)a0, (unsigned long long)a1,
                        b, (unsigned long long)b0, (unsigned long long)b1);
                for (uint32_t k = 0; k < narch; k++) free(sbuf[k]);
                free(sbuf); free(ssize); free(ooff); free(osize);
                free(cputype); free(cpusubtype); free(align); free(noff);
                return 1;
            }
        }
    }

    uint8_t *newbuf = calloc(1, (size_t)max_end);
    if (!newbuf) {
        fprintf(stderr, "ERROR: out of memory reassembling the fat file\n");
        for (uint32_t j = 0; j < narch; j++) free(sbuf[j]);
        free(sbuf); free(ssize); free(ooff); free(osize);
        free(cputype); free(cpusubtype); free(align); free(noff);
        return 1;
    }
    struct fat_header *nfh = (struct fat_header *)newbuf;
    nfh->magic = swap ? cd_swap32(FAT_MAGIC) : FAT_MAGIC;
    nfh->nfat_arch = swap ? cd_swap32(narch) : narch;
    struct fat_arch *nar = (struct fat_arch *)(newbuf + sizeof(struct fat_header));
    for (uint32_t j = 0; j < narch; j++) {
        uint32_t o = (uint32_t)noff[j], s = (uint32_t)ssize[j];
        nar[j].cputype    = swap ? (cpu_type_t)cd_swap32((uint32_t)cputype[j]) : (cpu_type_t)cputype[j];
        nar[j].cpusubtype = swap ? (cpu_subtype_t)cd_swap32((uint32_t)cpusubtype[j]) : (cpu_subtype_t)cpusubtype[j];
        nar[j].offset = swap ? cd_swap32(o) : o;
        nar[j].size   = swap ? cd_swap32(s) : s;
        nar[j].align  = swap ? cd_swap32(align[j]) : align[j];
        memcpy(newbuf + noff[j], sbuf[j], ssize[j]);
        printf("arch %u: placed at %llu (%llu bytes)\n", j,
               (unsigned long long)noff[j], (unsigned long long)ssize[j]);
    }

    for (uint32_t j = 0; j < narch; j++) free(sbuf[j]);
    free(sbuf); free(ssize); free(ooff); free(osize);
    free(cputype); free(cpusubtype); free(align); free(noff);

    free(buf);
    *pbuf = newbuf;
    *pfsize = (size_t)max_end;   /* NOT cursor -- see the comment above the alloc */
    return 0;
}

/* copy_xattrs, write_in_place and write_atomic moved to src/atomic_write.c
 * (wa_copy_xattrs / wa_write_in_place / wa_write_atomic) so `macho9 grow`
 * can share this exact logic instead of writing its result via a plain
 * ftruncate()+write() of its own -- see atomic_write.h. */

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s input [-grow] [-change old new] [-delete path] "
                        "[-reexport path] [-add path] [-insert path] "
                        "[-strip-lc name] [-change-rpath old new] "
                        "[-delete-rpath path] [-add-rpath path] ...\n", argv[0]);
        fprintf(stderr, "  -strip-lc kinds:");
        for (size_t k = 0; k < LC_STRIP_KINDS_COUNT; k++)
            fprintf(stderr, " %s", strippable[k].name);
        fprintf(stderr, "\n");
        return 1;
    }
    const char *path = argv[1];

    struct change changes[CD_MAX_OPS];
    int nchanges = 0;
    const char *adds[CD_MAX_OPS];
    int nadds = 0;
    const char *inserts[CD_MAX_OPS];
    int ninserts = 0;
    struct change rchanges[CD_MAX_OPS];
    int nrchanges = 0;
    const char *radds[CD_MAX_OPS];
    int nradds = 0;
    int allow_grow = 0;
    uint32_t strip[CD_MAX_STRIP];
    int nstrip = 0;
    for (int i = 2; i < argc; ) {
        if (strcmp(argv[i], "-grow") == 0) {
            allow_grow = 1;
            i += 1;
        } else if (strcmp(argv[i], "-strip-lc") == 0 && i + 1 < argc) {
            size_t k, nk = LC_STRIP_KINDS_COUNT;
            for (k = 0; k < nk; k++)
                if (strcmp(argv[i+1], strippable[k].name) == 0) break;
            if (k == nk) { fprintf(stderr, "unknown -strip-lc kind: %s\n", argv[i+1]); return 1; }
            CD_ROOM(nstrip, CD_MAX_STRIP, "-strip-lc");
            strip[nstrip++] = strippable[k].cmd;
            i += 2;
        } else if (strcmp(argv[i], "-add") == 0 && i + 1 < argc) {
            CD_ROOM(nadds, CD_MAX_OPS, "-add");
            adds[nadds++] = argv[i+1];
            i += 2;
        } else if (strcmp(argv[i], "-insert") == 0 && i + 1 < argc) {
            CD_ROOM(ninserts, CD_MAX_OPS, "-insert");
            inserts[ninserts++] = argv[i+1];
            i += 2;
        } else if (strcmp(argv[i], "-change") == 0 && i + 2 < argc) {
            CD_ROOM(nchanges, CD_MAX_OPS, "-change");
            changes[nchanges].old_path = argv[i+1];
            changes[nchanges].new_path = argv[i+2];
            changes[nchanges].reexport = 0;
            nchanges++;
            i += 3;
        } else if (strcmp(argv[i], "-delete") == 0 && i + 1 < argc) {
            CD_ROOM(nchanges, CD_MAX_OPS, "-delete");
            changes[nchanges].old_path = argv[i+1];
            changes[nchanges].new_path = NULL;
            changes[nchanges].reexport = 0;
            nchanges++;
            i += 2;
        } else if (strcmp(argv[i], "-reexport") == 0 && i + 1 < argc) {
            CD_ROOM(nchanges, CD_MAX_OPS, "-reexport");
            changes[nchanges].old_path = argv[i+1];
            changes[nchanges].new_path = "";
            changes[nchanges].reexport = 1;
            nchanges++;
            i += 2;
        } else if (strcmp(argv[i], "-add-rpath") == 0 && i + 1 < argc) {
            CD_ROOM(nradds, CD_MAX_OPS, "-add-rpath");
            radds[nradds++] = argv[i+1];
            i += 2;
        } else if (strcmp(argv[i], "-change-rpath") == 0 && i + 2 < argc) {
            CD_ROOM(nrchanges, CD_MAX_OPS, "-change-rpath");
            rchanges[nrchanges].old_path = argv[i+1];
            rchanges[nrchanges].new_path = argv[i+2];
            rchanges[nrchanges].reexport = 0;
            nrchanges++;
            i += 3;
        } else if (strcmp(argv[i], "-delete-rpath") == 0 && i + 1 < argc) {
            CD_ROOM(nrchanges, CD_MAX_OPS, "-delete-rpath");
            rchanges[nrchanges].old_path = argv[i+1];
            rchanges[nrchanges].new_path = NULL;
            rchanges[nrchanges].reexport = 0;
            nrchanges++;
            i += 2;
        } else { fprintf(stderr, "bad arg: %s\n", argv[i]); return 1; }
    }

    /* The O_RDWR fd is opened up front, as before -- that ordering is
     * load-bearing: it is what makes an unwritable file fail immediately
     * instead of after all the analysis has run and printed. It is no
     * longer HELD for the write-back (write_atomic() below replaces `path`
     * via a temp file + rename rather than writing through this fd
     * directly), and -- restoring the mi_open/mi_release split this tool
     * used before it grew fat support (see a41263d) -- it is no longer used
     * for the THIN read either: only to learn the size/mode and to peek the
     * magic, since a fat file's magic isn't MH_MAGIC_64 and mi_open (thin
     * only) would refuse it outright. This is the one place that has to
     * tell fat from thin apart before choosing how to read the rest. */
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 1; }
    if (st.st_size < 4) {
        fprintf(stderr, "%s: too small to be a Mach-O\n", path);
        close(fd);
        return 1;
    }
    mode_t orig_mode = st.st_mode;

    uint32_t magic;
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, &magic, sizeof magic) != (ssize_t)sizeof magic) {
        perror("read"); close(fd); return 1;
    }

    uint8_t *buf;
    size_t fsize;
    int modified = 0;
    int rc;

    /* A 64-bit fat container (fat_arch_64 -- wide offsets, used for arm64e /
     * watchOS-style slices) genuinely IS a Mach-O; this tool just doesn't
     * speak that variant, only the classic 32-bit-offset fat_arch one. Say
     * so explicitly rather than falling through to the thin path's "not a
     * readable 64-bit Mach-O", which reads as "this isn't Mach-O at all". */
    if (magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64) {
        fprintf(stderr, "%s: 64-bit fat Mach-O (fat_arch_64); not supported -- only the "
                        "32-bit-offset fat_arch container is\n", path);
        close(fd);
        return 1;
    }

    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        /* Fat: mi_open only understands a thin 64-bit Mach-O, so this is the
         * one shape it cannot serve -- read the raw bytes ourselves.
         * mfat_parse (inside process_fat) does this format's own
         * validation. */
        fsize = (size_t)st.st_size;
        buf = (uint8_t *)malloc(fsize);
        if (!buf) { fprintf(stderr, "out of memory\n"); close(fd); return 1; }
        if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, buf, fsize) != (ssize_t)fsize) {
            perror("read"); close(fd); free(buf); return 1;
        }
        close(fd);
        rc = process_fat(&buf, &fsize, changes, nchanges, adds, nadds,
                          inserts, ninserts, strip, nstrip, rchanges, nrchanges,
                          radds, nradds, allow_grow, &modified);
    } else {
        /* Thin (or not a Mach-O at all): mi_open does the actual read and
         * full validation -- cmdsize bounds/alignment and LC_SEGMENT_64/
         * nsects agreement, none of which the magic-only peek above looked
         * at. mi_release hands this function ownership of the buffer,
         * needed because mg_grow_header (inside process_one, via -grow)
         * reallocs it -- an mi_image left pointing at the old allocation
         * would be a dangling pointer waiting for a mi_close that never
         * comes. */
        close(fd);
        mi_image im;
        if (mi_open(path, &im) != 0) {
            /* mi_open reports pass/fail only -- on failure "*out is
             * untouched and nothing is allocated" (its own contract), so
             * there is no buffer here to inspect for WHY. Reconstruct the
             * three-way too-short/bad-magic/malformed diagnostic this tool
             * has always given from what's already in hand instead: st.st_size
             * (the real file size, from the fstat above) and magic (the
             * 4-byte peek above -- valid here since the fat-magic branch
             * above already ruled out both fat magics). */
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
            return 1;
        }
        fsize = im.size;
        buf = mi_release(&im);

        int po = process_one(&buf, &fsize, path, changes, nchanges, adds, nadds,
                              inserts, ninserts, strip, nstrip, rchanges, nrchanges,
                              radds, nradds, allow_grow, &modified);
        if (po == PO_SKIP) {
            /* Unreachable in practice: mi_open above already validated this
             * exact buffer with the identical algorithm process_one's own
             * mi_wrap runs on it, so mi_wrap cannot disagree. Kept as a
             * defensive fallback only -- the detailed diagnostic this branch
             * used to give now lives at the mi_open failure site above,
             * where it is actually reachable. */
            fprintf(stderr, "%s: not a 64-bit Mach-O (rejected during processing)\n", path);
            rc = 1;
        } else {
            rc = (po == PO_ERROR) ? 1 : 0;
        }
    }

    if (rc == 0 && modified) {
        if (wa_write_atomic(path, orig_mode, buf, fsize) != 0) {
            fprintf(stderr, "ERROR: %s left unmodified (atomic replace failed)\n", path);
            rc = 1;
        } else {
            printf("Updated %s (%zu bytes)\n", path, fsize);
        }
    }

    free(buf);
    return rc;
}
