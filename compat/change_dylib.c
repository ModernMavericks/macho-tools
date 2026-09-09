/*
 * Rewrite LC_LOAD_DYLIB and LC_RPATH paths.
 *
 * This file is now ONLY the old command-line grammar: it parses argv into an
 * mr_ops and hands it to mr_apply_file (src/rewrite.h), which is where every
 * byte of the rewrite -- and every message it prints -- actually lives.
 * cli/macho9.c's `dylib`/`rpath`/`lc` verbs parse their own, newer grammar
 * into the same mr_ops and call the same function, so the two tools cannot
 * drift apart on what a rewrite does. (macho9 used to fork and exec THIS
 * binary to get the work done, which made change_dylib a runtime dependency
 * of macho9 -- a cycle, once change_dylib becomes a wrapper around macho9.)
 *
 * By default the new load commands must fit in the header padding between the
 * last load command and the first section's file data; if they don't, the tool
 * fails (unchanged behavior). Pass -grow to opt in to enlarging that padding
 * first (see src/grow.h) — that resize only works on a PIE executable and is
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
 * Library ordinals -- why -insert and -delete have to renumber, and what
 * happens if they don't -- are documented in src/rewrite.h, next to the code
 * that does the renumbering.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "lc_kinds.h"
#include "rewrite.h"

/* The -strip-lc KIND vocabulary lives in src/lc_kinds.c now, shared with
 * cli/macho9.c's `lc -delete` and its --capabilities output -- see that
 * file's comment for why. `strippable` was this table's name here before;
 * kept as a local alias so the rest of this file (and its usage text) don't
 * all need renaming for a table that hasn't changed shape. */
#define strippable LC_STRIP_KINDS

/* Caps on how many times one option may repeat. Each option accumulates into a
 * fixed-size array; nothing reads a length back, so an unchecked write past the
 * end corrupts whatever follows instead of failing. Check every one. The
 * numbers themselves are MR_MAX_OPS/MR_MAX_STRIP (src/rewrite.h), shared with
 * macho9's parser so both front-ends refuse at the same point. */
#define CD_ROOM(n, max, flag)                                            \
    do {                                                                 \
        if ((n) == (max)) {                                              \
            fprintf(stderr, "too many %s (max %d)\n", (flag), (max));    \
            return 1;                                                    \
        }                                                                \
    } while (0)

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

    mr_change changes[MR_MAX_OPS];
    int nchanges = 0;
    const char *adds[MR_MAX_OPS];
    int nadds = 0;
    const char *inserts[MR_MAX_OPS];
    int ninserts = 0;
    mr_change rchanges[MR_MAX_OPS];
    int nrchanges = 0;
    const char *radds[MR_MAX_OPS];
    int nradds = 0;
    int allow_grow = 0;
    uint32_t strip[MR_MAX_STRIP];
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
            CD_ROOM(nstrip, MR_MAX_STRIP, "-strip-lc");
            strip[nstrip++] = strippable[k].cmd;
            i += 2;
        } else if (strcmp(argv[i], "-add") == 0 && i + 1 < argc) {
            CD_ROOM(nadds, MR_MAX_OPS, "-add");
            adds[nadds++] = argv[i+1];
            i += 2;
        } else if (strcmp(argv[i], "-insert") == 0 && i + 1 < argc) {
            CD_ROOM(ninserts, MR_MAX_OPS, "-insert");
            inserts[ninserts++] = argv[i+1];
            i += 2;
        } else if (strcmp(argv[i], "-change") == 0 && i + 2 < argc) {
            CD_ROOM(nchanges, MR_MAX_OPS, "-change");
            changes[nchanges].old_path = argv[i+1];
            changes[nchanges].new_path = argv[i+2];
            changes[nchanges].reexport = 0;
            nchanges++;
            i += 3;
        } else if (strcmp(argv[i], "-delete") == 0 && i + 1 < argc) {
            CD_ROOM(nchanges, MR_MAX_OPS, "-delete");
            changes[nchanges].old_path = argv[i+1];
            changes[nchanges].new_path = NULL;
            changes[nchanges].reexport = 0;
            nchanges++;
            i += 2;
        } else if (strcmp(argv[i], "-reexport") == 0 && i + 1 < argc) {
            CD_ROOM(nchanges, MR_MAX_OPS, "-reexport");
            changes[nchanges].old_path = argv[i+1];
            changes[nchanges].new_path = "";
            changes[nchanges].reexport = 1;
            nchanges++;
            i += 2;
        } else if (strcmp(argv[i], "-add-rpath") == 0 && i + 1 < argc) {
            CD_ROOM(nradds, MR_MAX_OPS, "-add-rpath");
            radds[nradds++] = argv[i+1];
            i += 2;
        } else if (strcmp(argv[i], "-change-rpath") == 0 && i + 2 < argc) {
            CD_ROOM(nrchanges, MR_MAX_OPS, "-change-rpath");
            rchanges[nrchanges].old_path = argv[i+1];
            rchanges[nrchanges].new_path = argv[i+2];
            rchanges[nrchanges].reexport = 0;
            nrchanges++;
            i += 3;
        } else if (strcmp(argv[i], "-delete-rpath") == 0 && i + 1 < argc) {
            CD_ROOM(nrchanges, MR_MAX_OPS, "-delete-rpath");
            rchanges[nrchanges].old_path = argv[i+1];
            rchanges[nrchanges].new_path = NULL;
            rchanges[nrchanges].reexport = 0;
            nrchanges++;
            i += 2;
        } else { fprintf(stderr, "bad arg: %s\n", argv[i]); return 1; }
    }

    mr_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.dylib_changes = changes;   ops.n_dylib_changes = nchanges;
    ops.dylib_appends = adds;      ops.n_dylib_appends = nadds;
    ops.dylib_inserts = inserts;   ops.n_dylib_inserts = ninserts;
    ops.strip_cmds    = strip;     ops.n_strip_cmds    = nstrip;
    ops.rpath_changes = rchanges;  ops.n_rpath_changes = nrchanges;
    ops.rpath_appends = radds;     ops.n_rpath_appends = nradds;
    ops.allow_grow    = allow_grow;

    return mr_apply_file(path, &ops);
}
