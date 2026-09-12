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
# macho9, printing the teaching message, running what was translated, and the
# pre-checks a wrapper has to make for itself because the command it is about
# to run would report them differently. Those are one implementation here, not
# six.
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
elif command -v macho9 >/dev/null 2>&1; then
    # NOT the same hard failure as a missing macho9-translate.sh above,
    # because a caller may legitimately have installed macho9 elsewhere on
    # PATH -- but it is NOT the macho9 the header comment above promises
    # ("the one that ships alongside this wrapper"), so a version mismatch
    # here would be silent without this line. Warn and proceed rather than
    # refuse: refusing would break that legitimate case outright.
    printf '%s: WARNING: macho9 is not next to me in %s; using whatever\n' "$0" "$MW_DIR" >&2
    printf '%s: "macho9" resolves to on PATH instead, which may not be the\n' "$0" >&2
    printf '%s: same build. MACHO9_COMPAT_DIR must name a directory that\n' "$0" >&2
    printf '%s: already has macho9-compat.sh and macho9-translate.sh in it\n' "$0" >&2
    printf '%s: (this one does), so silencing this means putting or linking\n' "$0" >&2
    printf '%s: the macho9 you want right there, not just anywhere on PATH\n' "$0" >&2
else
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
# A single temporary a wrapper has made BESIDE the caller's file, rather than
# inside MW_T -- so that whatever creates one does not also have to remember
# every exit path. Empty when there is none, which is every path today.
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
    # COMMANDS, not lines. `macho9 edit FILE -` carries its statements in a
    # here-document, so one command can be six lines; a command is a line that
    # STARTS with the program word (compat/translate.sh's output contract says
    # so, and mt_pre_word is where that word comes from). A statement line
    # cannot be mistaken for one -- every statement begins with its kind.
    MW_NCMDS=$(printf '%s\n' "$MW_CMDS" | awk -v p="$(mt_pre_word) " 'index($0, p) == 1 { n++ } END { print n + 0 }')
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
    # Indent the COMMAND lines only. A here-document's body and its terminator
    # have to start where they start: `MACHO9_EDIT` with four spaces in front
    # of it does not end the here-document, so an indented block would teach a
    # command that hangs when it is pasted. Leading whitespace before the
    # command itself is harmless, so the block still reads as a block.
    printf '%s\n' "$MW_CMDS" \
        | awk -v p="$(mt_pre_word) " '{ if (index($0, p) == 1) print "    " $0; else print }' >&2
    return 0
}

# ---- run -----------------------------------------------------------------
#
# mw_run -- run the translation, and return its exit code.
#
# THIS NO LONGER LOOPS, and that is the whole point of the change that removed
# the loop: an old invocation that would have been a sequence of macho9
# commands is now ONE `macho9 edit FILE -` with the operations as statements
# on stdin, so a translation is at most one command and there is no sequence
# left to step through. (compat/retag_swift_classes.sh is the one
# translation that is still several commands -- one per binary -- and it has
# always run its own lines itself, because it needs each file's own exit code
# and its own stdout, and one code for the whole script is not that.)
#
# The whole translation is eval'd as ONE script rather than line by line,
# because a here-document only reaches `macho9`'s stdin if the shell running
# the command also reads the lines that follow it. `</dev/null` is the default
# stdin for the script, so a command with no redirection of its own -- every
# verb line -- still cannot eat anything; the `edit` line's own here-document
# redirection overrides it, which is exactly what it is for.
mw_run() {
    eval "$MW_CMDS" </dev/null
}

# mw_require_writable FILE
#
# The absent/unwritable pre-check, in the words the C tools produced. Both
# fix_macho and rename_segment open()ed the file O_RDWR before looking at
# anything at all, so an absent or unwritable FILE failed immediately with
# perror("open"): `open: No such file or directory` or `open: Permission
# denied` -- no program name, on stderr, exit 1.
#
# A `dylib`/`rpath`/`lc`/`segment` command reproduces that for free, from
# mr_apply_file's own O_RDWR. The paths that do NOT are the ones that open the
# file some other way first: `macho9 edit` reads the image O_RDONLY and only
# discovers it cannot write when it writes, and rename_segment gates on
# `macho9 info`, which is O_RDONLY too. Either way the caller's first
# diagnostic would be a different message at a different time. It lives here
# rather than in a wrapper because two byte-for-byte copies of it in two
# wrappers is the thing this file exists not to have.
#
# Returns 1 rather than exiting, so the caller keeps the decision; both call
# sites read `mw_require_writable "$mw_file" || exit $?`. The two strings are
# a contract, not a message: tests/wrapper_test.sh and tests/known-callers.sh
# pin them.
mw_require_writable() {
    if [ ! -e "$1" ]; then
        printf 'open: No such file or directory\n' >&2
        return 1
    fi
    if [ ! -w "$1" ]; then
        printf 'open: Permission denied\n' >&2
        return 1
    fi
    return 0
}
