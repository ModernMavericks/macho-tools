#!/bin/sh
# retag_swift_classes -- a /bin/sh wrapper around `machotool retag-swift FILE OUT`.
#
#   retag_swift_classes binary [binary ...]
#
# WHAT THIS REPLACED. compat/retag_swift_classes.c was a multi-file argv loop,
# two messages and a `had_error ? 1 : 0` exit over mswift_retag_file
# (src/swift_retag.h) -- the same function `machotool retag-swift` calls. Why the
# retag is needed (a Swift runtime built for a pre-10.14.4 target tests the
# LEGACY is-swift bit, while a modern linker sets the stable-ABI one) is
# written down in src/swift_retag.h, which outlives this front-end.
#
# GRAMMAR -- THE ONE VARIADIC TOOL. `machotool retag-swift` takes exactly ONE
# file (and now names its own output, since it no longer writes the file it
# is given); this tool takes any number. So the translation is a LOOP, one
# emitted `retag-swift`+install pair per argument in argv order, and this
# wrapper runs them one at a time rather than through mw_run: mw_run evaluates
# a translation as ONE script and hands back one exit code, and this tool
# needs each file's own code and each file's own stdout to rebuild its
# per-file and total lines. It is also the only translation that is still
# more than one command, which is why mw_run does not loop. For each file this
# wrapper follows the same install path add_version_min.sh does --
# mw_prepare, retranslate naming the temp, run, mw_finish -- just once per
# argument instead of once for the whole invocation.
#
# EXIT CODES -- MAPPED, per cli/machotool.c's cmd_retag_swift ("ONE DELIBERATE
# DIVERGENCE FROM retag_swift_classes"). machotool's own scheme is 0 ok, 1
# refused, 2 error (cli/machotool.c's top-of-file comment):
#
#   machotool 1 (EX_REFUSED)  -> SKIPPED, silently, and the loop keeps going.
#       This is MSWIFT_NOT_MACHO and nothing else. retag_swift_classes treated
#       a non-Mach-O argument as a benign skip -- it printed nothing and did
#       not set had_error -- so machotool's diagnostic for it is discarded too,
#       which is why each file's stderr is captured rather than passed
#       straight through. tests/compat-matrix.tsv has three "blocked" rows
#       that are exactly this case (`nm`, `f nm`, `f nm f`); this is what
#       unblocks them.
#   machotool 2 (EX_FAIL)     -> had_error, and the loop keeps going. That covers
#       MSWIFT_ERROR, which is what retag_swift_classes counted as an error
#       too -- an unreadable argument (including a DIRECTORY: mw_prepare's
#       hard-link check only looks at regular files, so a directory falls
#       through it and reaches machotool itself, which refuses with its own
#       words, `d: cannot open or read`, from mi_open's read failing on one)
#       -- and, new with this wrapper's install step, a write that machotool itself
#       cannot make: a WRITABLE argument inside a NON-writable directory.
#       mw_prepare's own checks pass (the argument itself is fine), but the
#       temp machotool writes beside it needs the DIRECTORY writable, which the
#       old tool never needed -- it wrote through the already-open descriptor,
#       never creating a second name. Measured (`chmod 555 ro`, arguments
#       `a ro/b`): `a` is retagged and printed; `ro/b` is not -- machotool fails
#       the write with `mkstemp: Permission denied` on stderr, exits EX_FAIL,
#       and this wrapper's had_error path takes it from there, the same as any
#       other machotool failure. compat/add_version_min.sh's own header names the
#       identical shape for its one file (there it surfaces as that wrapper's
#       raw, forwarded exit 2; here it is folded into had_error's flat 1, since
#       this wrapper never forwards one argument's exit code as the whole
#       run's), and compat/change_dylib.sh's records it too.
#   machotool 0               -> count it, then install: mw_finish installs the
#       temp over the argument, or discards it when the bytes did not change,
#       same as add_version_min.sh.
#
# This wrapper's OWN pre-checks (mw_prepare, run once per argument, BEFORE
# machotool ever runs) are a fourth source of per-file failure the old tool never
# had, in this wrapper's own words rather than machotool's: an absent argument
# (`open: No such file or directory`), an unwritable one (`open: Permission
# denied`), or a regular file carrying other hard links (`... has N hard
# links; ...`) -- each reported on stderr, counted as had_error, and the loop
# moves on to the next argument. The old tool wrote through the open file
# descriptor directly, so a hard-linked argument was retagged like any other;
# this wrapper installs via mv instead (machotool-compat.sh's "the install path"
# has the reasoning), so a hard-linked argument is refused rather than
# retagged, the same trade add_version_min.sh's own header names for its one
# file.
#
# UNLIKE add_version_min.sh, these first two are a WORDING divergence too, not
# just an earlier-than-machotool one. add_version_min.c's own open() failure
# printed literally "open: ..." (its perror's argument was the string "open",
# not the path), so mw_require_writable's identical words happen to match the
# old tool's own by construction. retag_swift_classes.c's open() failure used
# perror(path) instead -- "<path>: No such file or directory" -- which is
# still what mswift_retag_file itself prints when machotool actually reaches the
# open() (tests/compat-matrix.tsv's rows for an absent argument recorded both
# sides matching on that wording, before the wrapper's own pre-check began
# answering first). mw_require_writable now
# intercepts first and says "open: ..." instead, so an absent or unwritable
# argument no longer matches the old tool's wording, only its exit code.
#
# The final exit is `had_error ? 1 : 0`, as it always was.
#
# STDOUT -- REBUILT, because two things differ:
#
#   * `machotool retag-swift` prints its per-file "%s: retagged %d class
#     record(s)" line ALWAYS, followed by its own "Wrote ..." line naming the
#     temp; retag_swift_classes printed the count line only when it was
#     nonzero, and never named a temp at all.
#   * retag_swift_classes ends with "total: %d class record(s) retagged",
#     which a single-file verb has nothing to say about.
#
# So machotool's stdout is captured, the count is read back out of it, and this
# wrapper prints the C tool's two messages itself -- machotool's own stdout for
# each file is never forwarded, so its closing "Wrote ..." line never needs
# separate suppression the way mw_run_to_tmp suppresses it for a single-shot
# wrapper. The count is extracted with an anchored substitution over machotool's
# own stable output -- the same oracle tests/cli_test.sh asserts against, and
# explicitly not otool/nm text (tests/README.md's second lesson).
# tests/leaf-tool-crashes.sh greps stdout for "^total: 0 class record(s)
# retagged$" on a deliberately malformed fixture, so the total line is
# load-bearing, not decoration.

MW_SELF=$(command -v "$0" 2>/dev/null) || MW_SELF=$0
MW_DIR=${MACHOTOOL_COMPAT_DIR:-$(dirname "$MW_SELF")}
# Checked here, before sourcing, so a missing support file gets this message
# rather than the shell's own "No such file or directory" from the `.` below.
# The case that actually reaches it: a SYMLINK to this wrapper placed on PATH.
# $0 resolves to the symlink, so MW_DIR is the symlink's directory, not the
# one holding machotool -- which is why MACHOTOOL_COMPAT_DIR exists.
[ -r "$MW_DIR/machotool-compat.sh" ] || {
    printf '%s: cannot find machotool-compat.sh in %s -- machotool and its two support\n' "$0" "$MW_DIR" >&2
    printf '%s: files must sit beside this wrapper; a symlink to it resolves to the\n' "$0" >&2
    printf '%s: SYMLINK directory, so set MACHOTOOL_COMPAT_DIR to where they really are\n' "$0" >&2
    exit 1
}
. "$MW_DIR/machotool-compat.sh"

mw_translate retag_swift_classes "$@" || exit $?

mw_total=0
mw_had_error=0
for mw_f in "$@"; do
    mw_prepare "$mw_f" || { mw_had_error=1; continue; }
    # mw_retranslate's signature is TOOL ARG..., so retranslating THIS ONE
    # argument -- naming the temp mw_prepare just chose -- is the same call
    # mw_translate itself made above, with $mw_f standing in for the whole
    # original argv.
    mw_retranslate retag_swift_classes "$mw_f" || {
        mw_had_error=1
        rm -f -- "$MW_TMPFILE"; MW_TMPFILE=''
        continue
    }
    eval "$MW_CMDS" </dev/null >"$MW_T/out" 2>"$MW_T/err"
    mw_rc=$?
    case $mw_rc in
    0)
        cat "$MW_T/err" >&2
        mw_n=$(sed -n 's/^.*: retagged \([0-9][0-9]*\) class record(s)$/\1/p' "$MW_T/out")
        [ -n "$mw_n" ] || mw_n=0
        if mw_finish; then
            if [ "$mw_n" -gt 0 ]; then
                printf '%s: retagged %d class record(s)\n' "$mw_f" "$mw_n"
                mw_total=$((mw_total + mw_n))
            fi
        else
            mw_had_error=1
        fi
        ;;
    1)
        # MSWIFT_NOT_MACHO: the benign skip. No message, no error flag, and
        # nothing was written -- discard the unused temp mw_prepare made.
        rm -f -- "$MW_TMPFILE"; MW_TMPFILE=''
        ;;
    *)
        cat "$MW_T/err" >&2
        mw_had_error=1
        rm -f -- "$MW_TMPFILE"; MW_TMPFILE=''
        ;;
    esac
done

printf 'total: %d class record(s) retagged\n' "$mw_total"
[ "$mw_had_error" -eq 0 ] || exit 1
exit 0
