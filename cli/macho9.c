/*
 * macho9 — the multi-call CLI the seven rewriters converge behind.
 *
 * Grammar settled in docs/PROPOSAL.md ("Verbs"); task-4-brief.md gives the
 * verbatim subset this build targets:
 *
 *   macho9 declassify IN OUT
 *   macho9 dylib FILE [--allow-grow] [--fatal-warnings] OP...   -replace -delete -append -insert -reexport
 *   macho9 rpath FILE [--allow-grow] [--fatal-warnings] OP...   -replace -delete -append -insert
 *   macho9 segment FILE OLD NEW
 *   macho9 retag-swift FILE
 *   macho9 lc FILE [--fatal-warnings] -delete KIND
 *   macho9 grow FILE N
 *   macho9 minos FILE 10.9
 *   macho9 info FILE
 *   macho9 verify FILE
 *
 * Not every line above is implemented by every build -- `macho9 --capabilities`
 * is the machine-readable truth about which ones are, so the wrapper and this
 * binary never have to move in lockstep (docs/PROPOSAL.md "Migration"). See
 * print_capabilities() below for the exact format and what is real today.
 *
 * DELEGATION, not reimplementation. Every verb here calls straight into the
 * primitives src/ already builds and tests: `verify`, `info` and `grow` into
 * mg_plausible, mi_open/mi_each_lc and mg_grow_header; `dylib`, `rpath`,
 * `lc` and `segment` into mr_apply_file (src/rewrite.h); `minos` into
 * mv_add_version_min (src/version_min.h); `retag-swift` into
 * mswift_retag_file (src/swift_retag.h); `declassify` into md_declassify
 * (src/declassify.h). Each verb is a thin shell over code
 * that already exists in this repo, and each translates this grammar into the
 * ONE
 * implementation -- so the ordinal-renumbering logic that has twice shipped
 * loader-crashing bugs (docs/PROPOSAL.md "verify") is exercised exactly once,
 * however it is reached.
 *
 * Those last four verbs used to be delegated by RUNNING change_dylib and
 * add_version_min as subprocesses, found next to macho9 on disk. That made
 * this binary depend at runtime on the very binaries the compat-retirement
 * plan replaces with wrappers around it -- a cycle. Task 0.5 lifted the
 * rewrite out of change_dylib.c's main() into src/rewrite.c and
 * add_version_min.c's into src/version_min.c; both tools now parse their old
 * grammars into the same calls this file makes, so there is no sibling binary
 * to find, and no way for the two front-ends to drift apart.
 *
 * `segment` and `retag-swift` are the same arrangement one task later:
 * rename_segment's rename is src/segname.h and retag_swift_classes' per-file
 * work is src/swift_retag.h, and each compat tool keeps only its own grammar,
 * write path, exit code and messages. `segment` reaches the shared rename
 * THROUGH mr_apply_file rather than calling it directly, which is what gives
 * this verb fat containers and an atomic write-back that rename_segment has
 * never had.
 *
 * `declassify` is the last of them, and the same arrangement again:
 * patch_macho's chained-fixups conversion is src/declassify.h, and that tool
 * keeps only its `IN OUT` grammar, its own open+write of OUT, its messages
 * and its flat exit code. It is the one verb here that reads one file and
 * writes another rather than rewriting in place, so it writes OUT through
 * wa_write_atomic itself instead of going through mr_apply_file.
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
#include "ordinals.h"
#include "grow.h"
#include "lc_kinds.h"
#include "atomic_write.h"
#include "declassify.h"
#include "rewrite.h"
#include "segname.h"
#include "swift_retag.h"
#include "version_min.h"
#include "mach_compat.h"

/* Exit codes. 0 is success, as always. Everything else used to be a flat 1,
 * which meant a caller checking only "did this exit nonzero" (still fully
 * supported -- see below) could not tell "macho9 examined FILE and declined,
 * on purpose, because of what it found" (not a Mach-O, not plausible, a
 * KIND/version/segment name this build doesn't support, mg_grow_header's own
 * designed refusal) apart from "something actually went wrong running macho9 itself"
 * (couldn't open/read/write, malloc failed, a usage error). Refusal is
 * load-bearing throughout this codebase -- "-grow refuses rather than
 * guesses" is a global rule, not an incidental behavior -- so a
 * caller that wants to script around "this file just isn't one macho9 will
 * touch" (vs. "retry, or investigate an environment problem") deserves a way
 * to tell the two apart without scraping stderr text, which --capabilities
 * already exists to make unnecessary for everything else this binary
 * reports. EX_REFUSED is used ONLY at a point where macho9 itself examined
 * the input and made that call; it is never used for a genuine operational
 * failure (a syscall that failed, a malloc that failed, a bad number of
 * command-line arguments) or for the exit code of the shared rewrite drivers
 * (mr_apply_file, mv_add_version_min) that dylib/rpath/lc/minos hand back --
 * those return 0 or 1 and do not make this distinction themselves, so
 * forwarding them verbatim keeps this from claiming a precision it does not
 * have. 1 keeps meaning exactly what it always did, so a caller that only
 * checks "== 0" or "!= 0" needs no changes; --capabilities documents both
 * codes (see print_capabilities below) and tests/README.md repeats it for
 * humans.
 *
 * The one exception, since --fatal-warnings: mr_apply_file returns MR_REFUSED
 * (rewrite.h), not just 0 or 1, when ops.fatal_unmatched turned "an operation
 * matched nothing" into a refusal. dylib/rpath/lc still forward mr_apply_file's
 * return value verbatim (see their own `return mr_apply_file(...)` call
 * sites) -- no new mapping was added at those call sites -- so this only
 * keeps meaning EX_REFUSED because MR_REFUSED is DEFINED to equal it; see
 * the typedef just below. */
#define EX_REFUSED 2

/* mr_apply_file's MR_REFUSED (rewrite.h) is forwarded verbatim by
 * cmd_dylib_or_rpath and cmd_lc as this binary's own exit code, so it has to
 * equal EX_REFUSED or --capabilities' documented refused=2 would be a lie
 * for exactly the case --fatal-warnings exists to handle. A mismatch here is
 * a build failure, not a hope -- the same device commit 247d09d used for
 * mg_classify/ml_bump_lc's coupling. */
typedef char mr_refused_is_ex_refused[(MR_REFUSED == EX_REFUSED) ? 1 : -1];

/* The KIND vocabulary `lc -delete` accepts is LC_STRIP_KINDS (src/lc_kinds.h),
 * shared with change_dylib's -strip-lc -- so lc's translation to it is a
 * rename, not a new decision, and there is exactly one table to edit if the
 * vocabulary ever changes. print_capabilities() below reads the same table
 * to build its "kinds=" list, rather than keeping a separate string that can
 * silently drift from what this function (and change_dylib) actually
 * accept -- that drift is exactly what a whole-branch review found here. */

/* Operations `dylib`/`rpath` accept, and which mr_ops array each one fills --
 * ONE table drives both cmd_dylib_or_rpath's parser (below) and
 * print_capabilities' "ops=" list, for the same reason LC_STRIP_KINDS is
 * shared: two hand-maintained lists (the parser's if/else chain and a
 * hardcoded ops= string) had already diverged from each other by the time of
 * review. DOP_NONE for a mode means "not supported in that mode" -- LC_RPATH
 * has only one kind, so `reexport` is meaningless for it and is simply absent
 * from rpath's derived ops= list and refused by the parser. `-insert` IS
 * supported for both now: docs/PROPOSAL.md calls rpath -insert "a new
 * capability" change_dylib never had (its grammar has no spelling for it), and
 * this build implements it -- an LC_RPATH placed ahead of every existing one,
 * so dyld, which takes the first search path that resolves, tries it first. */
enum dylib_op_kind {
    DOP_NONE = 0,   /* must stay 0: the ops= filter tests for falsiness */
    DOP_REPLACE,
    DOP_DELETE,
    DOP_APPEND,
    DOP_INSERT,
    DOP_REEXPORT
};
struct dylib_op {
    const char *flag;      /* this grammar's -OP spelling, e.g. "-replace" */
    const char *cap_name;  /* same op's spelling in ops=, e.g. "replace" */
    int nargs;             /* args consumed after the flag: 1 or 2 */
    int dylib_kind;        /* what it does when is_rpath==0, or DOP_NONE */
    int rpath_kind;        /* what it does when is_rpath==1, or DOP_NONE */
};
static const struct dylib_op DYLIB_OPS[] = {
    { "-replace",  "replace",  2, DOP_REPLACE,  DOP_REPLACE },
    { "-delete",   "delete",   1, DOP_DELETE,   DOP_DELETE  },
    { "-append",   "append",   1, DOP_APPEND,   DOP_APPEND  },
    { "-insert",   "insert",   1, DOP_INSERT,   DOP_INSERT  },
    { "-reexport", "reexport", 1, DOP_REEXPORT, DOP_NONE    },
};
#define N_DYLIB_OPS (sizeof(DYLIB_OPS) / sizeof(DYLIB_OPS[0]))

/* Print LC_STRIP_KINDS as a comma-separated list, no trailing comma -- the
 * "kinds=" value in --capabilities and cmd_lc's own error message. */
static void print_kinds_csv(void) {
    for (size_t k = 0; k < LC_STRIP_KINDS_COUNT; k++)
        printf("%s%s", k ? "," : "", LC_STRIP_KINDS[k].name);
}

/* Print DYLIB_OPS' cap_name for every op this mode (rpath or dylib) actually
 * supports, comma-separated -- the "ops=" value in --capabilities. */
static void print_ops_csv(int is_rpath) {
    int first = 1;
    for (size_t i = 0; i < N_DYLIB_OPS; i++) {
        if (is_rpath ? !DYLIB_OPS[i].rpath_kind : !DYLIB_OPS[i].dylib_kind) continue;
        printf("%s%s", first ? "" : ",", DYLIB_OPS[i].cap_name);
        first = 0;
    }
}

/* ---- capabilities -------------------------------------------------------
 *
 * Stable, line-oriented, greppable -- shell is the wrapper's own language, so
 * this is not JSON. Contract:
 *
 *   line 1: "format <N>"       -- bump N only if a later build changes this
 *                                  TEXT's shape in a way old parsing breaks.
 *   line 2: "exitcodes ok=0 refused=<N> failed=1" -- what this binary's own
 *       exit codes mean: ok=0 always; refused=EX_REFUSED is used only where
 *       macho9 itself examined FILE and declined on purpose (bad magic,
 *       implausible, an unsupported KIND/version, a grow mg_grow_header
 *       itself refused); failed=1 is everything else (syscall/malloc
 *       failure, usage error) -- unchanged from before this line existed, so
 *       a caller checking only nonzero needs no changes. dylib/rpath/lc/minos
 *       return the shared rewrite drivers' own code (mr_apply_file,
 *       mv_add_version_min: 0 or 1), which does not make this distinction,
 *       so their exit codes are still not covered by this line -- except for
 *       the checks macho9 makes BEFORE calling them (an unknown lc KIND, a
 *       version other than 10.9), which are refusals and say so, AND except
 *       for a dylib/rpath/lc run given --fatal-warnings, where mr_apply_file
 *       itself returns EX_REFUSED (as MR_REFUSED, rewrite.h) when an
 *       operation matched nothing -- see that flag's own entry below. See
 *       EX_REFUSED's own comment for the full reasoning.
 *   line 3+: "verb <name> [key=value ...]"
 *       one line per verb this build actually implements. A verb's absence
 *       means "not implemented" -- never advertise one that errors out.
 *       Recognized attributes:
 *         ops=a,b,c      the -OP flags this verb accepts (comma-separated,
 *                         no spaces)
 *         kinds=a,b,c    (lc only) the KIND vocabulary -delete accepts
 *         versions=a,b   (minos only) the floors this build can target
 *         flags=a,b      verb-level flags, e.g. allow-grow. fatal-warnings
 *                        (dylib/rpath/lc) turns "an operation matched
 *                        nothing" from a stderr report into a refusal
 *                        (EX_REFUSED) -- but it NEVER ROLLS BACK a write it
 *                        made: if some other operation in the same run DID
 *                        match, that write already happened by the time
 *                        this refuses. (If EVERY operation matched nothing,
 *                        there was no write to roll back in the first
 *                        place -- same as any other all-miss run.) Named
 *                        after `ld`/`gas`'s own --fatal-warnings. It catches
 *                        "you asked for something that matched nothing",
 *                        NOT "you asked for something that matched but was
 *                        shadowed by an earlier operation": two -replace
 *                        flags naming the same old path both count as hits
 *                        and this stays silent, even though only the first
 *                        can act. That line is where it is because counting
 *                        only the operation that ACTED reports the -delete
 *                        of `-replace X N -delete X` as a false miss --
 *                        src/rewrite.h's mr_ops.fatal_unmatched has the
 *                        whole argument.
 *         reports=a,b    machine-readable "<verb>: <key>=<value>" lines this
 *                         verb prints on success, by key -- today only
 *                         `segment reports=renamed`
 *
 * `dylib` lists all five brief ops; `rpath` lists four -- everything but
 * `reexport`, which LC_RPATH's single kind makes meaningless. Both lists are
 * derived from DYLIB_OPS, the same table the parser matches against, so
 * neither can advertise an op the parser would refuse.
 *
 * Every verb listed below is unconditional now. dylib/rpath/lc/minos used to
 * be gated on a sibling binary (change_dylib / add_version_min) being present
 * and executable next to macho9, because that is what they ran to do the
 * work: advertising them when the sibling was missing would have violated
 * this function's own contract ("never advertise one that errors out") the
 * moment macho9 was packaged apart from them. Task 0.5 removed the
 * subprocess -- the rewrite is linked in from src/rewrite.c and
 * src/version_min.c now -- so there is no external file left whose absence
 * could make an advertised verb fail, and nothing left to probe. */
static int print_capabilities(void) {
    printf("format 1\n");
    printf("exitcodes ok=0 refused=%d failed=1\n", EX_REFUSED);
    printf("verb declassify\n");
    printf("verb verify\n");
    printf("verb info\n");
    printf("verb grow\n");
    /* reports=renamed: this verb prints "macho9 segment: renamed=<N>" on
     * success, the match count nothing outside the rewriter can derive. See
     * cmd_segment for why, and compat/rename_segment.sh for who needs it. */
    printf("verb segment reports=renamed\n");
    printf("verb retag-swift\n");
    printf("verb minos versions=10.9\n");
    printf("verb lc ops=delete kinds=");
    print_kinds_csv();
    printf(" flags=fatal-warnings\n");
    printf("verb dylib ops=");
    print_ops_csv(0);
    printf(" flags=allow-grow,fatal-warnings\n");
    printf("verb rpath ops=");
    print_ops_csv(1);
    printf(" flags=allow-grow,fatal-warnings\n");
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s --capabilities\n"
        "       %s declassify IN OUT                        chained fixups -> LC_DYLD_INFO_ONLY;\n"
        "                                                    IN is only read, so IN and OUT may match\n"
        "       %s dylib FILE [--allow-grow] [--fatal-warnings] OP...\n"
        "                                                    -replace OLD NEW | -delete PATH |\n"
        "                                                    -append PATH | -insert PATH | -reexport PATH\n"
        "       %s rpath FILE [--allow-grow] [--fatal-warnings] OP...\n"
        "                                                    -replace OLD NEW | -delete PATH |\n"
        "                                                    -append PATH (searched LAST) |\n"
        "                                                    -insert PATH (searched FIRST)\n"
        "       %s segment FILE OLD NEW                     rename every segment named OLD, and\n"
        "                                                    its sections' copy of that name\n"
        "       %s retag-swift FILE\n"
        "       %s lc FILE [--fatal-warnings] -delete KIND [-delete KIND...]\n"
        "                                                    uuid | codesig | source-version |\n"
        "                                                    build-version | code-sign-drs\n"
        "       %s grow FILE N\n"
        "       %s minos FILE 10.9\n"
        "       %s info FILE\n"
        "       %s verify FILE\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* ---- verify: a thin shell over mg_plausible -----------------------------
 *
 * Exactly what the brief asks Step 2 to prove: dispatch works, and the verb
 * adds no logic of its own beyond opening the file and reporting the result.
 */
static int cmd_verify(const char *path) {
    mi_image im;
    if (mi_open(path, &im) != 0) {
        fprintf(stderr, "macho9 verify: %s: not a readable 64-bit Mach-O\n", path);
        return EX_REFUSED;
    }
    int rc = mg_plausible(im.buf, im.size);
    printf("%s: %s\n", path, rc == 0 ? "OK" : "FAILED (see above)");
    mi_close(&im);
    return rc == 0 ? 0 : EX_REFUSED;
}

/* ---- info: dump load commands, ordinals, pads ---------------------------
 *
 * No existing tool does this dump, so unlike verify/grow this is new code --
 * but it is a pure reader: everything it walks comes from mi_open/mi_each_lc
 * (image.h) and mo_is_ordinal_lc (ordinals.h), never from re-deriving what
 * "ordinal-bearing" or "the header pad" mean. Output is deliberately stable
 * and greppable: tests/cli_test.sh asserts on it directly, which the task's
 * own host-portability rule endorses over parsing otool/nm. */
struct info_ctx {
    int idx;
    int ordinal;
};

static const char *lc_name(uint32_t cmd) {
    switch (cmd) {
    case LC_SEGMENT_64:         return "LC_SEGMENT_64";
    case LC_SYMTAB:              return "LC_SYMTAB";
    case LC_DYSYMTAB:            return "LC_DYSYMTAB";
    case LC_LOAD_DYLIB:          return "LC_LOAD_DYLIB";
    case LC_ID_DYLIB:            return "LC_ID_DYLIB";
    case LC_LOAD_WEAK_DYLIB:     return "LC_LOAD_WEAK_DYLIB";
    case LC_REEXPORT_DYLIB:      return "LC_REEXPORT_DYLIB";
    case LC_LOAD_UPWARD_DYLIB:   return "LC_LOAD_UPWARD_DYLIB";
    case LC_RPATH:                return "LC_RPATH";
    case LC_UUID:                 return "LC_UUID";
    case LC_CODE_SIGNATURE:      return "LC_CODE_SIGNATURE";
    case LC_VERSION_MIN_MACOSX:  return "LC_VERSION_MIN_MACOSX";
    case LC_MAIN:                 return "LC_MAIN";
    case LC_DYLD_INFO:            return "LC_DYLD_INFO";
    case LC_DYLD_INFO_ONLY:      return "LC_DYLD_INFO_ONLY";
    case LC_FUNCTION_STARTS:     return "LC_FUNCTION_STARTS";
    case LC_DATA_IN_CODE:        return "LC_DATA_IN_CODE";
    case LC_SOURCE_VERSION:      return "LC_SOURCE_VERSION";
    case LC_BUILD_VERSION:       return "LC_BUILD_VERSION";
    case LC_DYLIB_CODE_SIGN_DRS: return "LC_DYLIB_CODE_SIGN_DRS";
    case LC_DYLD_EXPORTS_TRIE:   return "LC_DYLD_EXPORTS_TRIE";
    case LC_DYLD_CHAINED_FIXUPS: return "LC_DYLD_CHAINED_FIXUPS";
    default:                      return NULL;
    }
}

/* dylib_command/rpath_command names are an lc_str offset relative to the
 * command's own start; the bounds check against cmdsize lives once, in
 * mo_lc_str_at (ordinals.h), which change_dylib.c's build_lcs and
 * mo_map_build also call -- so this dump can't drift out of agreement with
 * what the rewriters consider in-bounds, the way it briefly did. */
static const char *lc_str_at(const struct load_command *lc, uint32_t offset) {
    const char *s = mo_lc_str_at(lc, offset);
    return s ? s : "(malformed: offset past cmdsize)";
}

static int info_cb(const struct load_command *lc, void *ctx_) {
    struct info_ctx *ctx = ctx_;
    const char *name = lc_name(lc->cmd);
    if (name) printf("LC[%d] %s cmdsize=%u\n", ctx->idx, name, lc->cmdsize);
    else      printf("LC[%d] 0x%08x cmdsize=%u\n", ctx->idx, lc->cmd, lc->cmdsize);

    if (lc->cmd == LC_SEGMENT_64) {
        const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
        printf("  segname=%.16s vmaddr=0x%llx vmsize=0x%llx fileoff=%llu filesize=%llu nsects=%u\n",
               seg->segname, (unsigned long long)seg->vmaddr, (unsigned long long)seg->vmsize,
               (unsigned long long)seg->fileoff, (unsigned long long)seg->filesize, seg->nsects);
    }
    if (mo_is_ordinal_lc(lc->cmd)) {
        ctx->ordinal++;
        const struct dylib_command *dc = (const struct dylib_command *)lc;
        printf("  ordinal=%d path=%s\n", ctx->ordinal, lc_str_at(lc, dc->dylib.name.offset));
    }
    if (lc->cmd == LC_RPATH) {
        const struct rpath_command *rc = (const struct rpath_command *)lc;
        printf("  rpath=%s\n", lc_str_at(lc, rc->path.offset));
    }
    if (lc->cmd == LC_VERSION_MIN_MACOSX) {
        const struct version_min_command *vc = (const struct version_min_command *)lc;
        printf("  version=%u.%u.%u sdk=%u.%u.%u\n",
               vc->version >> 16, (vc->version >> 8) & 0xff, vc->version & 0xff,
               vc->sdk >> 16, (vc->sdk >> 8) & 0xff, vc->sdk & 0xff);
    }
    ctx->idx++;
    return 0;   /* prints every command; never needs to stop early */
}

static int cmd_info(const char *path) {
    mi_image im;
    if (mi_open(path, &im) != 0) {
        fprintf(stderr, "macho9 info: %s: not a readable 64-bit Mach-O\n", path);
        return EX_REFUSED;
    }
    printf("%s: %zu bytes, %u load commands, filetype=%u\n",
           path, im.size, im.hdr->ncmds, im.hdr->filetype);
    struct info_ctx ctx = { 0, 0 };
    mi_each_lc(&im, info_cb, &ctx);

    uint32_t first_sect_off = mg_first_sect_off(im.buf, im.size);
    if (first_sect_off != UINT32_MAX) {
        uint32_t lc_end = (uint32_t)sizeof(struct mach_header_64) + im.hdr->sizeofcmds;
        uint32_t pad = first_sect_off > lc_end ? first_sect_off - lc_end : 0;
        printf("header pad: %u bytes available (LC end=%u, first sect=%u)\n",
               pad, lc_end, first_sect_off);
    }
    mi_close(&im);
    return 0;
}

/* ---- grow: a thin shell over mg_grow_header -----------------------------
 *
 * mg_grow_header already runs mg_verify + mg_plausible internally before it
 * reports success (src/grow.h "Phase 4: prove it"), so there is nothing
 * left for this verb to check on top -- it opens, calls the real primitive,
 * and writes back only on success. On failure mg_grow_header has already
 * explained why on stderr and left *pbuf as whatever is safe to discard;
 * the file itself is never touched.
 *
 * The write-back goes through wa_write_atomic (src/atomic_write.h), the same
 * mkstemp()+rename() (symlink-safe, hard-link-aware, xattr-preserving) path
 * change_dylib uses -- this used to ftruncate()+write() straight into the
 * open file instead, which a previous review deferred fixing "as consistent
 * with change_dylib" back when change_dylib ALSO did that. That reason went
 * stale the moment change_dylib became atomic and this verb didn't follow;
 * sharing the one implementation is what keeps that from happening again. */
static int cmd_grow(const char *path, const char *n_str) {
    char *end;
    unsigned long n = strtoul(n_str, &end, 10);
    if (*end != '\0' || n == 0 || n > UINT32_MAX) {
        fprintf(stderr, "macho9 grow: N must be a positive byte count (got '%s')\n", n_str);
        return 1;
    }

    /* Opened O_RDWR up front only to fail fast on an unwritable/missing file
     * and to learn its mode for the replacement's fchmod -- not held for the
     * write-back, which wa_write_atomic does via its own mkstemp()+rename(),
     * same rationale as change_dylib.c's main(). */
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("macho9 grow: open"); return 1; }
    struct stat st;
    mode_t mode = (fstat(fd, &st) == 0) ? st.st_mode : 0644;
    close(fd);

    mi_image im;
    if (mi_open(path, &im) != 0) {
        fprintf(stderr, "macho9 grow: %s: not a readable 64-bit Mach-O\n", path);
        return EX_REFUSED;
    }
    size_t fsize = im.size;
    uint8_t *buf = mi_release(&im);

    if (mg_grow_header(&buf, &fsize, (uint32_t)n) != 0) {
        /* mg_grow_header's whole design is "refuse rather than guess" (a
         * global rule -- see EX_REFUSED's own comment) -- growth that would
         * need a real __LINKEDIT resize, a non-PIE image, an unsupported
         * ULEB re-encode -- so a failure here is a refusal, not an
         * operational error. */
        fprintf(stderr, "macho9 grow: %s left unmodified\n", path);
        free(buf);
        return EX_REFUSED;
    }

    if (wa_write_atomic(path, mode, buf, fsize) != 0) {
        fprintf(stderr, "macho9 grow: %s left unmodified (atomic replace failed)\n", path);
        free(buf);
        return 1;
    }
    printf("Grew %s: header pad enlarged, file now %zu bytes\n", path, fsize);
    free(buf);
    return 0;
}

/* ---- minos: a thin shell over mv_add_version_min ------------------------
 *
 * mv_add_version_min only knows how to target 10.9 (it hardcodes
 * LC_VERSION_MIN_MACOSX 10.9.0 -- see src/version_min.c), which is exactly
 * why this verb's grammar spells the floor literally rather than taking any
 * version: there is only one this build can honor, so refusing anything else
 * up front is a clearer failure than calling in and hoping. */
static int cmd_minos(const char *path, const char *version) {
    if (strcmp(version, "10.9") != 0) {
        fprintf(stderr, "macho9 minos: only 10.9 is supported by this build (got '%s')\n", version);
        return EX_REFUSED;
    }
    return mv_add_version_min(path);
}

/* ---- lc -delete: a thin shell over mr_apply_file's strip_cmds -----------
 *
 * The KIND vocabulary is validated here (against the very table --
 * LC_STRIP_KINDS, shared with change_dylib's -strip-lc -- that also drives
 * --capabilities) before anything runs, so a bad KIND fails with this verb's
 * own message rather than somewhere deeper. Translating a KIND to the LC_*
 * constant the rewriter wants is that same table's other column, so this verb
 * decides nothing the vocabulary does not already say.
 *
 * `--fatal-warnings` is parsed the same way `dylib`/`rpath` parse it: a
 * verb-level flag, checked before the `-delete` branch below, that becomes
 * ops.fatal_unmatched. See cmd_dylib_or_rpath's own comment for what it
 * means and why it is named after `ld`/`gas`'s flag of the same name. */
static int cmd_lc(int argc, char **argv) {
    /* argv[0]=macho9 argv[1]="lc" argv[2]=FILE argv[3..]=ops */
    const char *path = argv[2];
    uint32_t strip[MR_MAX_STRIP];
    int nstrip = 0;
    int fatal_warnings = 0;
    for (int i = 3; i < argc; ) {
        if (strcmp(argv[i], "--fatal-warnings") == 0) {
            fatal_warnings = 1;
            i += 1;
        } else if (strcmp(argv[i], "-delete") == 0 && i + 1 < argc) {
            const char *kind = argv[i + 1];
            size_t kk;
            for (kk = 0; kk < LC_STRIP_KINDS_COUNT; kk++)
                if (strcmp(kind, LC_STRIP_KINDS[kk].name) == 0) break;
            if (kk == LC_STRIP_KINDS_COUNT) {
                fprintf(stderr, "macho9 lc: unknown KIND '%s' (expected one of:", kind);
                for (kk = 0; kk < LC_STRIP_KINDS_COUNT; kk++) fprintf(stderr, " %s", LC_STRIP_KINDS[kk].name);
                fprintf(stderr, ")\n");
                return EX_REFUSED;
            }
            /* Same CAP as change_dylib's -strip-lc (MR_MAX_STRIP, shared via
             * src/rewrite.h so the two front-ends refuse the same inputs),
             * deliberately DIFFERENT wording. change_dylib prints "too many
             * -strip-lc (max 16)"; repeating that here would leak the old
             * grammar's flag spellings out of a verb whose whole point is not
             * to expose them -- cli_test.sh asserts exactly that, for
             * --allow-grow's own no-op message. THE change_dylib SHELL WRAPPER
             * therefore cannot get the origin message by passing this through:
             * it enforces the 16 itself, in compat/translate.sh's mt_room, and
             * prints "too many -strip-lc (max 16)". tests/wrapper_test.sh pins
             * that text. */
            if (nstrip == MR_MAX_STRIP) {
                fprintf(stderr, "macho9 lc: too many -delete operations (max %d)\n", MR_MAX_STRIP);
                return 1;
            }
            strip[nstrip++] = LC_STRIP_KINDS[kk].cmd;
            i += 2;
        } else {
            fprintf(stderr, "macho9 lc: unknown operation '%s' (only -delete KIND and --fatal-warnings are supported)\n", argv[i]);
            return 1;
        }
    }
    if (nstrip == 0) {
        fprintf(stderr, "macho9 lc: need at least one -delete KIND\n");
        return 1;
    }
    mr_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.strip_cmds = strip;
    ops.n_strip_cmds = nstrip;
    ops.fatal_unmatched = fatal_warnings;
    return mr_apply_file(path, &ops);
}

/* ---- dylib / rpath: a thin shell over mr_apply_file ---------------------
 *
 * Token-for-token translation into an mr_ops (src/rewrite.h): -replace and
 * -delete become entries in the dylib_changes (or rpath_changes) array,
 * -append and -insert become the appends/inserts arrays of whichever kind
 * this verb is, -reexport becomes a change with an empty new_path, and
 * `--allow-grow` becomes allow_grow. Position within the OP list does not matter -- the
 * rewriter applies whole arrays, not a sequence -- so this does not enforce
 * one; only the order WITHIN each array is meaningful, and that is the order
 * the operations were typed.
 *
 * -append and -insert are NOT interchangeable for either kind, and the
 * distinction is the whole point of having both: an appended LC_LOAD_DYLIB
 * gets the highest library ordinal while an inserted one gets ordinal 1, and
 * an appended LC_RPATH is the LAST search path dyld tries while an inserted
 * one is the FIRST. Downgrading either -insert to an -append would produce a
 * wrong answer that merely runs.
 *
 * `--fatal-warnings` becomes ops.fatal_unmatched, the same way `--allow-grow`
 * becomes ops.allow_grow just below -- a verb-level flag, parsed in this same
 * loop, not one of DYLIB_OPS' per-operation entries. Named after `ld` and
 * `gas`'s own --fatal-warnings (and GCC's -Werror, the same idea under a
 * different name): "an operation matched nothing" already IS a warning
 * (mr_report_unmatched, src/rewrite.c), and this promotes it to a refusal.
 * It never rolls back a write it made -- if some other operation in the
 * same run DID match, that write already happened by the time this
 * refuses, and this only refuses about the miss after the fact. (If every
 * operation matched nothing there was no write to roll back at all.) See
 * mr_ops.fatal_unmatched's own comment in rewrite.h for the full contract.
 */
static int cmd_dylib_or_rpath(int argc, char **argv, int is_rpath) {
    const char *path = argv[2];
    /* Fixed-size, capped exactly where change_dylib's own parser caps (see
     * MR_MAX_OPS in src/rewrite.h) so the two front-ends refuse the same
     * inputs -- but in this grammar's vocabulary, since the wrapper contract
     * is that macho9 never leaks change_dylib's flag spellings. */
    mr_change changes[MR_MAX_OPS];   int nchanges = 0;
    mr_change rchanges[MR_MAX_OPS];  int nrchanges = 0;
    const char *appends[MR_MAX_OPS]; int nappends = 0;
    const char *inserts[MR_MAX_OPS]; int ninserts = 0;
    /* Counts actual -replace/-delete/-append/-insert/-reexport ops only --
     * NOT --allow-grow or --fatal-warnings, neither of which on its own is
     * something to do to a file. Without this, `dylib FILE --allow-grow`
     * with nothing else used to fall through to change_dylib and print ITS
     * usage -- leaking the exact -change/-add/-strip-lc spellings this
     * grammar deliberately doesn't offer. */
    int nops = 0;
    int allow_grow = 0;
    int fatal_warnings = 0;

    for (int i = 3; i < argc; ) {
        const char *tok = argv[i];
        if (strcmp(tok, "--allow-grow") == 0) {
            allow_grow = 1;
            i += 1;
            continue;
        }
        if (strcmp(tok, "--fatal-warnings") == 0) {
            fatal_warnings = 1;
            i += 1;
            continue;
        }

        /* Match against the shared op table (declared near the top of this
         * file) instead of a bespoke if/else chain, so this loop and
         * print_capabilities' "ops=" list are structurally unable to name
         * different operations. */
        size_t oi;
        for (oi = 0; oi < N_DYLIB_OPS; oi++)
            if (strcmp(tok, DYLIB_OPS[oi].flag) == 0) break;
        if (oi == N_DYLIB_OPS || i + DYLIB_OPS[oi].nargs >= argc) {
            fprintf(stderr, "macho9 %s: unknown or incomplete operation '%s'\n",
                    is_rpath ? "rpath" : "dylib", tok);
            return 1;
        }
        const struct dylib_op *op = &DYLIB_OPS[oi];
        int kind = is_rpath ? op->rpath_kind : op->dylib_kind;
        if (kind == DOP_NONE) {
            /* An op this table knows but this MODE does not support (today
             * only `rpath -reexport`) is reported exactly like an op the table
             * never heard of: from the caller's side both mean "this verb does
             * not accept that", and --capabilities' ops= list -- built from
             * this same table -- is where the answer to "then what does it
             * accept?" lives. */
            fprintf(stderr, "macho9 %s: unknown or incomplete operation '%s'\n",
                    is_rpath ? "rpath" : "dylib", tok);
            return 1;
        }

        mr_change *chs = is_rpath ? rchanges : changes;
        int *nchs = is_rpath ? &nrchanges : &nchanges;
        int full = 0;
        switch (kind) {
        case DOP_REPLACE:
        case DOP_DELETE:
        case DOP_REEXPORT:
            if (*nchs == MR_MAX_OPS) { full = 1; break; }
            chs[*nchs].old_path = argv[i + 1];
            chs[*nchs].new_path = (kind == DOP_REPLACE) ? argv[i + 2]
                                : (kind == DOP_REEXPORT) ? "" : NULL;
            chs[*nchs].reexport = (kind == DOP_REEXPORT);
            (*nchs)++;
            break;
        case DOP_APPEND:
            if (nappends == MR_MAX_OPS) { full = 1; break; }
            appends[nappends++] = argv[i + 1];
            break;
        case DOP_INSERT:
            if (ninserts == MR_MAX_OPS) { full = 1; break; }
            inserts[ninserts++] = argv[i + 1];
            break;
        }
        /* Same CAP as change_dylib's own parser (MR_MAX_OPS, shared via
         * src/rewrite.h), deliberately DIFFERENT wording -- and note this
         * names THIS grammar's flag (`-append`), not the one change_dylib
         * would have named (`-add`). change_dylib prints "too many -add (max
         * 32)"; repeating that here would leak the old grammar's spellings
         * out of a verb whose whole point is not to expose them, which
         * cli_test.sh already asserts against for --allow-grow's no-op
         * message. THE change_dylib SHELL WRAPPER therefore cannot get the
         * origin message by passing this through: it enforces the 32 itself,
         * in compat/translate.sh's mt_room, and prints "too many <old flag>
         * (max 32)". tests/wrapper_test.sh pins that text. */
        if (full) {
            fprintf(stderr, "macho9 %s: too many %s operations (max %d)\n",
                    is_rpath ? "rpath" : "dylib", op->flag, MR_MAX_OPS);
            return 1;
        }
        nops++;
        i += 1 + op->nargs;
    }
    if (nops == 0) {
        fprintf(stderr, "macho9 %s: need at least one operation\n", is_rpath ? "rpath" : "dylib");
        return 1;
    }

    mr_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.dylib_changes = changes;    ops.n_dylib_changes = nchanges;
    ops.dylib_appends = is_rpath ? NULL : appends;
    ops.n_dylib_appends = is_rpath ? 0 : nappends;
    ops.dylib_inserts = is_rpath ? NULL : inserts;
    ops.n_dylib_inserts = is_rpath ? 0 : ninserts;
    ops.rpath_changes = rchanges;   ops.n_rpath_changes = nrchanges;
    ops.rpath_appends = is_rpath ? appends : NULL;
    ops.n_rpath_appends = is_rpath ? nappends : 0;
    ops.rpath_inserts = is_rpath ? inserts : NULL;
    ops.n_rpath_inserts = is_rpath ? ninserts : 0;
    ops.allow_grow = allow_grow;
    ops.fatal_unmatched = fatal_warnings;
    return mr_apply_file(path, &ops);
}

/* ---- segment: a segment rename, routed through mr_apply_file -------------
 *
 * The rename itself is mseg_rename_lc (src/segname.h), the one function
 * every segment rename in this repo goes through -- including the old
 * `rename_segment` grammar, which reaches this very verb through
 * compat/rename_segment.sh. This verb reaches it through an mr_ops rather than
 * calling it directly, and that is the whole reason the operation lives in
 * mr_ops at all: mr_apply_file already handles a classic fat container by
 * rewriting each slice and reassembling, already passes through a slice it
 * does not understand, and already writes back atomically only when something
 * changed. rename_segment is thin-only and writes through its own fd; this
 * verb gets all three for free, which is what the retirement plan needs before
 * fix_macho's -rename_seg can fold into it.
 *
 * The one check that belongs HERE and not down there is the NEW name's length:
 * a segname field is 16 bytes, and refusing before any I/O -- rather than
 * silently truncating deep inside a per-slice rewrite -- is what
 * rename_segment has always done, in the same place, via the same
 * mseg_name_fits.
 *
 * FOUR DELIBERATE DIVERGENCES FROM rename_segment, all of which a wrapper
 * author has to know about, because reproducing rename_segment's observable
 * behaviour on top of this verb means accounting for each. Each is closed (or
 * knowingly not closed) in compat/rename_segment.sh, whose header says which
 * and why. A fifth USED TO belong on this list -- mg_plausible -- and no
 * longer does; see the note below the bullets.
 *
 *   - EXIT CODE WHEN NOTHING MATCHED. mr_apply_file reports "nothing to
 *     change" and exits 0; rename_segment exits 2. This verb hands back the
 *     shared driver's own code, exactly as dylib/rpath/lc do -- but it also
 *     prints `macho9 segment: renamed=<N>`, so a wrapper can tell the two
 *     apart exactly rather than by inference. See the count's own comment in
 *     cmd_segment below.
 *   - STDOUT. mr_process_thin prints its own "header pad N bytes available"
 *     and "updated (sizeofcmds=...)" lines, and mr_apply_file its "Updated
 *     ..." line; rename_segment prints exactly one line, "%s: renamed %d
 *     segment(s) %s -> %s". A wrapper that passes this verb's stdout through
 *     will not look like rename_segment.
 *   - FAT CONTAINERS. This verb reaches mr_apply_file, which handles a
 *     classic fat container by rewriting each slice it understands and
 *     reassembling; rename_segment ran mi_open, which is thin-only and fails
 *     outright on a fat file. So this verb can rename inside a fat binary
 *     that rename_segment refused to touch at all.
 *   - LC_LAZY_LOAD_DYLIB. mr_apply_file builds the library-ordinal map
 *     (mo_map_build, src/ordinals.c) before it looks at what the operations
 *     are, and that builder refuses any image carrying an
 *     LC_LAZY_LOAD_DYLIB. A segment rename touches no ordinal, so the
 *     refusal cannot be protecting anything here, but it is real: this verb
 *     can REFUSE a binary rename_segment -- which never built an ordinal
 *     map -- happily renamed. Not closed by this verb; compat/rename_segment.sh
 *     reports it rather than working around it.
 *
 * mg_plausible USED TO BE a fifth divergence and no longer is. mr_process_thin
 * (src/rewrite.c) skips that gate for a rename-only operation set -- scoped by
 * mr_is_rename_only, not by an environment variable -- because the gate asks
 * an OFFSET question and a segment rename moves no offset. So this verb no
 * longer refuses anything rename_segment would have renamed on that account;
 * MACHO_NO_VERIFY is not part of this verb's or its wrapper's story at all
 * (compat/rename_segment.sh sets no environment variable). Every operation
 * that CAN move an offset still meets the gate exactly as before. */
static int cmd_segment(const char *path, const char *oldname, const char *newname) {
    if (!mseg_name_fits(newname)) {
        fprintf(stderr, "macho9 segment: new segment name '%s' is longer than the %d bytes "
                        "a segname field holds\n", newname, MSEG_NAME_MAX);
        return EX_REFUSED;
    }
    int renamed = 0;
    mr_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.segment_rename_old = oldname;
    ops.segment_rename_new = newname;
    ops.segment_renamed = &renamed;
    int rc = mr_apply_file(path, &ops);
    /* THE MATCH COUNT, MACHINE-READABLE, and the reason mr_ops has an
     * out-param for it at all.
     *
     * A caller cannot derive it. mseg_rename_lc matches with strncmp over the
     * 16-byte segname field, which is neither NUL-terminated nor free of
     * whitespace, so reading a name back out of `macho9 info`'s human-readable
     * dump gets it wrong in at least two reachable ways -- an OLD longer than
     * 16 bytes whose first 16 match, and a segname containing a space. That is
     * tests/README.md's second lesson ("never parse human-readable output as
     * an oracle") applied to this binary's own output rather than to otool's.
     *
     * So this verb states it, in the shape --capabilities already established:
     * one line, key=value, greppable, no spaces in the value. It is what
     * compat/rename_segment.sh needs for BOTH of its observables -- the count
     * in its one output line, and its exit 2 when nothing matched -- and
     * `verb segment reports=renamed` in --capabilities is how a wrapper checks
     * this build provides it instead of assuming.
     *
     * Printed only on success, and it is 0 when nothing matched (mr_apply_file
     * says "nothing to change." and writes nothing, which is exactly the case
     * the old grammar reported as exit 2). */
    if (rc == 0) printf("macho9 segment: renamed=%d\n", renamed);
    return rc;
}

/* ---- retag-swift: a thin shell over mswift_retag_file --------------------
 *
 * THIN ONLY, matching compat/retag_swift_classes.c, which never handled a fat
 * container -- the class lists this walks are found through LC_SEGMENT_64
 * sections of one image, and there is no per-slice driver for that the way
 * mr_apply_file is one for load-command rewrites. Anything mswift_retag_file
 * cannot read gets SAID SO here, rather than the bare "return 0" the old
 * multi-file tool used to keep its argv loop going: a single-file verb that
 * prints nothing and exits 0 on a fat binary is exactly the silent success
 * docs/PROPOSAL.md's `verify` section exists to rule out.
 *
 * TWO DELIBERATE DIVERGENCES FROM retag_swift_classes, both of which a
 * wrapper author has to know about, because in each case the two front-ends
 * return DIFFERENT codes for the same input. Both are handled in
 * compat/retag_swift_classes.sh, whose header says how:
 *
 *   - MSWIFT_NOT_MACHO. retag_swift_classes skips such an argument silently
 *     and keeps going through the rest of its argv, ending at 0; this verb
 *     has exactly one file to talk about, so it refuses (EX_REFUSED) and says
 *     why.
 *   - MSWIFT_RACED -- `path` named a different inode by the time it was
 *     validated, so NOTHING was written. retag_swift_classes returns 0 for
 *     that (a benign skip in a multi-file run, already reported on stderr);
 *     this verb returns 1. Reporting success for work it did not do is the
 *     silent-success shape this codebase refuses, and a caller that scripted
 *     `macho9 retag-swift F && install F` on a 0 would install the file the
 *     race left behind.
 *
 * Both codes are tested BY NAME below, never as `n < 0` -- swift_retag.h says
 * why: a fourth benign code added later would otherwise silently become a
 * macho9 failure, which is the same "two places deciding one thing" drift the
 * shared module exists to prevent. */
static int cmd_retag_swift(const char *path) {
    int n = mswift_retag_file(path);
    if (n == MSWIFT_NOT_MACHO) {
        fprintf(stderr, "macho9 retag-swift: %s: not a readable 64-bit Mach-O. "
                        "This verb is thin-only, like retag_swift_classes, so that "
                        "covers a fat container as well as anything that is not a "
                        "Mach-O at all.\n", path);
        return EX_REFUSED;
    }
    /* Both already printed their own diagnostic inside mswift_retag_file. */
    if (n == MSWIFT_ERROR || n == MSWIFT_RACED) return 1;
    if (n < 0) {
        /* A code swift_retag.h grew that this verb has not been taught. Refuse
         * rather than fall through to "retagged -4 class record(s)" and exit
         * 0 -- an unrecognized negative is precisely the case the by-name rule
         * above exists for, and guessing which side of refusal it belongs on
         * is not this verb's call to make. */
        fprintf(stderr, "macho9 retag-swift: %s: mswift_retag_file returned an "
                        "unrecognized code %d; refusing rather than reporting a "
                        "count this verb cannot vouch for\n", path, n);
        return 1;
    }
    printf("%s: retagged %d class record(s)\n", path, n);
    return 0;
}

/* ---- declassify: a thin shell over md_declassify -------------------------
 *
 * The conversion -- chained fixups lowered to LC_DYLD_INFO_ONLY, the exports
 * trie and every LC_BUILD_VERSION stripped, __LINKEDIT extended over the
 * appended opcode streams -- is src/declassify.c, and this verb is the only C
 * front-end over it. The old `patch_macho IN OUT` grammar reaches THIS VERB
 * through compat/patch_macho.sh, so there is no second implementation left to
 * disagree with: the output file's bytes are identical by construction rather
 * than by two implementations happening to agree.
 *
 * IN OUT, not in place: this is the one verb that reads one file and writes
 * another, because that is the grammar docs/PROPOSAL.md settled on and what
 * patch_macho's callers pass. IN is only ever read (the whole image is in
 * memory before a byte is written), so `macho9 declassify F F` is safe and
 * behaves as an in-place conversion.
 *
 * FOUR DELIBERATE DIVERGENCES FROM patch_macho, all of which a wrapper author
 * has to know about, because reproducing patch_macho's observable behaviour on
 * top of this verb means accounting for each. compat/patch_macho.sh closes the
 * first and the third and enumerates the other two:
 *
 *   - EXIT CODES. patch_macho returns a flat 1 for everything that goes
 *     wrong. This verb returns EX_REFUSED where it examined the input and
 *     declined on purpose -- not a readable 64-bit Mach-O, no chained fixups
 *     to convert, or any of the conversion's own refusals -- declassify.h's
 *     LIMITS section lists them all: too many segments or strippable
 *     commands, more fixups than the opcode buffers hold, an unknown pointer
 *     format, no room for the 48-byte LC_DYLD_INFO_ONLY, no __LINKEDIT --
 *     and 1 only for an operational failure, which here means an allocation
 *     md_declassify could not make, or writing OUT. That is what
 *     EX_REFUSED's contract above asks for, and this verb is free to use it:
 *     unlike dylib/rpath/lc/minos it has never forwarded another program's
 *     exit code, so there is nothing to preserve. A wrapper that must look
 *     like patch_macho maps both nonzero codes to 1.
 *   - THE WRITE. patch_macho creates OUT with open(O_CREAT|O_TRUNC, 0755) and
 *     writes into it; a write that fails partway leaves a truncated OUT
 *     behind. This verb goes through wa_write_atomic (src/atomic_write.h) --
 *     a temp file in OUT's directory, chmod 0755, renamed over OUT -- so a
 *     failed run leaves no half-written output, and an OUT that already
 *     exists keeps its xattrs. The BYTES written are identical either way.
 *   - "Wrote ..." ON THE PASS-THROUGH PATH. patch_macho prints its "Wrote %s
 *     (%zu bytes)" line only when it actually converted something; a
 *     pass-through says "Already patched ... passing through." and nothing
 *     else, so the file it just wrote is never named. This verb reports every
 *     write, including the pass-through, because a verb that copies a file
 *     without saying so is the silent-success shape docs/PROPOSAL.md's
 *     `verify` section exists to rule out.
 *   - THE UNREADABLE-INPUT MESSAGE. patch_macho prints "IN: not a readable
 *     64-bit Mach-O"; this verb prefixes it, as every other verb here does.
 *     md_declassify deliberately prints nothing for that case so each
 *     front-end can name the file in its own words.
 *
 * The MDCL_ codes are tested BY NAME below, never as `rc < 0` or `rc != 0` --
 * declassify.h says why: MDCL_PASSTHROUGH is a nonzero SUCCESS, and a code
 * added later must not silently become either a success or the wrong kind of
 * failure. */
static int cmd_declassify(const char *in, const char *out) {
    uint8_t *buf = NULL;
    size_t len = 0;
    int rc = md_declassify(in, &buf, &len);

    if (rc == MDCL_NOT_MACHO) {
        fprintf(stderr, "macho9 declassify: %s: not a readable 64-bit Mach-O. "
                        "This verb is thin-only, like patch_macho, so that covers "
                        "a fat container as well as anything that is not a Mach-O "
                        "at all.\n", in);
        return EX_REFUSED;
    }
    if (rc == MDCL_REFUSED) return EX_REFUSED;  /* md_declassify already said why */
    /* An allocation md_declassify could not make. Also already reported, but
     * NOT a refusal: EX_REFUSED's contract above rules out using it for "a
     * malloc that failed", and a caller scripting around "this file just isn't
     * one macho9 will touch" would be told the wrong thing. */
    if (rc == MDCL_ERROR) return 1;
    if (rc != MDCL_CONVERTED && rc != MDCL_PASSTHROUGH) {
        /* A code declassify.h grew that this verb has not been taught. Refuse
         * rather than write an output file from a buffer md_declassify never
         * promised to fill -- an unrecognized code is precisely the case the
         * by-name rule above exists for, and guessing which side of refusal it
         * belongs on is not this verb's call to make. */
        fprintf(stderr, "macho9 declassify: %s: md_declassify returned an "
                        "unrecognized code %d; refusing rather than writing an "
                        "output this verb cannot vouch for\n", in, rc);
        return 1;
    }

    if (wa_write_atomic(out, 0755, buf, len) != 0) {
        /* wa_write_atomic already reported which syscall failed. This is an
         * operational failure, not a refusal: nothing about the input was
         * wrong. */
        free(buf);
        return 1;
    }
    printf("Wrote %s (%zu bytes)\n", out, len);
    free(buf);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--capabilities") == 0)
        return print_capabilities();

    if (argc < 2) { usage(argv[0]); return 1; }
    const char *verb = argv[1];

    if (strcmp(verb, "verify") == 0) {
        if (argc != 3) { fprintf(stderr, "usage: %s verify FILE\n", argv[0]); return 1; }
        return cmd_verify(argv[2]);
    }
    if (strcmp(verb, "info") == 0) {
        if (argc != 3) { fprintf(stderr, "usage: %s info FILE\n", argv[0]); return 1; }
        return cmd_info(argv[2]);
    }
    if (strcmp(verb, "grow") == 0) {
        if (argc != 4) { fprintf(stderr, "usage: %s grow FILE N\n", argv[0]); return 1; }
        return cmd_grow(argv[2], argv[3]);
    }
    if (strcmp(verb, "minos") == 0) {
        if (argc != 4) { fprintf(stderr, "usage: %s minos FILE 10.9\n", argv[0]); return 1; }
        return cmd_minos(argv[2], argv[3]);
    }
    if (strcmp(verb, "segment") == 0) {
        if (argc != 5) { fprintf(stderr, "usage: %s segment FILE OLD NEW\n", argv[0]); return 1; }
        return cmd_segment(argv[2], argv[3], argv[4]);
    }
    if (strcmp(verb, "retag-swift") == 0) {
        if (argc != 3) { fprintf(stderr, "usage: %s retag-swift FILE\n", argv[0]); return 1; }
        return cmd_retag_swift(argv[2]);
    }
    if (strcmp(verb, "lc") == 0) {
        if (argc < 5) { fprintf(stderr, "usage: %s lc FILE [--fatal-warnings] -delete KIND [-delete KIND...]\n", argv[0]); return 1; }
        return cmd_lc(argc, argv);
    }
    if (strcmp(verb, "dylib") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: %s dylib FILE [--allow-grow] [--fatal-warnings] OP...\n", argv[0]); return 1; }
        return cmd_dylib_or_rpath(argc, argv, 0);
    }
    if (strcmp(verb, "rpath") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: %s rpath FILE [--allow-grow] [--fatal-warnings] OP...\n", argv[0]); return 1; }
        return cmd_dylib_or_rpath(argc, argv, 1);
    }
    if (strcmp(verb, "declassify") == 0) {
        if (argc != 4) { fprintf(stderr, "usage: %s declassify IN OUT\n", argv[0]); return 1; }
        return cmd_declassify(argv[2], argv[3]);
    }

    fprintf(stderr, "macho9: unknown verb '%s'\n", verb);
    usage(argv[0]);
    return 1;
}
