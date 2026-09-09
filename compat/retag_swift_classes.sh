#!/bin/sh
# retag_swift_classes -- a /bin/sh wrapper around `macho9 retag-swift FILE`.
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
# file; this tool takes any number. So the translation is a LOOP, one emitted
# line per argument in argv order, and this wrapper runs them one at a time
# rather than through mw_run: it needs each file's own exit code and its own
# stdout, which mw_run's stop-at-the-first-failure contract deliberately
# does not give. The lines it runs are still the translated ones, not
# hand-built ones, so what ships is what tests/translate_test.sh pinned.
#
# EXIT CODES -- MAPPED, per cli/macho9.c's cmd_retag_swift ("TWO DELIBERATE
# DIVERGENCES FROM retag_swift_classes"):
#
#   macho9 2 (EX_REFUSED)  -> SKIPPED, silently, and the loop keeps going.
#       This is MSWIFT_NOT_MACHO and nothing else. retag_swift_classes treated
#       a non-Mach-O argument as a benign skip -- it printed nothing and did
#       not set had_error -- so macho9's diagnostic for it is discarded too,
#       which is why each file's stderr is captured rather than passed
#       straight through. tests/compat-matrix.tsv has three "blocked" rows
#       that are exactly this case (`nm`, `f nm`, `f nm f`); this is what
#       unblocks them.
#   macho9 1               -> had_error, and the loop keeps going. That covers
#       MSWIFT_ERROR (which is what retag_swift_classes counted as an error
#       too) and MSWIFT_RACED (which it did not). The two are indistinguishable
#       from outside macho9, and treating a race -- "the file changed under us,
#       so NOTHING was written" -- as success is the silent-success shape this
#       codebase refuses. So the raced case now exits 1 where the C tool
#       exited 0. Named here because it is a real, if unreachable-on-purpose,
#       behavioural difference; a race is not something a test can stage.
#   macho9 0               -> count it.
#
# The final exit is `had_error ? 1 : 0`, as it always was.
#
# STDOUT -- REBUILT, because two things differ:
#
#   * `macho9 retag-swift` prints its per-file "%s: retagged %d class
#     record(s)" line ALWAYS; retag_swift_classes printed it only when the
#     count was nonzero.
#   * retag_swift_classes ends with "total: %d class record(s) retagged",
#     which a single-file verb has nothing to say about.
#
# So macho9's stdout is captured, the count is read back out of it, and this
# wrapper prints the C tool's two messages itself. The count is extracted with
# an anchored substitution over macho9's own stable output -- the same oracle
# tests/cli_test.sh asserts against, and explicitly not otool/nm text
# (tests/README.md's second lesson). tests/leaf-tool-crashes.sh greps stdout
# for "^total: 0 class record(s) retagged$" on a deliberately malformed
# fixture, so the total line is load-bearing, not decoration.

MW_SELF=$(command -v "$0" 2>/dev/null) || MW_SELF=$0
MW_DIR=${MACHO9_COMPAT_DIR:-$(dirname "$MW_SELF")}
. "$MW_DIR/macho9-compat.sh"

mw_translate retag_swift_classes "$@" || exit $?

mw_total=0
mw_had_error=0
mw_i=0
for mw_f in "$@"; do
    mw_i=$((mw_i + 1))
    mw_line=$(printf '%s\n' "$MW_CMDS" | sed -n "${mw_i}p")
    eval "$mw_line" </dev/null >"$MW_T/out" 2>"$MW_T/err"
    mw_rc=$?
    case $mw_rc in
    0)
        cat "$MW_T/err" >&2
        mw_n=$(sed -n 's/^.*: retagged \([0-9][0-9]*\) class record(s)$/\1/p' "$MW_T/out")
        [ -n "$mw_n" ] || mw_n=0
        if [ "$mw_n" -gt 0 ]; then
            printf '%s: retagged %d class record(s)\n' "$mw_f" "$mw_n"
            mw_total=$((mw_total + mw_n))
        fi
        ;;
    2)
        # MSWIFT_NOT_MACHO: the benign skip. No message, no error flag.
        ;;
    *)
        cat "$MW_T/err" >&2
        mw_had_error=1
        ;;
    esac
done

printf 'total: %d class record(s) retagged\n' "$mw_total"
[ "$mw_had_error" -eq 0 ] || exit 1
exit 0
