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
#include <sys/stat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include "image.h"
#include "macho_grow.h"
#include "ordinals.h"

/* Load commands safe to drop: purely informational, or invalidated the moment
 * the binary is rewritten. Deliberately excludes LC_FUNCTION_STARTS (avxemu
 * reads it for patch-safety bounds) and LC_DATA_IN_CODE. None of them carries
 * a library ordinal, so stripping never disturbs the renumbering below. */
#ifndef LC_SOURCE_VERSION
#define LC_SOURCE_VERSION 0x2A
#endif
#ifndef LC_BUILD_VERSION
#define LC_BUILD_VERSION 0x32
#endif
#ifndef LC_DYLIB_CODE_SIGN_DRS
#define LC_DYLIB_CODE_SIGN_DRS 0x2B
#endif
static const struct { const char *name; uint32_t cmd; } strippable[] = {
    { "uuid",           LC_UUID                },
    { "codesig",        LC_CODE_SIGNATURE      },
    { "source-version", LC_SOURCE_VERSION      },
    { "build-version",  LC_BUILD_VERSION       },
    { "code-sign-drs",  LC_DYLIB_CODE_SIGN_DRS },
};

#ifndef LC_LOAD_UPWARD_DYLIB
#define LC_LOAD_UPWARD_DYLIB (0x23 | LC_REQ_DYLD)
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

        if (lc->cmd == LC_LOAD_DYLIB || lc->cmd == LC_LOAD_WEAK_DYLIB ||
            lc->cmd == LC_ID_DYLIB || lc->cmd == LC_REEXPORT_DYLIB) {
            const struct dylib_command *dc = (const struct dylib_command *)lcp;
            const char *name = (const char *)lcp + dc->dylib.name.offset;
            if (lc->cmd != LC_ID_DYLIB) {  /* never rewrite this dylib's own identity */
                for (int c = 0; c < nchanges; c++)
                    if (strcmp(name, changes[c].old_path) == 0) { matched = c; break; }
            }
            if (matched >= 0 && changes[matched].new_path != NULL) {
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
        } else if (matched >= 0 && changes[matched].new_path == NULL) {
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

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s input [-grow] [-change old new] [-delete path] "
                        "[-reexport path] [-add path] [-insert path] "
                        "[-strip-lc name] [-change-rpath old new] "
                        "[-delete-rpath path] [-add-rpath path] ...\n", argv[0]);
        fprintf(stderr, "  -strip-lc kinds:");
        for (size_t k = 0; k < sizeof(strippable)/sizeof(strippable[0]); k++)
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
            size_t k, nk = sizeof(strippable)/sizeof(strippable[0]);
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

    /* The O_RDWR fd is opened up front, as before, and held for the write-back
     * at the end. That ordering is load-bearing: it is what makes an unwritable
     * file fail immediately instead of after all the analysis has run and
     * printed. mi_open reads and validates separately. */
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    /* mi_release, not the image, owns the buffer from here: mg_grow_header
     * below reallocs it, which would leave an mi_image dangling. The hand-off
     * is explicit so the ownership is readable rather than implied. */
    mi_image im;
    if (mi_open(path, &im) != 0) {
        fprintf(stderr, "%s: not a readable 64-bit Mach-O\n", path);
        close(fd);
        return 1;
    }
    size_t fsize = im.size;
    struct mach_header_64 *hdr = im.hdr;
    uint8_t *buf = mi_release(&im);

    uint32_t first_sect_off = mg_first_sect_off(buf, fsize);
    if (first_sect_off == UINT32_MAX) {
        fprintf(stderr, "ERROR: %s fails validation; refusing (see above)\n", path);
        return 1;
    }
    uint32_t cur_lc_end = sizeof(struct mach_header_64) + hdr->sizeofcmds;
    uint32_t pad_avail = first_sect_off > cur_lc_end ? first_sect_off - cur_lc_end : 0;
    printf("Header pad: %u bytes available (LC end=%u, first sect=%u)\n",
           pad_avail, cur_lc_end, first_sect_off);

    /* Upper bound on bytes the -add/-insert commands contribute, so the scratch
     * buffer can hold the full new table even before the header pad is grown. */
    uint32_t add_bytes = 0;
    for (int a = 0; a < nadds; a++)
        add_bytes += (uint32_t)((sizeof(struct dylib_command) + strlen(adds[a]) + 1 + 7) & ~7UL);
    for (int s = 0; s < ninserts; s++)
        add_bytes += (uint32_t)((sizeof(struct dylib_command) + strlen(inserts[s]) + 1 + 7) & ~7UL);
    for (int a = 0; a < nradds; a++)
        add_bytes += (uint32_t)((sizeof(struct rpath_command) + strlen(radds[a]) + 1 + 7) & ~7UL);

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
        return 1;
    int nold = omap.n;
    /* A survivor count below nold means at least one dylib was deleted; that
     * and any -insert are the only reasons a rewrite needs to renumber. */
    int needs_renumber = (ninserts > 0) || (nnew - ninserts < nold);
    if (mo_map_validate(&omap, nnew) != 0) {
        fprintf(stderr, "ERROR: internal: ordinal map failed validation\n");
        return 1;
    }
    if (nnew + nadds > MO_MAX_DYLIBS) {
        fprintf(stderr, "ERROR: result would exceed %d dylibs\n", MO_MAX_DYLIBS);
        return 1;
    }

    /* Build the new table once to learn its size (and print diagnostics). */
    uint8_t *new_lcs = calloc(1, first_sect_off + add_bytes + 64);
    uint32_t new_off, new_ncmds; int modifications;
    build_lcs(buf, changes, nchanges, adds, nadds, inserts, ninserts, strip, nstrip,
              rchanges, nrchanges, radds, nradds,
              new_lcs, &new_off, &new_ncmds, &modifications, 1);

    if (modifications == 0) { printf("Nothing to change.\n"); return 0; }

    /* The new table must fit before the first section's data. The boundary is
     * sizeof(mach_header_64) + sizeofcmds; using new_off alone would understate
     * it by the 32-byte header and allow a 16-byte overlap into the section. */
    uint32_t need_end = (uint32_t)sizeof(struct mach_header_64) + new_off;
    if (need_end > first_sect_off) {
        if (!allow_grow) {
            /* Default, unchanged behavior: refuse rather than resize. */
            fprintf(stderr, "ERROR: new LCs (%u bytes) don't fit in header pad (%u avail); "
                            "pass -grow to enlarge it\n", new_off, pad_avail);
            return 1;
        }
        uint32_t grow_req = need_end - first_sect_off;
        printf("Load commands need %u more bytes than the %u-byte pad; growing header...\n",
               grow_req, pad_avail);
        if (mg_grow_header(&buf, &fsize, grow_req) != 0) {
            fprintf(stderr, "ERROR: new LCs (%u bytes) don't fit and header could not be grown\n",
                    new_off);
            return 1;
        }
        hdr = (struct mach_header_64 *)buf;
        first_sect_off = mg_first_sect_off(buf, fsize);
        if (first_sect_off == UINT32_MAX) {
            fprintf(stderr, "ERROR: header grow produced an image that fails validation\n");
            return 1;
        }
        printf("Grew header pad: first sect now at %u (%u bytes available)\n",
               first_sect_off, first_sect_off - cur_lc_end);
        /* Rebuild against the relocated header so segment/linkedit offsets in
         * the copied load commands reflect the shift. */
        free(new_lcs);
        new_lcs = calloc(1, first_sect_off + add_bytes + 64);
        build_lcs(buf, changes, nchanges, adds, nadds, inserts, ninserts, strip, nstrip,
                  rchanges, nrchanges, radds, nradds,
                  new_lcs, &new_off, &new_ncmds, &modifications, 0);
    }

    /* Commit: zero the whole LC area, write the new table, fix up the header. */
    memset(buf + sizeof(struct mach_header_64), 0, first_sect_off - sizeof(struct mach_header_64));
    memcpy(buf + sizeof(struct mach_header_64), new_lcs, new_off);
    hdr->ncmds = new_ncmds;
    hdr->sizeofcmds = new_off;

    /* Ordinals last, against the committed table — and before any write, so a
     * refusal leaves the input untouched rather than half-rewritten. */
    if (needs_renumber && mo_map_apply(buf, &omap, 1) != 0) {
        fprintf(stderr, "ERROR: %s left unmodified\n", path);
        return 1;
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
        fprintf(stderr, "ERROR: refusing to write %s -- it would carry base-relative "
                        "offsets that name no known function. Left unmodified.\n", path);
        return 1;
    }

    if (ftruncate(fd, fsize) != 0) { perror("ftruncate"); return 1; }
    lseek(fd, 0, SEEK_SET);
    if (write(fd, buf, fsize) != (ssize_t)fsize) { perror("write"); return 1; }
    close(fd);
    printf("Updated %s (sizeofcmds=%u, %zu bytes)\n", path, new_off, fsize);
    return 0;
}
