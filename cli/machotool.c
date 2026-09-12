/*
 * machotool — the multi-call CLI the seven rewriters converge behind.
 *
 * Grammar settled in docs/PROPOSAL.md ("Verbs"). The subset this build
 * actually implements, verbatim:
 *
 *   machotool declassify IN OUT
 *   machotool dylib FILE OUT [--allow-grow] [--fatal-warnings] OP...   -replace -delete -append -insert -reexport
 *   machotool rpath FILE OUT [--allow-grow] [--fatal-warnings] OP...   -replace -delete -append -insert
 *   machotool segment FILE OUT OLD NEW
 *   machotool retag-swift FILE OUT
 *   machotool lc FILE OUT [--fatal-warnings] -delete KIND
 *   machotool grow FILE OUT N
 *   machotool minos FILE OUT 10.9 [--allow-grow]
 *   machotool info FILE
 *   machotool verify FILE
 *   machotool edit FILE OUT SCRIPT [--verbose]
 *
 * Not every line above is implemented by every build -- `machotool --capabilities`
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
 * (src/declassify.h); `edit` into ms_parse (src/script.h) and me_run
 * (src/edit.h), which reaches the in-memory cores of the same
 * implementations. Each verb is a thin shell over code
 * that already exists in this repo, and each translates this grammar into the
 * ONE
 * implementation -- so the ordinal-renumbering logic that has twice shipped
 * loader-crashing bugs (docs/PROPOSAL.md "verify") is exercised exactly once,
 * however it is reached.
 *
 * Those last four verbs used to be delegated by RUNNING change_dylib and
 * add_version_min as subprocesses, found next to machotool on disk. That made
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
 * keeps only its `IN OUT` grammar, its messages and its flat exit code. It was
 * the FIRST verb here to read one file and write another rather than rewriting
 * in place, so it writes OUT itself, through wa_write_new, instead of going
 * through mr_apply_file.
 *
 * THAT SHAPE IS NOW THE RULE, not declassify's exception: a rewriting verb
 * names OUT as the positional right after FILE and never writes FILE. `minos`
 * and `retag-swift` converted first, then `dylib`, `rpath`, `lc` and `segment`
 * together (all four are one mr_apply_file call), then `grow` -- which gained
 * an OUT -- alongside declassify's own refusal of an OUT that is IN, which it
 * used to allow. `edit` converted last, and with it went the last way this
 * binary had of writing the file it was given: its `--output` flag became the
 * OUT positional, and `--dry-run` went with it, a scratch OUT being the same
 * run. So NO verb here writes its input. bad_out holds the refusals every
 * one of them makes about OUT before it reads anything.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
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
#include "script.h"
#include "edit.h"

/* Exit codes. 0 is success, as always. Everything else used to be a flat 1,
 * which meant a caller checking only "did this exit nonzero" (still fully
 * supported -- see below) could not tell "machotool examined FILE and declined,
 * on purpose, because of what it found" (not a Mach-O, not plausible, a
 * KIND/version/segment name this build doesn't support, mg_grow_header's own
 * designed refusal) apart from "something actually went wrong running machotool itself"
 * (couldn't open/read/write, malloc failed, a usage error). Refusal is
 * load-bearing throughout this codebase -- "-grow refuses rather than
 * guesses" is a global rule, not an incidental behavior -- so a
 * caller that wants to script around "this file just isn't one machotool will
 * touch" (vs. "retry, or investigate an environment problem") deserves a way
 * to tell the two apart without scraping stderr text, which --capabilities
 * already exists to make unnecessary for everything else this binary
 * reports.
 *
 * THE SCHEME IS 0 OK, 1 REFUSED, 2 ERROR -- the reverse of what first
 * shipped (0 ok, 1 failed, 2 refused), and deliberately so. `diff`, `grep`
 * and `cmp` all reserve their HIGHEST code for "the tool could not do its
 * job" and a lower one for "a normal, expected, non-success answer"; the
 * original numbering had that backwards. binutils sets no precedent either
 * way -- it returns a flat 0 or 1 and has no notion of a considered refusal
 * at all, so there was no existing convention this binary owed compatibility
 * to. Nothing outside this repo had ever run the compat wrappers this
 * couples to (see MR_REFUSED's own comment, rewrite.h), so this was the last
 * point at which the numbering could change for free -- after `edit` ships,
 * it no longer is.
 *
 * EX_REFUSED is used ONLY at a point where machotool itself examined the input
 * and made that call; it is never used for a genuine operational failure (a
 * syscall that failed, a bad number of command-line arguments), save the one
 * allocation fold described below -- EX_FAIL is that catch-all, named the
 * same way as EX_REFUSED so a future change to either touches one place.
 * That includes the shared rewrite drivers (mr_apply_file,
 * mv_add_version_min) that dylib/rpath/lc/minos hand back:
 * they now draw the SAME line themselves (rewrite.h's own comment on
 * mr_apply_file has the full classification), returning MR_REFUSED
 * (== EX_REFUSED, enforced below) for a considered refusal -- "not a 64-bit
 * Mach-O" in any of its forms, no room to grow, a rewrite's own cross-check
 * failing, and more -- and MR_FAIL (== EX_FAIL, enforced below) for
 * open/fstat/read/write/malloc itself failing. Forwarding either verbatim is
 * exact, not an approximation, with one deliberate exception those two
 * drivers' own comments carry: an allocation failure INSIDE mg_grow_header
 * or mg_plausible (src/grow.c) is folded into MR_REFUSED, same as every
 * other reason either one refuses, not split out to MR_FAIL. The same fold
 * holds on the verbs that call those two directly -- cmd_grow
 * (mg_grow_header) and cmd_verify (mg_plausible) both return EX_REFUSED for
 * any failure of theirs. So a failed allocation that is checked at all is
 * EX_FAIL when it is mi_open's or mi_open_slack's, mfat_parse's,
 * md_declassify's, wa_write_new's temp-name buffer, or one src/rewrite.c's
 * own drivers make (rewrite.h's MR_FAIL comment names them); EX_REFUSED
 * when it is inside mg_grow_header or mg_plausible, by design (see
 * rewrite.c's comment on the fold for why); and no exit code at all when it
 * is wa_write_new's copy of an extended attribute, which only warns. A
 * caller that only checks "== 0" or "!= 0" still needs no changes;
 * --capabilities documents all three codes (see print_capabilities below)
 * and tests/README.md repeats it for humans. */
#define EX_REFUSED 1
#define EX_FAIL    2

/* mr_apply_file's MR_REFUSED (rewrite.h) is forwarded verbatim by
 * cmd_dylib_or_rpath and cmd_lc as this binary's own exit code, so it has to
 * equal EX_REFUSED or --capabilities' documented refused=1 would be a lie
 * for exactly the case --fatal-warnings exists to handle. A mismatch here is
 * a build failure, not a hope -- the same device commit 247d09d used for
 * mg_classify/ml_bump_lc's coupling. */
typedef char mr_refused_is_ex_refused[(MR_REFUSED == EX_REFUSED) ? 1 : -1];

/* Same coupling, same reason, for the other half of mr_apply_file's (and
 * mv_add_version_min's) exit-code vocabulary: every operational failure they
 * report is MR_FAIL (rewrite.h), forwarded verbatim by the same call sites,
 * so it has to equal EX_FAIL or --capabilities' documented failed=2 would be
 * a lie for exactly those failures. */
typedef char mr_fail_is_ex_fail[(MR_FAIL == EX_FAIL) ? 1 : -1];

/* THE TWO THINGS OUT MUST NOT BE, once, for every verb that reads FILE and
 * writes OUT. Both are mistakes about what the tool does rather than about
 * this file's content, so both are refused UP FRONT -- before any read, and
 * before whatever else the verb validates -- which is the difference between
 * "refused, nothing happened" and a refusal that arrives after the work.
 *
 * OUT MUST NOT BE FILE. wa_write_new refuses that again at the write (a path
 * can change in between), but that answer arrives in atomic_write.c's words,
 * after the rewrite; this one arrives in the verb's own, immediately.
 *
 * OUT MUST NOT BEGIN WITH '-'. Nothing here treats a positional as a flag, so
 * `machotool dylib FILE --allow-grow -append /x` -- the old flag-first habit, from
 * before these verbs took an output -- would otherwise CREATE a file called
 * "--allow-grow" and exit 0, having done something the caller plainly did not
 * ask for. The grammar just moved under every caller, so that is the mistake
 * people will actually make, and silently obeying it is the shape of failure
 * this whole toolkit is written to refuse. A caller who really does mean a file
 * whose name starts with a dash can spell it `./-name`, which the message says.
 * FILE gets no such check: it is only read, and mi_open's own failure names it.
 *
 * ONE function rather than the same lines in each verb, because the nine
 * callers must not drift: both wordings are asserted from the outside, per verb
 * (tests/cli_test.sh greps for "never writes its input" and for "which begins
 * with '-'"), and a verb that grew its own phrasing would be a verb whose
 * refusal reads differently for no reason. `verb` is the grammar's own
 * spelling, so the message names the verb the caller typed.
 *
 * Returns 1 when it printed a refusal (the caller returns EX_FAIL), else 0. */
static int bad_out(const char *verb, const char *path, const char *out) {
    if (out[0] == '-') {
        fprintf(stderr, "machotool %s: OUT is '%s', which begins with '-'; OUT is the "
                        "positional right after FILE, not a flag. Write './%s' if a "
                        "file of that name is really meant.\n", verb, out, out);
        return 1;
    }
    if (wa_is_input(path, out)) {
        fprintf(stderr, "machotool %s: %s is %s; machotool never writes its input\n", verb, out, path);
        return 1;
    }
    return 0;
}

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
 *   line 2: "exitcodes ok=0 refused=<N> failed=<M>" -- what this binary's own
 *       exit codes mean: ok=0 always; refused=EX_REFUSED is used wherever
 *       machotool (or a shared rewrite driver it calls into) examined FILE and
 *       declined on purpose -- bad magic, implausible, an unsupported KIND/
 *       version, a grow mg_grow_header itself refused, new load commands
 *       that don't fit and can't be grown, an unmatched --fatal-warnings
 *       operation, and more (rewrite.h's own comment on mr_apply_file has
 *       the full list, including the one exception -- an allocation
 *       failure inside mg_grow_header or mg_plausible themselves stays
 *       refused=EX_REFUSED, not failed, same as every other reason either
 *       one refuses, on grow and verify as well as the rewrite verbs);
 *       failed=EX_FAIL is everything else (syscall/malloc failure, usage
 *       error -- EX_REFUSED's own comment above has the exact allocation
 *       breakdown). The two numbers are 1 and 2, not the reverse
 *       -- see EX_REFUSED's own comment above for why this repo deliberately
 *       does not match what it originally shipped. A caller checking only
 *       nonzero needs no changes regardless of which way the numbers run.
 *       dylib/rpath/lc/minos return the shared rewrite drivers' own code
 *       (mr_apply_file, mv_add_version_min) verbatim, and those drivers now
 *       use this SAME EX_REFUSED/EX_FAIL split themselves (as MR_REFUSED/
 *       MR_FAIL, rewrite.h, enforced equal to these two by the typedefs
 *       below) -- so their exit codes ARE covered by this line, including
 *       the checks machotool makes BEFORE calling them (an unknown lc KIND, a
 *       version other than 10.9) and a dylib/rpath/lc run given
 *       --fatal-warnings, where mr_apply_file returns MR_REFUSED for an
 *       operation that matched nothing -- see that flag's own entry below.
 *       See EX_REFUSED's own comment for the full reasoning.
 *   line 3: "output positional=2 never-writes-input" -- the shape every
 *       rewriting verb's positionals take: FILE, then OUT as the positional
 *       right after it, and OUT=FILE (by path, symlink or hard link) is
 *       always refused. `positional=2` is OUT's position counting from 1;
 *       this line exists so a wrapper checks for it instead of assuming the
 *       shape.
 *   line 4+: "verb <name> [key=value ...]"
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
 *                        (EX_REFUSED), and THERE IS NOTHING TO ROLL BACK:
 *                        mr_apply_file decides that verdict before its one
 *                        write, so a refused run leaves OUT unwritten
 *                        whether one operation matched or none did -- and it
 *                        never writes FILE at all. (It used to refuse AFTER
 *                        rewriting FILE when some other operation matched,
 *                        which is what taking an OUT removed.) Named
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
 *                         `segment reports=renamed`. `edit`'s own flags=
 *                         entry is unrelated to the fatal-warnings paragraph
 *                         above -- `verbose` is a plain CLI switch
 *                         (--verbose), not a match-reporting mode -- listed
 *                         so a wrapper can tell whether this build accepts
 *                         it before passing it.
 *   line N+: "statement <kind> <op> <nargs>"
 *       one line per row of src/script.c's MS_TABLE -- the edit-script
 *       statement vocabulary the `edit` verb's parser (ms_parse) accepts.
 *       Generated by looping over ms_table_row, the same table ms_parse
 *       matches statements against, so this can never advertise a statement
 *       the parser would refuse, or omit one it accepts -- the same reason
 *       `kinds=` and `ops=` above are generated from LC_STRIP_KINDS and
 *       DYLIB_OPS rather than hand-copied. `nargs` is the operand count
 *       after `<kind> <op>`, e.g. "statement dylib replace 2" means `dylib
 *       replace OLD NEW`. Directives (allow-grow, fatal-warnings) are
 *       deliberately not listed here -- that is a later decision.
 *
 * `dylib` lists all five brief ops; `rpath` lists four -- everything but
 * `reexport`, which LC_RPATH's single kind makes meaningless. Both lists are
 * derived from DYLIB_OPS, the same table the parser matches against, so
 * neither can advertise an op the parser would refuse.
 *
 * Every verb listed below is unconditional now. dylib/rpath/lc/minos used to
 * be gated on a sibling binary (change_dylib / add_version_min) being present
 * and executable next to machotool, because that is what they ran to do the
 * work: advertising them when the sibling was missing would have violated
 * this function's own contract ("never advertise one that errors out") the
 * moment machotool was packaged apart from them. Task 0.5 removed the
 * subprocess -- the rewrite is linked in from src/rewrite.c and
 * src/version_min.c now -- so there is no external file left whose absence
 * could make an advertised verb fail, and nothing left to probe. */
static int print_capabilities(void) {
    printf("format 1\n");
    printf("exitcodes ok=0 refused=%d failed=%d\n", EX_REFUSED, EX_FAIL);
    /* Every rewriting verb reads FILE and writes OUT, the positional right
     * after it, and refuses an OUT that is FILE: machotool never writes its
     * input. A wrapper checks for this line rather than assume the shape. */
    printf("output positional=2 never-writes-input\n");
    printf("verb declassify\n");
    printf("verb verify\n");
    printf("verb info\n");
    printf("verb grow\n");
    /* reports=renamed: this verb prints "machotool segment: renamed=<N>" on
     * success, the match count nothing outside the rewriter can derive. See
     * cmd_segment for why, and compat/rename_segment.sh for who needs it. */
    printf("verb segment reports=renamed\n");
    printf("verb retag-swift\n");
    printf("verb minos versions=10.9 flags=allow-grow\n");
    printf("verb lc ops=delete kinds=");
    print_kinds_csv();
    printf(" flags=fatal-warnings\n");
    printf("verb dylib ops=");
    print_ops_csv(0);
    printf(" flags=allow-grow,fatal-warnings\n");
    printf("verb rpath ops=");
    print_ops_csv(1);
    printf(" flags=allow-grow,fatal-warnings\n");
    /* flags=verbose: the one CLI flag `edit` accepts. `output` and `dry-run`
     * were here while OUT was a flag and a run could skip its write; both are
     * gone, and advertising either would tell a wrapper it may pass something
     * this build refuses. */
    printf("verb edit flags=verbose\n");
    {
        int i;
        const char *kind, *op;
        int nargs;
        for (i = 0; ms_table_row(i, &kind, &op, &nargs); i++)
            printf("statement %s %s %d\n", kind, op, nargs);
    }
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s --capabilities\n"
        "       %s declassify IN OUT                        chained fixups -> LC_DYLD_INFO_ONLY;\n"
        "                                                    IN is only read; OUT must not be IN\n"
        "       %s dylib FILE OUT [--allow-grow] [--fatal-warnings] OP...\n"
        "                                                    -replace OLD NEW | -delete PATH |\n"
        "                                                    -append PATH | -insert PATH | -reexport PATH\n"
        "       %s rpath FILE OUT [--allow-grow] [--fatal-warnings] OP...\n"
        "                                                    -replace OLD NEW | -delete PATH |\n"
        "                                                    -append PATH (searched LAST) |\n"
        "                                                    -insert PATH (searched FIRST)\n"
        "       %s segment FILE OUT OLD NEW                 rename every segment named OLD, and\n"
        "                                                    its sections' copy of that name\n"
        "       %s retag-swift FILE OUT                     FILE is only read; OUT must not be FILE\n"
        "       %s lc FILE OUT [--fatal-warnings] -delete KIND [-delete KIND...]\n"
        "                                                    uuid | codesig | source-version |\n"
        "                                                    build-version | code-sign-drs\n"
        "       %s grow FILE OUT N                          FILE is only read; OUT must not be FILE\n"
        "       %s minos FILE OUT 10.9 [--allow-grow]\n"
        "                                                    FILE is only read; OUT must not be FILE\n"
        "       %s info FILE\n"
        "       %s verify FILE\n"
        "       %s edit FILE OUT SCRIPT [--verbose]         apply an edit script to FILE, writing OUT;\n"
        "                                                    FILE is only read; OUT must not be FILE;\n"
        "                                                    SCRIPT may be '-' for stdin\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* ---- verify: a thin shell over mg_plausible -----------------------------
 *
 * Exactly what the brief asks Step 2 to prove: dispatch works, and the verb
 * adds no logic of its own beyond opening the file and reporting the result.
 */
static int cmd_verify(const char *path) {
    mi_image im;
    int mo_rc = mi_open(path, &im);
    if (mo_rc == MI_IO_ERROR) {
        fprintf(stderr, "machotool verify: %s: cannot open or read\n", path);
        return EX_FAIL;
    }
    if (mo_rc != 0) {
        fprintf(stderr, "machotool verify: %s: not a readable 64-bit Mach-O\n", path);
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
    const char *name = lc_cmd_name(lc->cmd);
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
    int mo_rc = mi_open(path, &im);
    if (mo_rc == MI_IO_ERROR) {
        fprintf(stderr, "machotool info: %s: cannot open or read\n", path);
        return EX_FAIL;
    }
    if (mo_rc != 0) {
        fprintf(stderr, "machotool info: %s: not a readable 64-bit Mach-O\n", path);
        return EX_REFUSED;
    }
    printf("%s: %zu bytes, %u load commands, filetype=%u\n",
           path, im.size, im.hdr->ncmds, im.hdr->filetype);
    struct info_ctx ctx = { 0, 0 };
    mi_each_lc(&im, info_cb, &ctx);

    uint32_t first_sect_off = mg_first_sect_off(im.buf, im.size);
    if (first_sect_off == MG_NO_SECTION_DATA) {
        /* Nothing in the image says where the pad ends, so no number would
         * be true; the rewriting verbs refuse such an image for the same
         * reason. */
        printf("header pad: unknown (no section data bounds it)\n");
    } else if (first_sect_off != UINT32_MAX && first_sect_off > im.size) {
        /* The offset is read from the file, and a pad measured to a point
         * past the end of the image would be a number no write could use;
         * the rewriting verbs refuse this image too. */
        printf("header pad: unknown (the first section lies past the end of the image)\n");
    } else if (first_sect_off != UINT32_MAX) {
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
 * and writes OUT only on success. On failure mg_grow_header has already
 * explained why on stderr and left *pbuf as whatever is safe to discard;
 * neither file is touched.
 *
 * FILE IS ONLY READ. This verb used to grow the file it was given, in place;
 * now it reads FILE and writes the grown image to OUT, refusing an OUT that is
 * FILE (bad_out) before anything is read. The write goes through
 * wa_write_new (src/atomic_write.h): a mkstemp()+rename() in OUT's directory,
 * with FILE's mode, owner and xattrs, so OUT is either what it was or the
 * whole grown image, and a symlink at OUT is followed to its target rather
 * than replaced. A caller that wants the old in-place behaviour does what the
 * compat wrappers do -- name a temp beside FILE as OUT, then mv it over. */
static int cmd_grow(const char *path, const char *out, const char *n_str) {
    /* Before the N check, and before any read -- see bad_out. */
    if (bad_out("grow", path, out)) return EX_FAIL;
    char *end;
    unsigned long n = strtoul(n_str, &end, 10);
    if (*end != '\0' || n == 0 || n > UINT32_MAX) {
        fprintf(stderr, "machotool grow: N must be a positive byte count (got '%s')\n", n_str);
        return EX_FAIL;
    }

    /* Opened O_RDONLY -- this verb never writes FILE -- and only to fail fast
     * with open()'s own reason for a missing or unreadable FILE, which mi_open
     * below reports in less detail. Not held for anything: wa_write_new does
     * its own opening, of OUT's directory. */
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("machotool grow: open"); return EX_FAIL; }
    close(fd);

    mi_image im;
    int mo_rc = mi_open(path, &im);
    if (mo_rc == MI_IO_ERROR) {
        /* The open() above only proved this path opens, not that mi_open's own
         * independent open, read of the whole file, or the malloc it reads into
         * will succeed too -- any of those, or an actual TOCTOU race, land
         * here. Not a considered refusal either way. */
        fprintf(stderr, "machotool grow: %s: cannot open or read\n", path);
        return EX_FAIL;
    }
    if (mo_rc != 0) {
        fprintf(stderr, "machotool grow: %s: not a readable 64-bit Mach-O\n", path);
        return EX_REFUSED;
    }
    size_t fsize = im.size;
    uint8_t *buf = mi_release(&im);

    if (mg_grow_header(&buf, &fsize, (uint32_t)n) != 0) {
        /* mg_grow_header's whole design is "refuse rather than guess" (a
         * global rule -- see EX_REFUSED's own comment) -- growth that would
         * need a real __LINKEDIT resize, a non-PIE image, an unsupported
         * ULEB re-encode -- so a failure here is a refusal. That includes
         * every case where mg_grow_header's OWN failure is actually an
         * allocation failing (grow.c): its two reallocations of the whole
         * image, and its side tables -- the address snapshot, the export-
         * trie walk's scratch table, the trie rebuilder's, mg_verify's, and
         * those of the mg_plausible it runs last. This verb cannot tell any
         * of those apart from every other reason mg_grow_header declines,
         * and by deliberate choice does not try to -- see rewrite.c's
         * comment on the identical fold in mr_apply_image (the thin-image
         * step mr_apply_file goes through) for why. So a
         * failed mg_grow_header always exits here, EX_REFUSED, never
         * EX_FAIL. (A failed write, below, is EX_FAIL.) */
        fprintf(stderr, "machotool grow: %s left unmodified\n", path);
        free(buf);
        return EX_REFUSED;
    }

    if (wa_write_new(path, out, buf, fsize) != 0) {
        /* wa_write_new already reported which syscall failed (WA_IS_INPUT is
         * unreachable: bad_out answered it above, and it would have said so
         * too). An operational failure, not a refusal: nothing about the input
         * was wrong. */
        fprintf(stderr, "machotool grow: %s not written\n", out);
        free(buf);
        return EX_FAIL;
    }
    printf("Grew %s: header pad enlarged, file now %zu bytes\n", out, fsize);
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
static int cmd_minos(const char *path, const char *out, const char *version, int allow_grow) {
    /* Before the version check, and before any read -- see bad_out. */
    if (bad_out("minos", path, out)) return EX_FAIL;
    if (strcmp(version, "10.9") != 0) {
        fprintf(stderr, "machotool minos: only 10.9 is supported by this build (got '%s')\n", version);
        return EX_REFUSED;
    }
    return mv_add_version_min(path, out, allow_grow);
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
    /* argv[0]=machotool argv[1]="lc" argv[2]=FILE argv[3]=OUT argv[4..]=ops */
    const char *path = argv[2];
    const char *out = argv[3];
    if (bad_out("lc", path, out)) return EX_FAIL;
    uint32_t strip[MR_MAX_STRIP];
    int nstrip = 0;
    int fatal_warnings = 0;
    for (int i = 4; i < argc; ) {
        if (strcmp(argv[i], "--fatal-warnings") == 0) {
            fatal_warnings = 1;
            i += 1;
        } else if (strcmp(argv[i], "-delete") == 0 && i + 1 < argc) {
            const char *kind = argv[i + 1];
            uint32_t cmd;
            if (lc_kind_by_name(kind, &cmd) != 0) {
                size_t kk;
                fprintf(stderr, "machotool lc: unknown KIND '%s' (expected one of:", kind);
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
                fprintf(stderr, "machotool lc: too many -delete operations (max %d)\n", MR_MAX_STRIP);
                return EX_FAIL;
            }
            strip[nstrip++] = cmd;
            i += 2;
        } else {
            fprintf(stderr, "machotool lc: unknown operation '%s' (only -delete KIND and --fatal-warnings are supported)\n", argv[i]);
            return EX_FAIL;
        }
    }
    if (nstrip == 0) {
        fprintf(stderr, "machotool lc: need at least one -delete KIND\n");
        return EX_FAIL;
    }
    mr_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.strip_cmds = strip;
    ops.n_strip_cmds = nstrip;
    ops.fatal_unmatched = fatal_warnings;
    return mr_apply_file(path, out, &ops);
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
 * Nothing is written when it fires, whether one operation matched or none
 * did: mr_apply_file asks for the verdict before its wa_write_new, so OUT is
 * never created, and FILE it never writes in any case. See
 * mr_ops.fatal_unmatched's own comment in rewrite.h for the full contract.
 */
static int cmd_dylib_or_rpath(int argc, char **argv, int is_rpath) {
    /* argv[0]=machotool argv[1]=verb argv[2]=FILE argv[3]=OUT argv[4..]=ops */
    const char *path = argv[2];
    const char *out = argv[3];
    if (bad_out(is_rpath ? "rpath" : "dylib", path, out)) return EX_FAIL;
    /* Fixed-size, capped exactly where change_dylib's own parser caps (see
     * MR_MAX_OPS in src/rewrite.h) so the two front-ends refuse the same
     * inputs -- but in this grammar's vocabulary, since the wrapper contract
     * is that machotool never leaks change_dylib's flag spellings. */
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

    for (int i = 4; i < argc; ) {
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
            fprintf(stderr, "machotool %s: unknown or incomplete operation '%s'\n",
                    is_rpath ? "rpath" : "dylib", tok);
            return EX_FAIL;
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
            fprintf(stderr, "machotool %s: unknown or incomplete operation '%s'\n",
                    is_rpath ? "rpath" : "dylib", tok);
            return EX_FAIL;
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
            fprintf(stderr, "machotool %s: too many %s operations (max %d)\n",
                    is_rpath ? "rpath" : "dylib", op->flag, MR_MAX_OPS);
            return EX_FAIL;
        }
        nops++;
        i += 1 + op->nargs;
    }
    if (nops == 0) {
        fprintf(stderr, "machotool %s: need at least one operation\n", is_rpath ? "rpath" : "dylib");
        return EX_FAIL;
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
    return mr_apply_file(path, out, &ops);
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
 * does not understand, and already writes its result atomically as a new file
 * that is never the one it read.
 * rename_segment is thin-only and writes through its own fd; this
 * verb gets all three for free, which is what the retirement plan needs before
 * fix_macho's -rename_seg can fold into it.
 *
 * The one check that belongs HERE and not down there is the NEW name's length:
 * a segname field is 16 bytes, and refusing before any I/O -- rather than
 * silently truncating deep inside a per-slice rewrite -- is what
 * rename_segment has always done, in the same place, via the same
 * mseg_name_fits.
 *
 * FIVE DELIBERATE DIVERGENCES FROM rename_segment, all of which a wrapper
 * author has to know about, because reproducing rename_segment's observable
 * behaviour on top of this verb means accounting for each. Each is closed (or
 * knowingly not closed) in compat/rename_segment.sh, whose header says which
 * and why. A sixth USED TO belong on this list -- mg_plausible -- and no
 * longer does; see the note below the bullets.
 *
 *   - WHICH FILE IS WRITTEN. rename_segment rewrote the binary it was given;
 *     this verb reads FILE and writes OUT, and refuses an OUT that is FILE.
 *     A wrapper that has to edit FILE in place does it the way the historical
 *     tool looked like it did: a temp beside FILE as OUT, then mv.
 *   - EXIT CODE WHEN NOTHING MATCHED. mr_apply_file reports "nothing to
 *     change", writes OUT as a copy of FILE, and exits 0; rename_segment
 *     exits 2. This verb hands back the
 *     shared driver's own code, exactly as dylib/rpath/lc do -- but it also
 *     prints `machotool segment: renamed=<N>`, so a wrapper can tell the two
 *     apart exactly rather than by inference. See the count's own comment in
 *     cmd_segment below.
 *   - STDOUT. mr_process_thin prints its own "header pad N bytes available"
 *     and "updated (sizeofcmds=...)" lines, and mr_apply_file its "Wrote OUT
 *     (N bytes)" line; rename_segment prints exactly one line, "%s: renamed %d
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
static int cmd_segment(const char *path, const char *out,
                       const char *oldname, const char *newname) {
    /* Before the name-length check, and before any read -- see bad_out. Both
     * checks refuse before any I/O; this one first, because an unusable OUT is
     * a mistake about the tool rather than about what was asked of it. */
    if (bad_out("segment", path, out)) return EX_FAIL;
    if (!mseg_name_fits(newname)) {
        fprintf(stderr, "machotool segment: new segment name '%s' is longer than the %d bytes "
                        "a segname field holds\n", newname, MSEG_NAME_MAX);
        return EX_REFUSED;
    }
    int renamed = 0;
    mr_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.segment_rename_old = oldname;
    ops.segment_rename_new = newname;
    ops.segment_renamed = &renamed;
    int rc = mr_apply_file(path, out, &ops);
    /* THE MATCH COUNT, MACHINE-READABLE, and the reason mr_ops has an
     * out-param for it at all.
     *
     * A caller cannot derive it. mseg_rename_lc matches with strncmp over the
     * 16-byte segname field, which is neither NUL-terminated nor free of
     * whitespace, so reading a name back out of `machotool info`'s human-readable
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
     * says "nothing to change." and writes OUT as a copy of FILE, which is
     * exactly the case the old grammar reported as exit 2 -- so a wrapper that
     * wants the old answer reads this count and discards that copy). */
    if (rc == 0) printf("machotool segment: renamed=%d\n", renamed);
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
 * ONE DELIBERATE DIVERGENCE FROM retag_swift_classes, which a wrapper author
 * has to know about, because the two front-ends return DIFFERENT codes for
 * the same input: MSWIFT_NOT_MACHO. retag_swift_classes skips such an
 * argument silently and keeps going through the rest of its argv, ending at
 * 0; this verb has exactly one file to talk about, so it refuses (EX_REFUSED)
 * and says why. compat/retag_swift_classes.sh's header says how it maps that
 * back.
 *
 * MSWIFT_NOT_MACHO is tested BY NAME below, never as `n < 0` -- swift_retag.h
 * says why: a future benign code would otherwise silently become a machotool
 * failure, which is the same "two places deciding one thing" drift the
 * shared module exists to prevent. */
static int cmd_retag_swift(const char *path, const char *out) {
    /* Before any read -- see bad_out. */
    if (bad_out("retag-swift", path, out)) return EX_FAIL;
    size_t out_size = 0;
    int n = mswift_retag_file(path, out, &out_size);
    if (n == MSWIFT_NOT_MACHO) {
        fprintf(stderr, "machotool retag-swift: %s: not a readable 64-bit Mach-O. "
                        "This verb is thin-only, like retag_swift_classes, so that "
                        "covers a fat container as well as anything that is not a "
                        "Mach-O at all.\n", path);
        return EX_REFUSED;
    }
    if (n == MSWIFT_ERROR) return EX_FAIL;   /* already reported inside mswift_retag_file */
    if (n < 0) {
        /* A code swift_retag.h grew that this verb has not been taught. Refuse
         * rather than fall through to "retagged -4 class record(s)" and exit
         * 0 -- an unrecognized negative is precisely the case the by-name rule
         * above exists for, and guessing which side of refusal it belongs on
         * is not this verb's call to make. */
        fprintf(stderr, "machotool retag-swift: %s: mswift_retag_file returned an "
                        "unrecognized code %d; refusing rather than reporting a "
                        "count this verb cannot vouch for\n", path, n);
        return EX_FAIL;
    }
    printf("%s: retagged %d class record(s)\n", path, n);
    printf("Wrote %s (%zu bytes)\n", out, out_size);
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
 * IN OUT, not in place: this was the FIRST verb to read one file and write
 * another, because that is the grammar docs/PROPOSAL.md settled on and what
 * patch_macho's callers pass. IN is only ever read (the whole image is in
 * memory before a byte is written), but `machotool declassify F F` is no longer
 * allowed on that account: every rewriting verb here refuses an OUT that is its
 * FILE, up front, and this one is not an exception to a rule it started. The
 * wrapper is what still gives patch_macho's callers an IN == OUT conversion,
 * by naming a temp beside OUT and installing it.
 *
 * FIVE DELIBERATE DIVERGENCES FROM patch_macho, all of which a wrapper author
 * has to know about, because reproducing patch_macho's observable behaviour on
 * top of this verb means accounting for each. compat/patch_macho.sh closes the
 * first, the third and the fifth and enumerates the other two:
 *
 *   - EXIT CODES. patch_macho returns a flat 1 for everything that goes
 *     wrong. This verb returns EX_REFUSED where it examined the input and
 *     declined on purpose -- not a readable 64-bit Mach-O, no chained fixups
 *     to convert, or any of the conversion's own refusals -- declassify.h's
 *     LIMITS section lists them all: too many segments or strippable
 *     commands, more fixups than the opcode buffers hold, an unknown pointer
 *     format, no room for the 48-byte LC_DYLD_INFO_ONLY, no __LINKEDIT --
 *     and EX_FAIL only for an operational failure, which here means IN
 *     could not even be opened or read, an allocation md_declassify could
 *     not make, or writing OUT failed. That is what
 *     EX_REFUSED's contract above asks for, and this verb is free to use it:
 *     unlike dylib/rpath/lc/minos it has never forwarded another program's
 *     exit code, so there is nothing to preserve. A wrapper that must look
 *     like patch_macho maps both nonzero codes to 1.
 *   - THE WRITE, AND OUT'S MODE. patch_macho creates OUT with
 *     open(O_CREAT|O_TRUNC, 0755) and writes into it; a write that fails
 *     partway leaves a truncated OUT behind, a fresh OUT gets 0755 masked by
 *     the umask, and an existing OUT keeps whatever mode it had. This verb goes
 *     through wa_write_new (src/atomic_write.h) -- a temp file in OUT's
 *     directory carrying IN's mode, owner and xattrs, renamed over OUT -- so a
 *     failed run leaves no half-written output, OUT always gets a new inode,
 *     and its mode is IN's rather than any of the three the C tool produced.
 *     The BYTES written are identical either way.
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
 *   - OUT MAY NOT BE IN. patch_macho allowed it -- same inode, so its hard
 *     links and xattrs survived an in-place conversion -- and this verb refuses
 *     it (EX_FAIL, bad_out) before reading anything. The wrapper reproduces
 *     the old behaviour the way it reproduces every other tool's in-place edit:
 *     a temp beside OUT as this verb's OUT, then mv.
 *
 * The MDCL_ codes are tested BY NAME below, never as `rc < 0` or `rc != 0` --
 * declassify.h says why: MDCL_PASSTHROUGH is a nonzero SUCCESS, and a code
 * added later must not silently become either a success or the wrong kind of
 * failure. */
static int cmd_declassify(const char *in, const char *out) {
    /* Before any read -- see bad_out. */
    if (bad_out("declassify", in, out)) return EX_FAIL;
    uint8_t *buf = NULL;
    size_t len = 0;
    int rc = md_declassify(in, &buf, &len);

    if (rc == MDCL_NOT_MACHO) {
        fprintf(stderr, "machotool declassify: %s: not a readable 64-bit Mach-O. "
                        "This verb is thin-only, like patch_macho, so that covers "
                        "a fat container as well as anything that is not a Mach-O "
                        "at all.\n", in);
        return EX_REFUSED;
    }
    if (rc == MDCL_REFUSED) return EX_REFUSED;  /* md_declassify already said why */
    /* IN could not even be opened or read, or an allocation md_declassify
     * could not make -- md_declassify already reported which. NOT a
     * refusal either way: EX_REFUSED's contract above rules out using it
     * for any of those, and a caller scripting around "this file just
     * isn't one machotool will touch" would be told the wrong thing. */
    if (rc == MDCL_ERROR) return EX_FAIL;
    if (rc != MDCL_CONVERTED && rc != MDCL_PASSTHROUGH) {
        /* A code declassify.h grew that this verb has not been taught. Refuse
         * rather than write an output file from a buffer md_declassify never
         * promised to fill -- an unrecognized code is precisely the case the
         * by-name rule above exists for, and guessing which side of refusal it
         * belongs on is not this verb's call to make. */
        fprintf(stderr, "machotool declassify: %s: md_declassify returned an "
                        "unrecognized code %d; refusing rather than writing an "
                        "output this verb cannot vouch for\n", in, rc);
        return EX_FAIL;
    }

    if (wa_write_new(in, out, buf, len) != 0) {
        /* wa_write_new already reported which syscall failed (WA_IS_INPUT is
         * unreachable: bad_out answered it above, and it would have said so
         * too). This is an operational failure, not a refusal: nothing about
         * the input was wrong. */
        free(buf);
        return EX_FAIL;
    }
    printf("Wrote %s (%zu bytes)\n", out, len);
    free(buf);
    return 0;
}

/* ---- edit: parse an edit script and run it through me_run --------------
 *
 * The one verb whose positionals aren't at fixed argv indices: --verbose may
 * appear anywhere among the arguments, before or after the positionals or
 * between them, so this scans every token once instead of assuming a position.
 * The three tokens that are not "--verbose" -- in the order seen -- are FILE,
 * OUT and SCRIPT. OUT is the positional right after FILE, as it is for every
 * other rewriting verb; it was a `--output` flag while this verb still wrote
 * FILE, and `--dry-run` went with that flag, because a scratch OUT is the same
 * run with the answer somewhere the caller chose.
 *
 * Only the exact token "-" is exempt from the unrecognized-flag check below;
 * it is not "a positional may start with '-'" in general, and "-" is not
 * always stdin: it is stdin only where it lands as SCRIPT (checked below), a
 * literal filename "-" where it lands as FILE (`edit - o s.edits` opens a file
 * named "-"), and an OUT that bad_out refuses. A SCRIPT or FILE whose real
 * name starts with '-' has no escape here (no "--"); reference it through a
 * path that doesn't, e.g. "./-name" (README's edit section says so too).
 *
 * OUT IS CHECKED BEFORE THE SCRIPT IS READ -- bad_out, the same refusal
 * every other OUT-taking verb gets for a single-dash OUT (a `--`-prefixed one
 * is caught as an unknown flag first), before any input is opened. ms_parse then
 * runs, and can fail, before anything is written -- see edit.h's own header
 * comment, which states that as the property this module exists for: nothing is
 * written unless every statement succeeds and the final verify passes. A parse
 * error is reported here, prefixed the same way every other verb's own
 * diagnostics are, and returns EX_FAIL: an unparseable script is an operational
 * failure (a typo in the script), not a considered refusal about what FILE
 * contains. me_run's own return (0 / MR_REFUSED / MR_FAIL) is forwarded
 * verbatim past that point, the same way dylib/rpath/lc forward
 * mr_apply_file's and minos forwards mv_add_version_min's (cmd_minos, above).
 */
enum { ME_READ_OK = 0, ME_READ_IO = -1, ME_READ_MEM = -2 };

/* Reads all of `f` into a malloc'd buffer, growing as needed. There is no
 * size cap -- an edit script can be as long as its author wrote, same as
 * ms_parse's own contract. */
static int me_read_all(FILE *f, uint8_t **out, size_t *outlen) {
    size_t cap = 65536, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) return ME_READ_MEM;
    for (;;) {
        if (len == cap) {
            size_t ncap = cap * 2;
            uint8_t *nbuf = realloc(buf, ncap);
            if (!nbuf) { free(buf); return ME_READ_MEM; }
            buf = nbuf;
            cap = ncap;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) {
            if (ferror(f)) { free(buf); return ME_READ_IO; }
            break;   /* EOF */
        }
    }
    *out = buf;
    *outlen = len;
    return ME_READ_OK;
}

static int cmd_edit_usage(const char *prog) {
    fprintf(stderr, "usage: %s edit FILE OUT SCRIPT [--verbose]\n", prog);
    return EX_FAIL;
}

static int cmd_edit(int argc, char **argv) {
    const char *prog = argv[0];
    const char *file = NULL, *out = NULL, *script_path = NULL;
    int verbose = 0;
    int npos = 0;

    for (int i = 2; i < argc; i++) {
        const char *tok = argv[i];
        if (strcmp(tok, "--verbose") == 0) {
            verbose = 1;
        } else if (strncmp(tok, "--", 2) == 0) {
            /* Only a DOUBLE dash is a flag here, and --verbose is the only one
             * left: every other verb takes its FILE positionally without
             * examining it -- the historical tools open()ed whatever argv
             * handed them, so a file really named "-dashy" is a file name.
             * Rejecting a single dash here made `machotool edit -dashy o -` fail
             * where `machotool dylib -dashy ...` succeeds, which
             * tests/wrapper_test.sh's leading-dash case caught the moment the
             * compat wrappers started emitting `edit`. "-" alone stays the
             * stdin marker for SCRIPT. `--output` and `--dry-run` land here
             * now, which is the answer a caller passing either deserves: OUT is
             * a positional, and --capabilities no longer advertises them. */
            fprintf(stderr, "machotool edit: unknown flag '%s'\n", tok);
            return EX_FAIL;
        } else if (npos == 0) {
            file = tok;
            npos++;
        } else if (npos == 1) {
            out = tok;
            npos++;
        } else if (npos == 2) {
            script_path = tok;
            npos++;
        } else {
            return cmd_edit_usage(prog);
        }
    }
    if (npos != 3) return cmd_edit_usage(prog);

    /* Before the script is read, and before FILE is opened -- see bad_out. */
    if (bad_out("edit", file, out)) return EX_FAIL;

    FILE *f;
    if (strcmp(script_path, "-") == 0) {
        f = stdin;
    } else {
        f = fopen(script_path, "rb");
        if (!f) {
            fprintf(stderr, "machotool edit: %s: %s\n", script_path, strerror(errno));
            return EX_FAIL;
        }
    }

    uint8_t *buf = NULL;
    size_t len = 0;
    int rrc = me_read_all(f, &buf, &len);
    /* Captured before fclose(), which can itself touch errno and clobber
     * whatever fread()/ferror() just set for a genuine read failure. */
    int read_errno = errno;
    if (f != stdin) fclose(f);
    if (rrc == ME_READ_MEM) {
        fprintf(stderr, "machotool edit: %s: out of memory\n", script_path);
        return EX_FAIL;
    }
    if (rrc == ME_READ_IO) {
        fprintf(stderr, "machotool edit: %s: %s\n", script_path, strerror(read_errno));
        return EX_FAIL;
    }

    ms_script s;
    char perr[256];
    if (ms_parse((const char *)buf, len, &s, perr, sizeof perr) != 0) {
        fprintf(stderr, "machotool edit: %s: %s\n", script_path, perr);
        free(buf);
        return EX_FAIL;
    }
    free(buf);

    me_opts o;
    memset(&o, 0, sizeof o);
    o.verbose = verbose;
    o.log = stderr;

    int rc = me_run(file, out, &s, &o);
    ms_free(&s);
    return rc;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--capabilities") == 0)
        return print_capabilities();

    if (argc < 2) { usage(argv[0]); return EX_FAIL; }
    const char *verb = argv[1];

    if (strcmp(verb, "verify") == 0) {
        if (argc != 3) { fprintf(stderr, "usage: %s verify FILE\n", argv[0]); return EX_FAIL; }
        return cmd_verify(argv[2]);
    }
    if (strcmp(verb, "info") == 0) {
        if (argc != 3) { fprintf(stderr, "usage: %s info FILE\n", argv[0]); return EX_FAIL; }
        return cmd_info(argv[2]);
    }
    if (strcmp(verb, "grow") == 0) {
        if (argc != 5) { fprintf(stderr, "usage: %s grow FILE OUT N\n", argv[0]); return EX_FAIL; }
        return cmd_grow(argv[2], argv[3], argv[4]);
    }
    if (strcmp(verb, "minos") == 0) {
        int allow_grow = (argc == 6 && strcmp(argv[5], "--allow-grow") == 0);
        if (argc != 5 && !allow_grow) {
            fprintf(stderr, "usage: %s minos FILE OUT 10.9 [--allow-grow]\n", argv[0]);
            return EX_FAIL;
        }
        return cmd_minos(argv[2], argv[3], argv[4], allow_grow);
    }
    if (strcmp(verb, "segment") == 0) {
        if (argc != 6) { fprintf(stderr, "usage: %s segment FILE OUT OLD NEW\n", argv[0]); return EX_FAIL; }
        return cmd_segment(argv[2], argv[3], argv[4], argv[5]);
    }
    if (strcmp(verb, "retag-swift") == 0) {
        if (argc != 4) { fprintf(stderr, "usage: %s retag-swift FILE OUT\n", argv[0]); return EX_FAIL; }
        return cmd_retag_swift(argv[2], argv[3]);
    }
    if (strcmp(verb, "lc") == 0) {
        if (argc < 6) { fprintf(stderr, "usage: %s lc FILE OUT [--fatal-warnings] -delete KIND [-delete KIND...]\n", argv[0]); return EX_FAIL; }
        return cmd_lc(argc, argv);
    }
    if (strcmp(verb, "dylib") == 0) {
        if (argc < 5) { fprintf(stderr, "usage: %s dylib FILE OUT [--allow-grow] [--fatal-warnings] OP...\n", argv[0]); return EX_FAIL; }
        return cmd_dylib_or_rpath(argc, argv, 0);
    }
    if (strcmp(verb, "rpath") == 0) {
        if (argc < 5) { fprintf(stderr, "usage: %s rpath FILE OUT [--allow-grow] [--fatal-warnings] OP...\n", argv[0]); return EX_FAIL; }
        return cmd_dylib_or_rpath(argc, argv, 1);
    }
    if (strcmp(verb, "declassify") == 0) {
        if (argc != 4) { fprintf(stderr, "usage: %s declassify IN OUT\n", argv[0]); return EX_FAIL; }
        return cmd_declassify(argv[2], argv[3]);
    }
    if (strcmp(verb, "edit") == 0) {
        /* Flags may appear anywhere among the arguments, so there is no
         * fixed argc this dispatch can check up front -- cmd_edit's own scan
         * validates the positional count (and everything else) itself. */
        return cmd_edit(argc, argv);
    }

    fprintf(stderr, "machotool: unknown verb '%s'\n", verb);
    usage(argv[0]);
    return EX_FAIL;
}
