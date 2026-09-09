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
#include "macho_grow.h"

/* Not declared in every SDK's mach-o/loader.h (10.9's predates them). Same
 * fallback values change_dylib.c and patch_macho.c already carry -- data, not
 * logic, so duplicating it here is the same call this codebase already made. */
#ifndef LC_SOURCE_VERSION
#define LC_SOURCE_VERSION 0x2A
#endif
#ifndef LC_BUILD_VERSION
#define LC_BUILD_VERSION 0x32
#endif
#ifndef LC_DYLIB_CODE_SIGN_DRS
#define LC_DYLIB_CODE_SIGN_DRS 0x2B
#endif
#ifndef LC_LOAD_UPWARD_DYLIB
#define LC_LOAD_UPWARD_DYLIB (0x23 | LC_REQ_DYLD)
#endif

/* The KIND vocabulary `lc -delete` accepts -- verbatim from the brief's
 * grammar, and exactly what change_dylib's -strip-lc already understands, so
 * lc's translation to it is a rename, not a new decision. */
static const struct { const char *kind; uint32_t cmd; } LC_KINDS[] = {
    { "uuid",           LC_UUID                },
    { "codesig",        LC_CODE_SIGNATURE      },
    { "source-version", LC_SOURCE_VERSION      },
    { "build-version",  LC_BUILD_VERSION       },
    { "code-sign-drs",  LC_DYLIB_CODE_SIGN_DRS },
};
#define N_LC_KINDS (sizeof(LC_KINDS) / sizeof(LC_KINDS[0]))

/* ---- capabilities -------------------------------------------------------
 *
 * Stable, line-oriented, greppable -- shell is the wrapper's own language, so
 * this is not JSON. Contract:
 *
 *   line 1: "format <N>"       -- bump N only if a later build changes this
 *                                  TEXT's shape in a way old parsing breaks.
 *   line 2+: "verb <name> [key=value ...]"
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
    printf("verb verify\n");
    printf("verb info\n");
    printf("verb grow\n");
    if (have_add_version_min)
        printf("verb minos versions=10.9\n");
    if (have_change_dylib) {
        printf("verb lc ops=delete kinds=uuid,codesig,source-version,build-version,code-sign-drs\n");
        printf("verb dylib ops=replace,delete,append,insert,reexport flags=allow-grow\n");
        printf("verb rpath ops=replace,delete,append flags=allow-grow\n");
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
    char exe[PATH_MAX];
    uint32_t sz = sizeof(exe);
    if (_NSGetExecutablePath(exe, &sz) != 0) return -1;
    char resolved[PATH_MAX];
    if (!realpath(exe, resolved)) return -1;
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
        return 1;
    }
    int rc = mg_plausible(im.buf, im.size);
    printf("%s: %s\n", path, rc == 0 ? "OK" : "FAILED (see above)");
    mi_close(&im);
    return rc == 0 ? 0 : 1;
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

/* dylib_command/rpath_command names are a lc_str offset relative to the
 * command's own start; bounds-check against cmdsize before trusting it, same
 * caution change_dylib.c's build_lcs takes reading the same fields. */
static const char *lc_str_at(const struct load_command *lc, uint32_t offset) {
    if (offset >= lc->cmdsize) return "(malformed: offset past cmdsize)";
    return (const char *)lc + offset;
}

static void info_cb(const struct load_command *lc, void *ctx_) {
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
}

static int cmd_info(const char *path) {
    mi_image im;
    if (mi_open(path, &im) != 0) {
        fprintf(stderr, "macho9 info: %s: not a readable 64-bit Mach-O\n", path);
        return 1;
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
 * reports success (macho_grow.h "Phase 4: prove it"), so there is nothing
 * left for this verb to check on top -- it opens, calls the real primitive,
 * and writes back only on success. On failure mg_grow_header has already
 * explained why on stderr and left *pbuf as whatever is safe to discard;
 * the file itself is never touched. */
static int cmd_grow(const char *path, const char *n_str) {
    char *end;
    unsigned long n = strtoul(n_str, &end, 10);
    if (*end != '\0' || n == 0 || n > UINT32_MAX) {
        fprintf(stderr, "macho9 grow: N must be a positive byte count (got '%s')\n", n_str);
        return 1;
    }

    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("macho9 grow: open"); return 1; }

    mi_image im;
    if (mi_open(path, &im) != 0) {
        fprintf(stderr, "macho9 grow: %s: not a readable 64-bit Mach-O\n", path);
        close(fd);
        return 1;
    }
    size_t fsize = im.size;
    uint8_t *buf = mi_release(&im);

    if (mg_grow_header(&buf, &fsize, (uint32_t)n) != 0) {
        fprintf(stderr, "macho9 grow: %s left unmodified\n", path);
        free(buf);
        close(fd);
        return 1;
    }

    if (ftruncate(fd, (off_t)fsize) != 0) { perror("macho9 grow: ftruncate"); free(buf); close(fd); return 1; }
    lseek(fd, 0, SEEK_SET);
    if (write(fd, buf, fsize) != (ssize_t)fsize) { perror("macho9 grow: write"); free(buf); close(fd); return 1; }
    close(fd);
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
        return 1;
    }
    char *argv[3];
    argv[1] = (char *)path;
    argv[2] = NULL;
    return run_sibling("add_version_min", argv);
}

/* ---- lc -delete: delegates to change_dylib -strip-lc --------------------
 *
 * The KIND vocabulary is validated here (against the very table -- LC_KINDS
 * -- that also drives --capabilities) before anything runs, so a bad KIND
 * fails with this verb's own message rather than change_dylib's. */
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
            for (kk = 0; kk < N_LC_KINDS; kk++)
                if (strcmp(kind, LC_KINDS[kk].kind) == 0) break;
            if (kk == N_LC_KINDS) {
                fprintf(stderr, "macho9 lc: unknown KIND '%s' (expected one of:", kind);
                for (kk = 0; kk < N_LC_KINDS; kk++) fprintf(stderr, " %s", LC_KINDS[kk].kind);
                fprintf(stderr, ")\n");
                free(child);
                return 1;
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
        } else if (strcmp(tok, "-replace") == 0 && i + 2 < argc) {
            child[k++] = is_rpath ? "-change-rpath" : "-change";
            child[k++] = argv[i + 1];
            child[k++] = argv[i + 2];
            nops++;
            i += 3;
        } else if (strcmp(tok, "-delete") == 0 && i + 1 < argc) {
            child[k++] = is_rpath ? "-delete-rpath" : "-delete";
            child[k++] = argv[i + 1];
            nops++;
            i += 2;
        } else if (strcmp(tok, "-append") == 0 && i + 1 < argc) {
            child[k++] = is_rpath ? "-add-rpath" : "-add";
            child[k++] = argv[i + 1];
            nops++;
            i += 2;
        } else if (strcmp(tok, "-insert") == 0 && i + 1 < argc) {
            if (is_rpath) {
                fprintf(stderr, "macho9 rpath: -insert is not implemented in this build "
                                "(change_dylib has no rpath-insert to delegate to; a "
                                "workaround is deleting and re-adding every other -rpath "
                                "to reshuffle them, per docs/PROPOSAL.md)\n");
                free(child);
                return 1;
            }
            child[k++] = "-insert";
            child[k++] = argv[i + 1];
            nops++;
            i += 2;
        } else if (!is_rpath && strcmp(tok, "-reexport") == 0 && i + 1 < argc) {
            child[k++] = "-reexport";
            child[k++] = argv[i + 1];
            nops++;
            i += 2;
        } else {
            fprintf(stderr, "macho9 %s: unknown or incomplete operation '%s'\n",
                    is_rpath ? "rpath" : "dylib", tok);
            free(child);
            return 1;
        }
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
