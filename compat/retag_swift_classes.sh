#!/bin/sh
# retag_swift_classes -- a /bin/sh wrapper around `macho9 retag-swift FILE OUT`.
#
#   retag_swift_classes binary [binary ...]
#
# WHAT THIS REPLACED. compat/retag_swift_classes.c was a multi-file argv loop,
# two messages and a `had_error ? 1 : 0` exit over mswift_retag_file
# (src/swift_retag.h) -- the same function `macho9 retag-swift` calls. Why the
# retag is needed (a Swift runtime built for a pre-10.14.4 target tests the
# LEGACY is-swift bit, while a modern linker sets the stable-ABI one) is
# written down in src/swift_retag.h, which outlives this front-end.
#
# GRAMMAR -- THE ONE VARIADIC TOOL. `macho9 retag-swift` takes exactly ONE
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
# EXIT CODES -- MAPPED, per cli/macho9.c's cmd_retag_swift ("ONE DELIBERATE
# DIVERGENCE FROM retag_swift_classes"). macho9's own scheme is 0 ok, 1
# refused, 2 error (cli/macho9.c's top-of-file comment):
#
#   macho9 1 (EX_REFUSED)  -> SKIPPED, silently, and the loop keeps going.
#       This is MSWIFT_NOT_MACHO and nothing else. retag_swift_classes treated
#       a non-Mach-O argument as a benign skip -- it printed nothing and did
#       not set had_error -- so macho9's diagnostic for it is discarded too,
#       which is why each file's stderr is captured rather than passed
#       straight through. tests/compat-matrix.tsv has three "blocked" rows
#       that are exactly this case (`nm`, `f nm`, `f nm f`); this is what
#       unblocks them.
#   macho9 2 (EX_FAIL)     -> had_error, and the loop keeps going. That covers
#       MSWIFT_ERROR, which is what retag_swift_classes counted as an error
#       too.
#   macho9 0               -> count it, then install: mw_finish installs the
#       temp over the argument, or discards it when the bytes did not change,
#       same as add_version_min.sh.
#
# This wrapper's OWN pre-checks (mw_prepare, run once per argument) are a
# fourth source of per-file failure the old tool never had: an argument that
# cannot be prepared for install -- absent, unwritable, or a regular file
# carrying other hard links -- is reported on stderr in this wrapper's own
# words (macho9-compat.sh's mw_prepare) and counted as had_error, and the loop
# moves on to the next argument. The old tool wrote through the open file
# descriptor directly, so a hard-linked argument was retagged like any other;
# this wrapper installs via mv instead (macho9-compat.sh's "the install path"
# has the reasoning), so a hard-linked argument is refused rather than
# retagged, the same trade add_version_min.sh's own header names for its one
# file.
#
# The final exit is `had_error ? 1 : 0`, as it always was.
#
# STDOUT -- REBUILT, because two things differ:
#
#   * `macho9 retag-swift` prints its per-file "%s: retagged %d class
#     record(s)" line ALWAYS, followed by its own "Wrote ..." line naming the
#     temp; retag_swift_classes printed the count line only when it was
#     nonzero, and never named a temp at all.
#   * retag_swift_classes ends with "total: %d class record(s) retagged",
#     which a single-file verb has nothing to say about.
#
# So macho9's stdout is captured, the count is read back out of it, and this
# wrapper prints the C tool's two messages itself -- macho9's own stdout for
# each file is never forwarded, so its closing "Wrote ..." line never needs
# separate suppression the way mw_run_to_tmp suppresses it for a single-shot
# wrapper. The count is extracted with an anchored substitution over macho9's
# own stable output -- the same oracle tests/cli_test.sh asserts against, and
# explicitly not otool/nm text (tests/README.md's second lesson).
# tests/leaf-tool-crashes.sh greps stdout for "^total: 0 class record(s)
# retagged$" on a deliberately malformed fixture, so the total line is
# load-bearing, not decoration.

MW_SELF=$(command -v "$0" 2>/dev/null) || MW_SELF=$0
MW_DIR=${MACHO9_COMPAT_DIR:-$(dirname "$MW_SELF")}
# Checked here, before sourcing, so a missing support file gets this message
# rather than the shell's own "No such file or directory" from the `.` below.
# The case that actually reaches it: a SYMLINK to this wrapper placed on PATH.
# $0 resolves to the symlink, so MW_DIR is the symlink's directory, not the
# one holding macho9 -- which is why MACHO9_COMPAT_DIR exists.
[ -r "$MW_DIR/macho9-compat.sh" ] || {
    printf '%s: cannot find macho9-compat.sh in %s -- macho9 and its two support\n' "$0" "$MW_DIR" >&2
    printf '%s: files must sit beside this wrapper; a symlink to it resolves to the\n' "$0" >&2
    printf '%s: SYMLINK directory, so set MACHO9_COMPAT_DIR to where they really are\n' "$0" >&2
    exit 1
}
. "$MW_DIR/macho9-compat.sh"

mw_translate retag_swift_classes "$@" || exit $?

mw_total=0
mw_had_error=0
for mw_f in "$@"; do
    mw_prepare "$mw_f" || { mw_had_error=1; continue; }
    # Retranslate THIS ONE argument, naming the temp mw_prepare just chose --
    # not mw_retranslate, which re-emits the WHOLE original argv; this tool's
    # translation is already one line per argument, and only one of those
    # lines is being re-run right now.
    MT_PROG0=$0
    mw_line=$(MT_OUT=$MW_TMPFILE mt_translate retag_swift_classes "$mw_f")
    mw_trc=$?
    unset MT_PROG0
    if [ "$mw_trc" -ne 0 ]; then
        printf '%s: internal error: the translation is not stable under a change of output\n' "$MW_TOOL" >&2
        mw_had_error=1
        rm -f -- "$MW_TMPFILE"; MW_TMPFILE=''
        continue
    fi
    eval "$mw_line" </dev/null >"$MW_T/out" 2>"$MW_T/err"
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
