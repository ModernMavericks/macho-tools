#!/bin/sh
# compat/macho9-compat.sh -- the machinery the historical tools' /bin/sh
# wrappers share. Sourced, never run:
#
#   MW_DIR=... ; . "$MW_DIR/macho9-compat.sh"
#
# Each wrapper is then four or five lines of its own: translate this argv,
# teach the equivalent on stderr, run it, and map the exit code and stdout
# back to what the C tool it replaced would have produced.
#
# WHY THE WRAPPERS ARE NOT SIX COPIES OF THIS. Task 1 put the whole
# old-grammar-to-macho9 translation in ONE file (compat/translate.sh) so that
# "the translation that was tested is literally the translation that ships".
# The same argument applies to everything AROUND the translation -- finding
# macho9, printing the teaching message, running a sequence and stopping at
# the first failure, and the temp-file dance that keeps a multi-command
# sequence from leaving a half-converted binary behind. Those are one
# implementation here, not six.
#
# ---- what lives WHERE, and why -------------------------------------------
#
#   compat/translate.sh      argv -> macho9 command line(s). Pure text; runs
#                            nothing. Pinned by tests/translate_test.sh.
#   compat/macho9-compat.sh  this file: run those command lines safely.
#   compat/<tool>.sh         one per tool: only what that tool's observable
#                            behaviour needs that macho9's does not already
#                            give -- its exit-code mapping and its stdout.
#
# Every behavioural difference each wrapper has to close is documented at its
# site: the divergence list at the top of compat/translate.sh, and the
# "DELIBERATE DIVERGENCES FROM <tool>" blocks in cli/macho9.c's cmd_segment,
# cmd_retag_swift and cmd_declassify.
#
# ---- how a wrapper finds macho9 and its two support files ----------------
#
# All four files -- macho9, macho9-translate.sh, macho9-compat.sh and the
# wrapper itself -- are installed FLAT, in the same bin directory, and a
# wrapper looks for the other three next to itself ($0's directory, resolved
# through PATH when $0 has no slash). $MACHO9_COMPAT_DIR overrides that, which
# is how a build tree or a test harness points at an uninstalled set.
#
# Flat, rather than a libexec/ subdirectory, because of the shape of the one
# production caller: mavericksforever.com/claude/install.sh downloads the
# tools BY NAME into a single directory. A layout that needed a subdirectory
# would need that script changed; a flat one needs only the extra file names.
# (It needs those either way -- a wrapper cannot work without macho9 present,
# which is a packaging consequence of this whole plan, not of this layout.)
#
# ---- POSIX sh only -------------------------------------------------------
#
# 10.9's /bin/sh is bash 3.2 in sh mode, which accepts plenty a stricter shell
# does not, so nothing here may rely on that. tests/wrapper_test.sh re-runs
# the wrappers under /bin/ksh for the same reason tests/translate_test.sh
# re-runs the translator under it.
#
# set -u, and deliberately NOT set -e: an earlier task in this plan found
# `set -e` silently swallowing a real failure at two sites in this repo's
# shell, so every failure here is checked where it happens. Same choice
# tests/compat-sweep.sh and tests/translate_test.sh made.

set -u

# ---- locate everything ---------------------------------------------------
#
# MW_DIR is set by the wrapper before sourcing this file (it is the only thing
# that cannot be worked out from in here, since $0 is the wrapper either way,
# but the wrapper's own bootstrap is two lines and this keeps them there).
#
# Made ABSOLUTE first. Invoked as `./change_dylib`, $0's directory is ".", and
# this directory goes on PATH below -- so leaving it relative would prepend a
# literal "." to PATH and make every helper this file runs (cp, sed, awk)
# resolve against the working directory at the moment it runs. Resolving it
# once, here, keeps PATH naming one fixed directory: the one the wrapper
# itself came out of.
MW_DIR=$(cd "$MW_DIR" 2>/dev/null && pwd) || {
    printf '%s: cannot resolve my own directory\n' "$0" >&2
    exit 1
}
[ -r "$MW_DIR/macho9-translate.sh" ] || {
    printf '%s: cannot find macho9-translate.sh in %s -- macho9 and its two\n' "$0" "$MW_DIR" >&2
    printf '%s: support files must be installed together (or set MACHO9_COMPAT_DIR)\n' "$0" >&2
    exit 1
}

# PATH, not an absolute program word: the command lines translate.sh emits are
# the ones a human is being taught to type, so they must say `macho9` and mean
# the macho9 that ships alongside this wrapper. Prepending the wrapper's own
# directory is what makes those two the same thing. tests/compat-sweep.sh runs
# the emitted lines the same way, for the same reason.
if [ -x "$MW_DIR/macho9" ]; then
    PATH="$MW_DIR:$PATH"
    export PATH
elif ! command -v macho9 >/dev/null 2>&1; then
    printf '%s: macho9 is not next to me in %s and not on PATH; this tool is a\n' "$0" "$MW_DIR" >&2
    printf '%s: wrapper around it and cannot do anything without it\n' "$0" >&2
    exit 1
fi

MT_SOURCED=1
export MT_SOURCED
. "$MW_DIR/macho9-translate.sh"

# A scratch directory for the wrappers that have to capture macho9's output in
# order to reshape it. Created once, removed on every exit path.
MW_T=$(mktemp -d "${TMPDIR:-/tmp}/macho9-compat.XXXXXX") || {
    printf '%s: cannot create a temporary directory\n' "$0" >&2
    exit 1
}
MW_TMPFILE=''
mw_cleanup() {
    rm -rf "$MW_T"
    [ -n "$MW_TMPFILE" ] && rm -f -- "$MW_TMPFILE"
    return 0
}
trap 'mw_cleanup' EXIT
trap 'mw_cleanup; exit 130' INT
trap 'mw_cleanup; exit 143' TERM

MW_TOOL=''
MW_CMDS=''
MW_NCMDS=0

# ---- translate, and teach ------------------------------------------------
#
# mw_translate TOOL ARG...
#
# Fills MW_CMDS (the emitted command lines, newline-separated) and MW_NCMDS,
# and prints the teaching message. Returns compat/translate.sh's own exit
# code, which the caller forwards:
#
#   0  translated (MW_NCMDS may be 0 -- the old invocation was a no-op)
#   1  the OLD TOOL would have refused this argv; translate.sh has already
#      printed the old tool's own message, so there is nothing to add
#   2  no macho9 command line means what this argv meant; same
#
# MT_PROG0 is $0 rather than the tool's name, because every usage line these
# tools print names argv[0] -- so `/some/where/change_dylib` with bad
# arguments prints exactly the path it was invoked as, exactly as the C
# binary did.
mw_translate() {
    MW_TOOL=$1
    MT_PROG0=$0
    MW_CMDS=$(mt_translate "$@")
    mw_trc=$?
    unset MT_PROG0
    [ "$mw_trc" -eq 0 ] || return "$mw_trc"
    MW_NCMDS=0
    [ -n "$MW_CMDS" ] && MW_NCMDS=$(printf '%s\n' "$MW_CMDS" | wc -l | tr -d ' ')
    mw_teach
    return 0
}

# The plan's phase one, in one function: "the caller's script keeps working,
# and the message teaches the new grammar". On STDERR, so stdout stays exactly
# what the C tool printed for anything reading it.
mw_teach() {
    if [ "$MW_NCMDS" -eq 0 ]; then
        printf '%s: deprecated -- macho9 does this now. This invocation asks for nothing macho9 would have to do.\n' \
            "$MW_TOOL" >&2
        return 0
    fi
    if [ "$MW_NCMDS" -eq 1 ]; then
        printf '%s: deprecated -- macho9 does this now. The equivalent command is:\n' "$MW_TOOL" >&2
    else
        printf '%s: deprecated -- macho9 does this now. The equivalent commands, in this order, are:\n' \
            "$MW_TOOL" >&2
    fi
    printf '%s\n' "$MW_CMDS" | sed 's/^/    /' >&2
    return 0
}

# ---- run -----------------------------------------------------------------
#
# mw_run -- run MW_CMDS in order, stopping at the first nonzero exit and
# returning that exit code. This is compat/translate.sh's stated output
# contract ("Run them in the order printed and stop at the first nonzero
# exit"), in the one place the wrappers need it executed.
#
# The commands are fed from a here-document rather than a pipe so the loop
# runs in THIS shell (a pipe's subshell would lose mw_rc), and each command's
# own stdin is /dev/null so it cannot eat the rest of the list.
mw_run() {
    mw_rc=0
    while IFS= read -r mw_line; do
        [ -n "$mw_line" ] || continue
        eval "$mw_line" </dev/null || { mw_rc=$?; break; }
    done <<MW_RUN_EOF
$MW_CMDS
MW_RUN_EOF
    return "$mw_rc"
}

# mw_run_atomic TOOL FILE ARG...   (TOOL FILE ARG... is the OLD argv)
#
# THE MIXED-FAMILY SPLIT, AND WHAT IT COSTS. change_dylib applies every
# operation in ONE pass over the load commands and writes ONCE, atomically.
# macho9 has a verb per family, so an invocation touching more than one family
# becomes a SEQUENCE -- and tests/compat-sweep.sh measured what that costs:
# two rows came back where the C tool refused ATOMICALLY while the translated
# sequence refused AFTER ALREADY WRITING (the matrix marks them "+partial").
# install.sh's production line is exactly that shape: it strips uuid and
# codesig to reclaim header bytes and then rewrites three dylib paths.
#
# So a sequence never touches the caller's file. It runs against a COPY, and
# the copy is installed over the original only if every command succeeded. A
# failure anywhere leaves the original exactly as it was -- which is the C
# tool's behaviour, and the whole point.
#
# The copy is made by re-translating the SAME argv with the temp path in place
# of FILE, never by string-editing the emitted lines: the file name reaches
# those lines through mt_qargs' quoting, and unpicking that would be a second,
# worse parser.
#
# WHAT THIS DOES NOT PRESERVE THAT wa_write_atomic DOES (src/atomic_write.h):
#
#   * ATOMICITY OF THE FINAL REPLACEMENT. The result is installed with
#     `cat COPY > FILE`, which truncates and rewrites in place. wa_write_atomic
#     renames a fully-written temp file over the target, so the target is
#     always either wholly old or wholly new; here a crash, a full disk or a
#     kill DURING that last copy leaves FILE truncated. This is the one thing
#     given up, and it is given up knowingly: writing in place is what
#     preserves everything in the next paragraph.
#   * Nothing else. Writing THROUGH the existing path keeps the inode, so the
#     mode, the owner, the xattrs and every hard link to the file survive
#     unchanged, and a FILE that is a symlink is followed to its target rather
#     than replaced -- the three things wa_write_atomic goes out of its way to
#     arrange (realpath first, xattrs copied, ftruncate+write when st_nlink>1)
#     come free from not renaming at all.
#
# A SINGLE command needs none of this: macho9 already writes it atomically,
# through wa_write_atomic itself. So mw_run_atomic runs it directly, which
# also keeps the common case's stdout naming the caller's own path.
#
# AND ONE MORE THING IT COSTS, ON STDOUT: every line mr_apply_file prints is
# labelled with the path it was handed, so a sequence run against the copy
# prints the COPY's name -- `.f.macho9-compat.4711: header pad ...` rather
# than `f: header pad ...`. That only ever happens on invocations whose stdout
# already cannot match the C tool's (a sequence prints one header-pad/updated
# pair PER PASS where one invocation printed one pair), which is why it is
# accepted rather than papered over by rewriting macho9's own output: a line
# that named `f` would be claiming macho9 had been run on a file it was not.
mw_run_atomic() {
    mw_tool=$1
    mw_file=$2
    shift 2

    if [ "$MW_NCMDS" -le 1 ]; then
        mw_run
        return $?
    fi

    # Beside the original, not in $TMPDIR: the copy is about to be rewritten
    # by macho9, which writes atomically via a temp file of its OWN in the
    # same directory, so the directory has to be writable either way. Failing
    # here, before anything has run, is the earliest that can be found out.
    # Split with parameter expansion rather than dirname/basename: those two
    # would take a FILE beginning with `-` for an option, and the C tools
    # simply open()ed whatever they were handed. `cp` and `rm` get `--` for
    # the same reason.
    case $mw_file in
        */*) mw_dirpart=${mw_file%/*}; mw_basepart=${mw_file##*/} ;;
        *)   mw_dirpart=.;             mw_basepart=$mw_file ;;
    esac
    [ -n "$mw_dirpart" ] || mw_dirpart=/
    MW_TMPFILE="$mw_dirpart/.$mw_basepart.macho9-compat.$$"
    rm -f -- "$MW_TMPFILE"
    if ! cp -p -- "$mw_file" "$MW_TMPFILE"; then
        printf '%s: cannot copy %s aside; refusing to run a multi-step rewrite in place\n' \
            "$mw_tool" "$mw_file" >&2
        rm -f -- "$MW_TMPFILE"; MW_TMPFILE=''
        return 1
    fi

    MT_PROG0=$0
    MW_CMDS=$(mt_translate "$mw_tool" "$MW_TMPFILE" "$@")
    mw_rc=$?
    unset MT_PROG0
    if [ "$mw_rc" -ne 0 ]; then
        # The first translation of this same argv succeeded, and the only
        # thing that changed is the file name -- which no refusal in
        # translate.sh looks at. Reaching here means the two disagreed, so
        # stop rather than run something that was never shown to be equivalent.
        printf '%s: internal error: the translation is not stable under a change of file name\n' \
            "$mw_tool" >&2
        rm -f -- "$MW_TMPFILE"; MW_TMPFILE=''
        return 1
    fi

    mw_run
    mw_rc=$?
    if [ "$mw_rc" -eq 0 ]; then
        if ! cat -- "$MW_TMPFILE" > "$mw_file"; then
            printf '%s: %s: the rewrite succeeded but installing it failed\n' "$mw_tool" "$mw_file" >&2
            mw_rc=1
        fi
    fi
    rm -f -- "$MW_TMPFILE"; MW_TMPFILE=''
    return "$mw_rc"
}
