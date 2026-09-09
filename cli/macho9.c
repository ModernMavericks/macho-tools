/*
 * macho9 — the multi-call CLI the seven rewriters converge behind.
 *
 * Grammar settled in docs/PROPOSAL.md ("Verbs"); task-4-brief.md gives the
 * verbatim subset this build targets:
 *
 *   macho9 declassify IN OUT
 *   macho9 dylib FILE [--allow-grow] OP...   -replace -delete -append -insert -reexport
 *   macho9 rpath FILE [--allow-grow] OP...   -replace -delete -append -insert
 *   macho9 lc FILE -delete KIND
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
 * DELEGATION, not reimplementation. `verify`, `info` and `grow` call straight
 * into the primitives Tasks 1-3 already built and tested (mg_plausible,
 * mi_open/mi_each_lc, mg_grow_header) -- they are thin shells over code that
 * already exists in this repo.
 *
 * `dylib`, `rpath` and `lc` are different: the load-command rewrite and the
 * library-ordinal renumbering they need live only inside change_dylib.c's
 * monolithic main(), not as a reusable function -- extracting that is its own
 * piece of future work, not this one (change_dylib.c is deliberately not in
 * this task's file list). So these three verbs delegate by RUNNING the
 * already-tested change_dylib binary as a subprocess, translating this
 * grammar's flags into its. That is still delegation, not duplication: the
 * ordinal-renumbering logic that has twice shipped loader-crashing bugs
 * (docs/PROPOSAL.md "verify") is exercised exactly once, in change_dylib,
 * however it is reached. `change_dylib` is expected to sit right next to
 * `macho9` (both install to bin/; both land in the same CMake build dir) --
 * see sibling_path() below.
 *
 * `declassify` (patch_macho's chained-fixups conversion, ~350 lines of its
 * own) is NOT implemented here yet; it errors clearly rather than pretending.
 * Run patch_macho directly in the meantime.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include "image.h"
#include "ordinals.h"
#include "grow.h"
#include "lc_kinds.h"
#include "atomic_write.h"
#include "mach_compat.h"

/* Exit codes. 0 is success, as always. Everything else used to be a flat 1,
 * which meant a caller checking only "did this exit nonzero" (still fully
 * supported -- see below) could not tell "macho9 examined FILE and declined,
 * on purpose, because of what it found" (not a Mach-O, not plausible, a
 * KIND/version this build doesn't support, mg_grow_header's own designed
 * refusal) apart from "something actually went wrong running macho9 itself"
 * (couldn't open/read/write, malloc failed, fork/exec failed, a usage
 * error). Refusal is load-bearing throughout this codebase -- "-grow refuses
 * rather than guesses" is a global rule, not an incidental behavior -- so a
 * caller that wants to script around "this file just isn't one macho9 will
 * touch" (vs. "retry, or investigate an environment problem") deserves a way
 * to tell the two apart without scraping stderr text, which --capabilities
 * already exists to make unnecessary for everything else this binary
 * reports. EX_REFUSED is used ONLY at a point where macho9 itself examined
 * the input and made that call; it is never used for a genuine operational
 * failure (a syscall that failed, a missing sibling binary, a fork/waitpid
 * error, a bad number of command-line arguments) or for a delegated verb's
 * (dylib/rpath/lc) exit code, which is simply forwarded from the change_dylib
 * subprocess as before -- that subprocess does not make this distinction
 * itself, so forwarding it verbatim keeps this from claiming a precision it
 * does not have. 1 keeps meaning exactly what it always did, so a caller
 * that only checks "== 0" or "!= 0" needs no changes; --capabilities
 * documents both codes (see print_capabilities below) and tests/README.md
 * repeats it for humans. */
#define EX_REFUSED 2

/* The KIND vocabulary `lc -delete` accepts is LC_STRIP_KINDS (src/lc_kinds.h),
 * shared with change_dylib's -strip-lc -- so lc's translation to it is a
 * rename, not a new decision, and there is exactly one table to edit if the
 * vocabulary ever changes. print_capabilities() below reads the same table
 * to build its "kinds=" list, rather than keeping a separate string that can
 * silently drift from what this function (and change_dylib) actually
 * accept -- that drift is exactly what a whole-branch review found here. */

/* Operations `dylib`/`rpath` accept, and their change_dylib translation --
 * ONE table drives both cmd_dylib_or_rpath's parser (below) and
 * print_capabilities' "ops=" list, for the same reason LC_STRIP_KINDS is
 * shared: two hand-maintained lists (the parser's if/else chain and a
 * hardcoded ops= string) had already diverged from each other by the time of
 * review. A NULL child flag for a mode means "not supported in that mode" --
 * rpath has no -insert or -reexport equivalent in change_dylib, so both are
 * simply absent from rpath's derived ops= list and refused by the parser. */
struct dylib_op {
    const char *flag;         /* this grammar's -OP spelling, e.g. "-replace" */
    const char *cap_name;     /* same op's spelling in ops=, e.g. "replace" */
    int nargs;                /* args consumed after the flag: 1 or 2 */
    const char *dylib_child;  /* change_dylib flag when is_rpath==0, or NULL */
    const char *rpath_child;  /* change_dylib flag when is_rpath==1, or NULL */
};
static const struct dylib_op DYLIB_OPS[] = {
    { "-replace",  "replace",  2, "-change",   "-change-rpath" },
    { "-delete",   "delete",   1, "-delete",   "-delete-rpath" },
    { "-append",   "append",   1, "-add",      "-add-rpath"    },
    { "-insert",   "insert",   1, "-insert",   NULL            },
    { "-reexport", "reexport", 1, "-reexport", NULL            },
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
        if (is_rpath ? !DYLIB_OPS[i].rpath_child : !DYLIB_OPS[i].dylib_child) continue;
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
 *       (non-delegated) exit codes mean: ok=0 always; refused=EX_REFUSED is
 *       used only where macho9 itself examined FILE and declined on purpose
 *       (bad magic, implausible, an unsupported KIND/version, a grow
 *       mg_grow_header itself refused); failed=1 is everything else
 *       (syscall/malloc/fork failure, usage error) -- unchanged from before
 *       this line existed, so a caller checking only nonzero needs no
 *       changes. dylib/rpath/lc forward whatever change_dylib returned,
 *       which does not yet make this distinction, so their exit codes are
 *       not covered by this line. See EX_REFUSED's own comment for the full
 *       reasoning.
 *   line 3+: "verb <name> [key=value ...]"
 *       one line per verb this build actually implements. A verb's absence
 *       means "not implemented" -- never advertise one that errors out.
 *       Recognized attributes:
 *         ops=a,b,c      the -OP flags this verb accepts (comma-separated,
 *                         no spaces)
 *         kinds=a,b,c    (lc only) the KIND vocabulary -delete accepts
 *         versions=a,b   (minos only) the floors this build can target
 *         flags=a,b      verb-level flags, e.g. allow-grow
 *
 * `dylib` lists all five brief ops; `rpath` deliberately omits `insert` --
 * change_dylib has no rpath-insert to delegate to (docs/PROPOSAL.md calls
 * rpath -insert "a new capability" change_dylib never had), so this build
 * does not claim it.
 *
 * dylib/rpath/lc/minos all depend on a sibling binary being reachable next
 * to macho9 (see sibling_path() below) -- if it isn't, every one of them
 * fails at runtime no matter what this build's own code can do. Advertising
 * them unconditionally would violate this function's own contract ("never
 * advertise one that errors out") the moment macho9 is packaged or copied
 * apart from change_dylib/add_version_min: a wrapper that trusts the probe
 * would see `verb dylib`, use it, and watch every rewrite fail -- exactly
 * the lockstep failure --capabilities exists to prevent. So each of those
 * four is gated on the sibling it needs actually being there and
 * executable, checked fresh on every call (cheap: one stat). */
static int sibling_path(const char *name, char *out, size_t outsz);

static int sibling_exists(const char *name) {
    char path[PATH_MAX];
    if (sibling_path(name, path, sizeof(path)) != 0) return 0;
    return access(path, X_OK) == 0;
}

static int print_capabilities(void) {
    int have_change_dylib = sibling_exists("change_dylib");
    int have_add_version_min = sibling_exists("add_version_min");

    printf("format 1\n");
    printf("exitcodes ok=0 refused=%d failed=1\n", EX_REFUSED);
    printf("verb verify\n");
    printf("verb info\n");
    printf("verb grow\n");
    if (have_add_version_min)
        printf("verb minos versions=10.9\n");
    if (have_change_dylib) {
        printf("verb lc ops=delete kinds=");
        print_kinds_csv();
        printf("\n");
        printf("verb dylib ops=");
        print_ops_csv(0);
        printf(" flags=allow-grow\n");
        printf("verb rpath ops=");
        print_ops_csv(1);
        printf(" flags=allow-grow\n");
    }
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s --capabilities\n"
        "       %s declassify IN OUT                       (not implemented in this build)\n"
        "       %s dylib FILE [--allow-grow] OP...          -replace OLD NEW | -delete PATH |\n"
        "                                                    -append PATH | -insert PATH | -reexport PATH\n"
        "       %s rpath FILE [--allow-grow] OP...          -replace OLD NEW | -delete PATH | -append PATH\n"
        "       %s lc FILE -delete KIND [-delete KIND...]   uuid | codesig | source-version |\n"
        "                                                    build-version | code-sign-drs\n"
        "       %s grow FILE N\n"
        "       %s minos FILE 10.9\n"
        "       %s info FILE\n"
        "       %s verify FILE\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* ---- locating the sibling tools we delegate to --------------------------
 *
 * dylib/rpath/lc delegate to change_dylib by running it, not linking it (see
 * file header). It is expected right next to macho9: both are in MACHO_TOOLS'
 * install(TARGETS ... RUNTIME DESTINATION bin) and both land in the same
 * CMake build directory pre-install. _NSGetExecutablePath + realpath finds
 * macho9's own true location regardless of how it was invoked (bare name via
 * PATH, relative, symlinked), which a naive argv[0] read would not. */
static int sibling_path(const char *name, char *out, size_t outsz) {
    char stackbuf[PATH_MAX];
    char *exe = stackbuf;
    uint32_t sz = sizeof(stackbuf);
    char *heapbuf = NULL;
    if (_NSGetExecutablePath(exe, &sz) != 0) {
        /* Too small: _NSGetExecutablePath's documented contract is to set
         * `sz` to the size that WOULD have worked when it returns -1, and
         * this used to just give up right here instead of using that. A
         * PATH_MAX stack buffer covers essentially every real install, but
         * the whole reason this resolves the executable path at all (rather
         * than trusting argv[0]) is to keep finding the sibling tool
         * regardless of how this binary was invoked or how deep it lives --
         * so honor the retry the API is explicitly offering rather than
         * failing on a case it already told us how to handle. */
        heapbuf = (char *)malloc(sz);
        if (!heapbuf) return -1;
        exe = heapbuf;
        if (_NSGetExecutablePath(exe, &sz) != 0) { free(heapbuf); return -1; }
    }
    char resolved[PATH_MAX];
    int rok = (realpath(exe, resolved) != NULL);
    free(heapbuf);
    if (!rok) return -1;
    char dirbuf[PATH_MAX];
    strncpy(dirbuf, resolved, sizeof(dirbuf) - 1);
    dirbuf[sizeof(dirbuf) - 1] = '\0';
    char *dir = dirname(dirbuf);   /* may return a pointer into dirbuf, or static storage */
    int n = snprintf(out, outsz, "%s/%s", dir, name);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

/* Run sibling `name` with `argv` (argv[0] conventionally the resolved path;
 * NULL-terminated) and wait for it, forwarding its exit status. Its stdout
 * and stderr are inherited, so its diagnostics -- "Header pad: N bytes
 * available", ordinal-renumbering summaries, refusal reasons -- reach the
 * caller exactly as they would running that tool directly. */
static int run_sibling(const char *name, char **argv) {
    char path[PATH_MAX];
    if (sibling_path(name, path, sizeof(path)) != 0) {
        fprintf(stderr, "macho9: could not determine my own location to find '%s'\n", name);
        return 1;
    }
    if (access(path, X_OK) != 0) {
        fprintf(stderr, "macho9: '%s' not found next to macho9 (expected at %s); this "
                        "build delegates to it and cannot run without it\n", name, path);
        return 1;
    }
    argv[0] = path;
    pid_t pid = fork();
    if (pid < 0) { perror("macho9: fork"); return 1; }
    if (pid == 0) {
        execv(path, argv);
        perror("macho9: execv");
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) { perror("macho9: waitpid"); return 1; }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "macho9: %s terminated by signal %d\n", name, WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 1;
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

/* ---- minos: delegates to add_version_min --------------------------------
 *
 * add_version_min only knows how to target 10.9 (it hardcodes
 * LC_VERSION_MIN_MACOSX 10.9.0 -- see add_version_min.c), which is exactly
 * why this verb's grammar spells the floor literally rather than taking any
 * version: there is only one this build can honor, so refusing anything else
 * up front is a clearer failure than delegating and hoping. */
static int cmd_minos(const char *path, const char *version) {
    if (strcmp(version, "10.9") != 0) {
        fprintf(stderr, "macho9 minos: only 10.9 is supported by this build (got '%s')\n", version);
        return EX_REFUSED;
    }
    char *argv[3];
    argv[1] = (char *)path;
    argv[2] = NULL;
    return run_sibling("add_version_min", argv);
}

/* ---- lc -delete: delegates to change_dylib -strip-lc --------------------
 *
 * The KIND vocabulary is validated here (against the very table --
 * LC_STRIP_KINDS, shared with change_dylib itself -- that also drives
 * --capabilities) before anything runs, so a bad KIND fails with this
 * verb's own message rather than change_dylib's. */
static int cmd_lc(int argc, char **argv) {
    /* argv[0]=macho9 argv[1]="lc" argv[2]=FILE argv[3..]=ops */
    const char *path = argv[2];
    char **child = malloc((size_t)(argc + 2) * sizeof(char *));
    if (!child) { perror("macho9 lc: malloc"); return 1; }
    int k = 0;
    child[k++] = NULL;             /* argv[0]: filled in by run_sibling */
    child[k++] = (char *)path;
    int ndeletes = 0;
    for (int i = 3; i < argc; ) {
        if (strcmp(argv[i], "-delete") == 0 && i + 1 < argc) {
            const char *kind = argv[i + 1];
            size_t kk;
            for (kk = 0; kk < LC_STRIP_KINDS_COUNT; kk++)
                if (strcmp(kind, LC_STRIP_KINDS[kk].name) == 0) break;
            if (kk == LC_STRIP_KINDS_COUNT) {
                fprintf(stderr, "macho9 lc: unknown KIND '%s' (expected one of:", kind);
                for (kk = 0; kk < LC_STRIP_KINDS_COUNT; kk++) fprintf(stderr, " %s", LC_STRIP_KINDS[kk].name);
                fprintf(stderr, ")\n");
                free(child);
                return EX_REFUSED;
            }
            child[k++] = "-strip-lc";
            child[k++] = (char *)kind;
            ndeletes++;
            i += 2;
        } else {
            fprintf(stderr, "macho9 lc: unknown operation '%s' (only -delete KIND is supported)\n", argv[i]);
            free(child);
            return 1;
        }
    }
    if (ndeletes == 0) {
        fprintf(stderr, "macho9 lc: need at least one -delete KIND\n");
        free(child);
        return 1;
    }
    child[k] = NULL;
    int rc = run_sibling("change_dylib", child);
    free(child);
    return rc;
}

/* ---- dylib / rpath: delegate to change_dylib ----------------------------
 *
 * Token-for-token translation into change_dylib's existing flags (see
 * change_dylib.c's usage comment for the -change/-add/-insert/-delete/
 * -reexport family and its -*-rpath twins). `--allow-grow` maps to -grow;
 * position within the OP list does not matter to change_dylib's own parser
 * (it loops over all of argv[2:] regardless of order), so this does not
 * enforce a position either.
 *
 * rpath -insert has no change_dylib equivalent (see print_capabilities'
 * comment) -- refuse it explicitly rather than silently downgrading it to
 * -append, which would put the new search path LAST instead of FIRST and
 * change what the brief calls "a new capability" into a wrong answer that
 * merely runs.
 */
static int cmd_dylib_or_rpath(int argc, char **argv, int is_rpath) {
    const char *path = argv[2];
    /* Upper bound: every input token maps to at most one output token, plus
     * the FILE and the NULL terminator. */
    char **child = malloc((size_t)(argc + 2) * sizeof(char *));
    if (!child) { perror("macho9: malloc"); return 1; }
    int k = 0;
    child[k++] = NULL;             /* argv[0]: filled in by run_sibling */
    child[k++] = (char *)path;
    /* Counts actual -replace/-delete/-append/-insert/-reexport ops only --
     * NOT --allow-grow. `k` alone can't distinguish "no ops" from "only
     * --allow-grow" (both leave k > 2), which previously let
     * `dylib FILE --allow-grow` with nothing else fall through to
     * change_dylib and print ITS usage -- leaking the exact -change/-add/
     * -strip-lc spellings this grammar deliberately doesn't offer. */
    int nops = 0;

    for (int i = 3; i < argc; ) {
        const char *tok = argv[i];
        if (strcmp(tok, "--allow-grow") == 0) {
            child[k++] = "-grow";
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
            free(child);
            return 1;
        }
        const struct dylib_op *op = &DYLIB_OPS[oi];
        const char *cflag = is_rpath ? op->rpath_child : op->dylib_child;
        if (!cflag) {
            if (is_rpath && strcmp(op->flag, "-insert") == 0) {
                fprintf(stderr, "macho9 rpath: -insert is not implemented in this build "
                                "(change_dylib has no rpath-insert to delegate to; a "
                                "workaround is deleting and re-adding every other -rpath "
                                "to reshuffle them, per docs/PROPOSAL.md)\n");
            } else {
                fprintf(stderr, "macho9 %s: unknown or incomplete operation '%s'\n",
                        is_rpath ? "rpath" : "dylib", tok);
            }
            free(child);
            return 1;
        }
        child[k++] = (char *)cflag;
        for (int a = 1; a <= op->nargs; a++) child[k++] = argv[i + a];
        nops++;
        i += 1 + op->nargs;
    }
    if (nops == 0) {
        fprintf(stderr, "macho9 %s: need at least one operation\n", is_rpath ? "rpath" : "dylib");
        free(child);
        return 1;
    }
    child[k] = NULL;
    int rc = run_sibling("change_dylib", child);
    free(child);
    return rc;
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
    if (strcmp(verb, "lc") == 0) {
        if (argc < 5) { fprintf(stderr, "usage: %s lc FILE -delete KIND [-delete KIND...]\n", argv[0]); return 1; }
        return cmd_lc(argc, argv);
    }
    if (strcmp(verb, "dylib") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: %s dylib FILE [--allow-grow] OP...\n", argv[0]); return 1; }
        return cmd_dylib_or_rpath(argc, argv, 0);
    }
    if (strcmp(verb, "rpath") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: %s rpath FILE [--allow-grow] OP...\n", argv[0]); return 1; }
        return cmd_dylib_or_rpath(argc, argv, 1);
    }
    if (strcmp(verb, "declassify") == 0) {
        fprintf(stderr, "macho9 declassify: not implemented in this build (patch_macho's "
                        "chained-fixups conversion has not been ported to macho9 yet; run "
                        "patch_macho IN OUT directly)\n");
        return 1;
    }

    fprintf(stderr, "macho9: unknown verb '%s'\n", verb);
    usage(argv[0]);
    return 1;
}
