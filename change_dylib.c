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
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include "image.h"
#include "macho_grow.h"
#include "ordinals.h"
#include "fat.h"
#include "lc_kinds.h"
#include <mach-o/fat.h>

/* The -strip-lc KIND vocabulary lives in src/lc_kinds.c now, shared with
 * cli/macho9.c's `lc -delete` and its --capabilities output -- see that
 * file's comment for why. `strippable` was this table's name here before;
 * kept as a local alias so the rest of this file (and its usage text) don't
 * all need renaming for a table that hasn't changed shape. */
#define strippable LC_STRIP_KINDS

#ifndef LC_LOAD_UPWARD_DYLIB
#define LC_LOAD_UPWARD_DYLIB (0x23 | LC_REQ_DYLD)
#endif

/* The 10.9 SDK's <mach-o/fat.h> predates the 64-bit fat container (wide
 * offsets, for arm64e/watchOS-style slices with a component that overflows
 * 32 bits) and does not define these -- so they are not conditional on
 * anything this codebase controls, only on which SDK headers happened to be
 * on the include path. Values match every SDK that DOES define them. */
#ifndef FAT_MAGIC_64
#define FAT_MAGIC_64 0xcafebabfu
#endif
#ifndef FAT_CIGAM_64
#define FAT_CIGAM_64 0xbfbafecau
#endif

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

/*
 * Build the new load-command table into `new_lcs` from the current header in
 * `buf`. Returns the new total size (sizeofcmds) via *out_off, the new command
 * count via *out_ncmds, and how many changes applied via *out_mods. Does NOT
 * mutate the header, so it is safe to call more than once (e.g. again after the
 * header pad has been grown). `verbose` prints the per-change diagnostics once.
 */
static void build_lcs(const uint8_t *buf, const struct change *changes, int nchanges,
                      const char *const *adds, int nadds,
                      const char *const *inserts, int ninserts,
                      const uint32_t *strip, int nstrip,
                      const struct change *rchanges, int nrchanges,
                      const char *const *radds, int nradds,
                      uint8_t *new_lcs, uint32_t *out_off, uint32_t *out_ncmds,
                      int *out_mods, int verbose) {
    const struct mach_header_64 *hdr = (const struct mach_header_64 *)buf;
    uint32_t new_off = 0, ncmds = hdr->ncmds;
    int mods = 0, placed_inserts = 0;

    const uint8_t *lcp = buf + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)lcp;
        uint32_t cmdsize = lc->cmdsize;
        uint32_t write_size = cmdsize;
        int matched = -1;
        int deleted = 0;

        /* Dropping a command reclaims its bytes for the rest of the table.
         * Any __LINKEDIT payload it referenced simply stops being reachable;
         * nothing moves, so no offset anywhere needs fixing up. */
        int stripped = 0;
        for (int s = 0; s < nstrip; s++)
            if (lc->cmd == strip[s]) { stripped = 1; break; }
        if (stripped) {
            if (verbose) printf("  Strip [%u bytes]: load command 0x%x\n", cmdsize, lc->cmd);
            ncmds--;
            mods++;
            lcp += cmdsize;
            continue;
        }

        /* -insert goes immediately before the first ordinal-bearing dylib LC, so
         * the inserted libraries become ordinals 1..n and load (and initialize)
         * ahead of everything the image already depended on. Nothing strippable
         * bears an ordinal, so the strip pass above cannot move this boundary. */
        if (!placed_inserts && ninserts && mo_is_ordinal_lc(lc->cmd)) {
            for (int s = 0; s < ninserts; s++) {
                uint32_t cs = emit_dylib_lc(new_lcs + new_off, inserts[s]);
                new_off += cs;
                ncmds++;
                mods++;
                if (verbose) printf("  Insert [%u bytes]: LC_LOAD_DYLIB %s (now ordinal %d)\n",
                                    cs, inserts[s], s + 1);
            }
            placed_inserts = 1;
        }

        /* mo_is_ordinal_lc() here (rather than a locally re-listed set) is
         * what keeps this "which dylib LCs can be matched/renamed/deleted"
         * set in sync with mo_map_build's "which dylib LCs carry an ordinal"
         * set -- they used to disagree about LC_LOAD_UPWARD_DYLIB. LC_ID_DYLIB
         * is added back in because it names the image itself: it has to be
         * recognized as dylib-shaped so `dc`/`name` below are valid, but it's
         * excluded from matching just below, same as before. */
        if (mo_is_ordinal_lc(lc->cmd) || lc->cmd == LC_ID_DYLIB) {
            const struct dylib_command *dc = (const struct dylib_command *)lcp;
            const char *name = (const char *)lcp + dc->dylib.name.offset;
            if (lc->cmd != LC_ID_DYLIB) {  /* never rewrite this dylib's own identity */
                for (int c = 0; c < nchanges; c++)
                    if (strcmp(name, changes[c].old_path) == 0) { matched = c; break; }
                /* Deletion is decided by ord_is_deleted -- the SAME predicate
                 * passed to mo_map_build below -- not by whichever `changes`
                 * entry happens to match first. Without this, a path named
                 * in both a -change and a -delete could be kept here while
                 * mo_map_build's map (which scans every -delete, not just the
                 * first match) marks it gone: exactly the "renumberer and
                 * emitter disagree" bug this module exists to rule out. A
                 * -delete anywhere in the arguments now always wins, no
                 * matter where it falls relative to a conflicting -change. */
                deleted = ord_is_deleted(name, &(struct ord_delete_ctx){ changes, nchanges });
            }
            if (!deleted && matched >= 0 && changes[matched].new_path != NULL) {
                size_t base = dc->dylib.name.offset;
                size_t new_len = strlen(changes[matched].new_path) + 1;
                uint32_t needed = (uint32_t)((base + new_len + 7) & ~7UL);
                if (needed < cmdsize) needed = cmdsize;
                write_size = needed;
            }
        }

        /* LC_RPATH carries a single lc_str exactly like a dylib command, so
         * the same grow-the-command-and-rewrite-in-place logic applies. */
        int rmatched = -1;
        if (lc->cmd == LC_RPATH) {
            const struct rpath_command *rc = (const struct rpath_command *)lcp;
            const char *rp = (const char *)lcp + rc->path.offset;
            for (int c = 0; c < nrchanges; c++)
                if (strcmp(rp, rchanges[c].old_path) == 0) { rmatched = c; break; }
            if (rmatched >= 0 && rchanges[rmatched].new_path != NULL) {
                size_t base = rc->path.offset;
                size_t new_len = strlen(rchanges[rmatched].new_path) + 1;
                uint32_t needed = (uint32_t)((base + new_len + 7) & ~7UL);
                if (needed < cmdsize) needed = cmdsize;
                write_size = needed;
            }
        }

        if (rmatched >= 0) {
            if (rchanges[rmatched].new_path == NULL) {
                if (verbose) printf("  Delete rpath [%u bytes]: %s\n", cmdsize,
                                    rchanges[rmatched].old_path);
                ncmds--;
            } else {
                memcpy(new_lcs + new_off, lcp, cmdsize);
                struct rpath_command *nrc = (struct rpath_command *)(new_lcs + new_off);
                nrc->cmdsize = write_size;
                size_t base = nrc->path.offset;
                memset(new_lcs + new_off + base, 0, write_size - base);
                strcpy((char *)(new_lcs + new_off + base), rchanges[rmatched].new_path);
                if (verbose)
                    printf("  Change rpath [%u->%u bytes]: %s -> %s\n", cmdsize, write_size,
                           rchanges[rmatched].old_path, rchanges[rmatched].new_path);
                new_off += write_size;
            }
            mods++;
        } else if (deleted) {
            if (verbose) printf("  Delete [%u bytes]: %s\n", cmdsize, changes[matched].old_path);
            ncmds--;
            mods++;
        } else {
            memcpy(new_lcs + new_off, lcp, cmdsize);
            if (matched >= 0) {
                struct dylib_command *ndc = (struct dylib_command *)(new_lcs + new_off);
                ndc->cmdsize = write_size;
                if (changes[matched].reexport) {
                    ndc->cmd = LC_REEXPORT_DYLIB;
                    if (verbose) printf("  Reexport: %s\n", changes[matched].old_path);
                }
                if (changes[matched].new_path[0] != '\0') {
                    size_t base = ndc->dylib.name.offset;
                    memset(new_lcs + new_off + base, 0, write_size - base);
                    strcpy((char *)(new_lcs + new_off + base), changes[matched].new_path);
                    if (verbose)
                        printf("  Change [%u->%u bytes]: %s -> %s\n", cmdsize, write_size,
                               changes[matched].old_path, changes[matched].new_path);
                }
                mods++;
            }
            new_off += write_size;
        }
        lcp += cmdsize;
    }

    /* An image with no dylib load commands at all still honours -insert; there
     * was simply nothing to insert in front of. */
    if (!placed_inserts) {
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
     * cmdsize) needed = cmdsize;" lines below). Before this, add_bytes never
     * accounted for that growth at all: a long enough -change replacement
     * (repro: `-change /usr/lib/libSystem.B.dylib` with a ~9000-char new
     * path) made build_lcs write well past the end of a buffer sized only
     * for the -add/-insert commands, a heap buffer overflow (confirmed
     * under libgmalloc: SIGSEGV). This doesn't know here which existing
     * command a given -change will match (that's decided later, by name,
     * inside build_lcs) or that command's actual `base` (dc->dylib.name.
     * offset / rc->path.offset), so it bounds it the same way -add above
     * does: as if the match grew a brand-new, full-size
     * dylib_command/rpath_command header plus the new path -- at least as
     * large as `base + new_len` can ever be for a well-formed command,
     * whatever the match turns out to be (or if it turns out not to match
     * anything at all, in which case this is simply unused slack).
     *
     * NOT a worst-case bound, though: this budgets ONE grown command per
     * `-change`/`-change-rpath` TERM, but the matching loop above grows
     * every LOAD COMMAND that matches -- one term can match more than one
     * command. A binary carrying the same install name (or rpath) on two or
     * more load commands and a single `-change`/`-change-rpath` for it will
     * still under-budget add_bytes and can overflow new_lcs, same class of
     * bug as the one this comment used to describe. Unhandled; not fixed
     * here. */
    for (int c = 0; c < nchanges; c++)
        if (changes[c].new_path != NULL && changes[c].new_path[0] != '\0')
            add_bytes += (uint32_t)((sizeof(struct dylib_command) + strlen(changes[c].new_path) + 1 + 7) & ~7UL);
    for (int c = 0; c < nrchanges; c++)
        if (rchanges[c].new_path != NULL)
            add_bytes += (uint32_t)((sizeof(struct rpath_command) + strlen(rchanges[c].new_path) + 1 + 7) & ~7UL);

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
    build_lcs(buf, changes, nchanges, adds, nadds, inserts, ninserts, strip, nstrip,
              rchanges, nrchanges, radds, nradds,
              new_lcs, &new_off, &new_ncmds, &modifications, 1);

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
        /* Rebuild against the relocated header so segment/linkedit offsets in
         * the copied load commands reflect the shift. */
        free(new_lcs);
        new_lcs = calloc(1, first_sect_off + add_bytes + 64);
        build_lcs(buf, changes, nchanges, adds, nadds, inserts, ninserts, strip, nstrip,
                  rchanges, nrchanges, radds, nradds,
                  new_lcs, &new_off, &new_ncmds, &modifications, 0);
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
    if (needs_renumber && mo_map_apply(buf, &omap, 1) != 0) {
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

/* Copy every extended attribute from `src_path` onto the open descriptor
 * `dst_fd`. 10.9's <sys/xattr.h> has no fd-to-path or fd-to-fd copy call
 * (that's a Sierra-and-later copyfile(3) feature), so this is
 * listxattr+getxattr+fsetxattr by hand. Best-effort in the sense that it
 * keeps going past a single attribute's failure to try the rest, but it
 * DOES report failure to the caller -- silently dropping quarantine et al.
 * is exactly the bug being fixed here, so a failure is surfaced as a
 * warning rather than swallowed. A source with no xattr support at all
 * (ENOTSUP/ENOENT from the initial listxattr) is not an error. */
static int copy_xattrs(const char *src_path, int dst_fd) {
    ssize_t listlen = listxattr(src_path, NULL, 0, 0);
    if (listlen < 0) return (errno == ENOTSUP || errno == ENOENT) ? 0 : -1;
    if (listlen == 0) return 0;

    char *names = (char *)malloc((size_t)listlen);
    if (!names) return -1;
    ssize_t got = listxattr(src_path, names, (size_t)listlen, 0);
    if (got < 0) { free(names); return -1; }

    int rc = 0;
    for (ssize_t off = 0; off < got; ) {
        const char *name = names + off;
        off += (ssize_t)strlen(name) + 1;

        ssize_t vlen = getxattr(src_path, name, NULL, 0, 0, 0);
        if (vlen < 0) { rc = -1; continue; }
        void *val = NULL;
        if (vlen > 0) {
            val = malloc((size_t)vlen);
            if (!val) { rc = -1; continue; }
            ssize_t got2 = getxattr(src_path, name, val, (size_t)vlen, 0, 0);
            if (got2 < 0) { free(val); rc = -1; continue; }
            vlen = got2;
        }
        if (fsetxattr(dst_fd, name, val, (size_t)vlen, 0, 0) != 0) rc = -1;
        free(val);
    }
    free(names);
    return rc;
}

/* Write `size` bytes of `buf` directly into the file `path` resolves to, in
 * place: ftruncate() then write(), the sequence this codebase used before
 * write_atomic() existed (see the comment on write_atomic for why it is
 * still needed for hard-linked files). open() follows both symlinks and
 * hard links to the one underlying inode, so this updates every name for
 * the file at once and needs no xattr/owner/ACL copying -- nothing new was
 * created. The cost is the atomicity write_atomic()'s ordinary path buys:
 * a write failing partway (disk full, killed mid-write, ...) leaves `path`
 * truncated with only part of the new content in it. */
static int write_in_place(const char *path, const uint8_t *buf, size_t size) {
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (ftruncate(fd, (off_t)size) != 0) { perror("ftruncate"); close(fd); return 1; }

    size_t off = 0;
    int failed = 0;
    while (off < size) {
        ssize_t n = write(fd, buf + off, size - off);
        if (n < 0) { perror("write"); failed = 1; break; }
        off += (size_t)n;
    }
    if (!failed && fsync(fd) != 0) { perror("fsync"); failed = 1; }
    close(fd);
    return failed ? 1 : 0;
}

/* Write `size` bytes of `buf` as the new content of the file `path` refers
 * to. Two strategies, chosen by link count:
 *
 * ORDINARY CASE (the common one: a single hard link, `path` possibly a
 * symlink to it): atomic mkstemp()+rename(). `path` is realpath()'d FIRST
 * so the rename lands on the real target, never on `path` itself -- if
 * `path` is a symlink, replacing it via rename (the previous, buggy
 * behavior) turned it into a plain file and left the real target, and
 * everything else that follows the same symlink, unpatched. macOS
 * framework dylibs are exactly this shape (Foo.framework/Foo ->
 * Versions/A/Foo) and are a primary target of this toolkit. The temp file
 * is created in the resolved target's directory, so the rename stays on
 * one filesystem and is therefore atomic, and every xattr on the original
 * (quarantine, etc.) is copied onto it before the rename via copy_xattrs().
 * Either the OLD content (and its xattrs) is still there afterward or the
 * NEW content (and copied xattrs) is, in full -- never a half-written or
 * truncated file.
 *
 * HARD-LINK CASE (st_nlink > 1): rename() would give the resolved path a
 * FRESH inode, leaving every other name for that inode -- the sibling hard
 * links -- pointing at the old, unpatched content (and the new inode with
 * none of the original's xattrs/owner/ACL). There's no atomic way to
 * update every name for an inode at once, so this falls back to
 * write_in_place(), which writes through the existing inode -- see its
 * comment for the atomicity this gives up in exchange. */
static int write_atomic(const char *path, mode_t mode, const uint8_t *buf, size_t size) {
    char real[PATH_MAX];
    const char *target = (realpath(path, real) != NULL) ? real : path;

    struct stat tst;
    int have_stat = (stat(target, &tst) == 0);
    if (have_stat && tst.st_nlink > 1) {
        return write_in_place(target, buf, size);
    }

    size_t tlen = strlen(target) + 8;
    char *tmpl = (char *)malloc(tlen);
    if (!tmpl) { fprintf(stderr, "out of memory\n"); return 1; }
    snprintf(tmpl, tlen, "%s.XXXXXX", target);

    int tfd = mkstemp(tmpl);
    if (tfd < 0) { perror("mkstemp"); free(tmpl); return 1; }
    fchmod(tfd, mode);   /* best-effort: match the original file's permissions */
    /* tst is only valid when the stat above succeeded -- an uninitialized
     * st_uid/st_gid must never reach fchown. main() only ever gets here
     * having already opened `path` O_RDWR, so this stat cannot realistically
     * fail; the guard exists for defined behavior, not because failure is
     * expected in practice. */
    if (have_stat) fchown(tfd, tst.st_uid, tst.st_gid);   /* best-effort: needs privilege to change owner */
    if (copy_xattrs(target, tfd) != 0) {
        fprintf(stderr, "warning: %s: could not copy all extended attributes "
                        "(e.g. com.apple.quarantine) to the updated file\n", target);
    }
    /* NOTE: ACLs (acl_get_file/acl_set_file) are not copied. 10.9 has the
     * API to do so, but nothing in this toolkit's current call sites (CI
     * artifacts, build-tree binaries) sets ACLs on Mach-O files, so it has
     * not been implemented -- flagging here rather than silently doing
     * less than the comment above claims. */

    size_t off = 0;
    int failed = 0;
    while (off < size) {
        ssize_t n = write(tfd, buf + off, size - off);
        if (n < 0) { perror("write"); failed = 1; break; }
        off += (size_t)n;
    }
    if (!failed && fsync(tfd) != 0) { perror("fsync"); failed = 1; }
    close(tfd);

    if (failed || rename(tmpl, target) != 0) {
        if (!failed) perror("rename");
        unlink(tmpl);
        free(tmpl);
        return 1;
    }
    free(tmpl);
    return 0;
}

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
     * longer HELD for the write-back, though: write_atomic() below replaces
     * `path` via a temp file + rename rather than writing through this fd
     * directly, so it is closed as soon as the input has been read. */
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 1; }
    if (st.st_size < 4) {
        fprintf(stderr, "%s: too small to be a Mach-O\n", path);
        close(fd);
        return 1;
    }
    size_t fsize = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(fsize);
    if (!buf) { fprintf(stderr, "out of memory\n"); close(fd); return 1; }
    if (read(fd, buf, fsize) != (ssize_t)fsize) {
        perror("read"); close(fd); free(buf); return 1;
    }
    mode_t orig_mode = st.st_mode;
    close(fd);

    /* Read the raw bytes ourselves (rather than mi_open) because a fat file's
     * magic isn't MH_MAGIC_64 -- mi_open would refuse it outright, and this
     * is the one place that has to tell "fat" from "thin" apart before
     * either mi_wrap (thin, inside process_one) or the fat_header/fat_arch
     * walk (process_fat) can run. */
    uint32_t magic = *(uint32_t *)buf;
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
        free(buf);
        return 1;
    }

    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        rc = process_fat(&buf, &fsize, changes, nchanges, adds, nadds,
                          inserts, ninserts, strip, nstrip, rchanges, nrchanges,
                          radds, nradds, allow_grow, &modified);
    } else {
        int po = process_one(&buf, &fsize, path, changes, nchanges, adds, nadds,
                              inserts, ninserts, strip, nstrip, rchanges, nrchanges,
                              radds, nradds, allow_grow, &modified);
        if (po == PO_SKIP) {
            fprintf(stderr, "%s: not a readable 64-bit Mach-O\n", path);
            rc = 1;
        } else {
            rc = (po == PO_ERROR) ? 1 : 0;
        }
    }

    if (rc == 0 && modified) {
        if (write_atomic(path, orig_mode, buf, fsize) != 0) {
            fprintf(stderr, "ERROR: %s left unmodified (atomic replace failed)\n", path);
            rc = 1;
        } else {
            printf("Updated %s (%zu bytes)\n", path, fsize);
        }
    }

    free(buf);
    return rc;
}
