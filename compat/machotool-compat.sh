#!/bin/sh
# compat/machotool-compat.sh -- the machinery the historical tools' /bin/sh
# wrappers share. Sourced, never run:
#
#   MW_DIR=... ; . "$MW_DIR/machotool-compat.sh"
#
# Each wrapper is then a handful of lines of its own: translate this argv,
# teach the equivalent on stderr, run it, and map the exit code and stdout
# back to what the C tool it replaced would have produced. A wrapper whose
# verb has been converted to write an OUTPUT instead of rewriting its input
# has two more steps -- run it into a temp beside the caller's file, then
# install that temp over the file. ALL SIX take those two steps now, including
# patch_macho.sh, whose grammar has always named its own output: the temp goes
# beside that output and is installed onto it, which is also how `patch_macho
# IN IN` keeps working now that machotool refuses an OUT that is its input. See
# "the install path" below.
#
# WHY THE WRAPPERS ARE NOT SIX COPIES OF THIS. Task 1 put the whole
# old-grammar-to-machotool translation in ONE file (compat/translate.sh) so that
# "the translation that was tested is literally the translation that ships".
# The same argument applies to everything AROUND the translation -- finding
# machotool, printing the teaching message, running what was translated, and the
# pre-checks a wrapper has to make for itself because the command it is about
# to run would report them differently. Those are one implementation here, not
# six.
#
# ---- what lives WHERE, and why -------------------------------------------
#
#   compat/translate.sh      argv -> machotool command line(s). Pure text; runs
#                            nothing. Pinned by tests/translate_test.sh.
#   compat/machotool-compat.sh  this file: run those command lines safely.
#   compat/<tool>.sh         one per tool: only what that tool's observable
#                            behaviour needs that machotool's does not already
#                            give -- its exit-code mapping and its stdout.
#
# Every behavioural difference each wrapper has to close is documented at its
# site: the divergence list at the top of compat/translate.sh, and the
# "DELIBERATE DIVERGENCES FROM <tool>" blocks in cli/machotool.c's cmd_segment,
# cmd_retag_swift and cmd_declassify.
#
# ---- how a wrapper finds machotool and its two support files ----------------
#
# All four files -- machotool, machotool-translate.sh, machotool-compat.sh and the
# wrapper itself -- are installed FLAT, in the same bin directory, and a
# wrapper looks for the other three next to itself ($0's directory, resolved
# through PATH when $0 has no slash). $MACHOTOOL_COMPAT_DIR overrides that, which
# is how a build tree or a test harness points at an uninstalled set.
#
# Flat, rather than a libexec/ subdirectory, because of the shape of the one
# production caller: mavericksforever.com/claude/install.sh downloads the
# tools BY NAME into a single directory. A layout that needed a subdirectory
# would need that script changed; a flat one needs only the extra file names.
# (It needs those either way -- a wrapper cannot work without machotool present,
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
[ -r "$MW_DIR/machotool-translate.sh" ] || {
    printf '%s: cannot find machotool-translate.sh in %s -- machotool and its two\n' "$0" "$MW_DIR" >&2
    printf '%s: support files must be installed together (or set MACHOTOOL_COMPAT_DIR)\n' "$0" >&2
    exit 1
}

# PATH, not an absolute program word: the command lines translate.sh emits are
# the ones a human is being taught to type, so they must say `machotool` and mean
# the machotool that ships alongside this wrapper. Prepending the wrapper's own
# directory is what makes those two the same thing. tests/compat-sweep.sh runs
# the emitted lines the same way, for the same reason.
#
# The taught text is a pinned contract -- tests/known-callers.sh greps stderr
# for it, and tests/wrapper_test.sh pastes a taught block into a FRESH shell
# with nothing but PATH set and checks it still runs there -- so the word
# compat/translate.sh emits and the binary this finds have to be the same
# word. Both are `machotool`; the tool answered to `macho9` until the rename.
if [ -x "$MW_DIR/machotool" ]; then
    PATH="$MW_DIR:$PATH"
    export PATH
elif command -v machotool >/dev/null 2>&1; then
    # NOT the same hard failure as a missing machotool-translate.sh above,
    # because a caller may legitimately have installed machotool elsewhere on
    # PATH -- but it is NOT the machotool the header comment above promises
    # ("the one that ships alongside this wrapper"), so a version mismatch
    # here would be silent without this line. Warn and proceed rather than
    # refuse: refusing would break that legitimate case outright.
    printf '%s: WARNING: machotool is not next to me in %s; using whatever\n' "$0" "$MW_DIR" >&2
    printf '%s: "machotool" resolves to on PATH instead, which may not be the\n' "$0" >&2
    printf '%s: same build. MACHOTOOL_COMPAT_DIR must name a directory that\n' "$0" >&2
    printf '%s: already has machotool-compat.sh and machotool-translate.sh in it\n' "$0" >&2
    printf '%s: (this one does), so silencing this means putting or linking\n' "$0" >&2
    printf '%s: the machotool you want right there, not just anywhere on PATH\n' "$0" >&2
else
    printf '%s: machotool is not next to me in %s and not on PATH; this tool is a\n' "$0" "$MW_DIR" >&2
    printf '%s: wrapper around it and cannot do anything without it\n' "$0" >&2
    exit 1
fi

MT_SOURCED=1
export MT_SOURCED
. "$MW_DIR/machotool-translate.sh"

# A scratch directory for the wrappers that have to capture machotool's output in
# order to reshape it. Created once, removed on every exit path.
MW_T=$(mktemp -d "${TMPDIR:-/tmp}/machotool-compat.XXXXXX") || {
    printf '%s: cannot create a temporary directory\n' "$0" >&2
    exit 1
}
# A single temporary a wrapper has made BESIDE the caller's file, rather than
# inside MW_T -- so that whatever creates one does not also have to remember
# every exit path. mw_prepare names it, machotool writes it, mw_finish installs
# or discards it; empty whenever there is none to remove.
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
# mw_prepare's answers: the file FILE really is, and whether mw_finish ended
# up installing anything over it.
MW_TARGET=''
MW_CHANGED=0

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
#   2  no machotool command line means what this argv meant; same
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
    # COMMANDS, not lines. `machotool edit FILE OUT -` carries its statements in a
    # here-document, so one command can be six lines; a command is a line that
    # STARTS with the program word (compat/translate.sh's output contract says
    # so, and mt_pre_word is where that word comes from) -- or with `mv -f`,
    # the install step mt_install_line appends to the teaching form, which is
    # a command a reader would type too. A statement line cannot be mistaken
    # for either -- every statement begins with its kind.
    #
    # Through the environment, not `awk -v`, for the reason mw_run_to_tmp's own
    # comment gives at length: `-v` escape-processes what it assigns, so a
    # $MACHOTOOL containing a backslash would make awk look for a word the emitted
    # lines do not start with, and every command would go uncounted.
    MW_NCMDS=$(printf '%s\n' "$MW_CMDS" \
        | MW_PRE="$(mt_pre_word) " awk \
            'index($0, ENVIRON["MW_PRE"]) == 1 || index($0, "mv -f ") == 1 { n++ } END { print n + 0 }')
    mw_teach
    return 0
}

# The plan's phase one, in one function: "the caller's script keeps working,
# and the message teaches the new grammar". On STDERR, so stdout stays exactly
# what the C tool printed for anything reading it.
mw_teach() {
    if [ "$MW_NCMDS" -eq 0 ]; then
        printf '%s: deprecated -- machotool does this now. This invocation asks for nothing machotool would have to do.\n' \
            "$MW_TOOL" >&2
        return 0
    fi
    if [ "$MW_NCMDS" -eq 1 ]; then
        printf '%s: deprecated -- machotool does this now. The equivalent command is:\n' "$MW_TOOL" >&2
    else
        printf '%s: deprecated -- machotool does this now. The equivalent commands, in this order, are:\n' \
            "$MW_TOOL" >&2
    fi
    # Indent the COMMAND lines only. A here-document's body and its terminator
    # have to start where they start: `MACHOTOOL_EDIT` with four spaces in front
    # of it does not end the here-document, so an indented block would teach a
    # command that hangs when it is pasted. Leading whitespace before the
    # command itself is harmless, so the block still reads as a block. The
    # same two-part test mw_translate counts with, for the same reason.
    printf '%s\n' "$MW_CMDS" \
        | MW_PRE="$(mt_pre_word) " awk \
            '{ if (index($0, ENVIRON["MW_PRE"]) == 1 || index($0, "mv -f ") == 1) print "    " $0; else print }' >&2
    return 0
}

# ---- run -----------------------------------------------------------------
#
# mw_run -- run the translation, and return its exit code.
#
# THIS NO LONGER LOOPS, and that is the whole point of the change that removed
# the loop: an old invocation that would have been a sequence of machotool
# commands is now ONE `machotool edit FILE OUT -` with the operations as statements
# on stdin, so a translation is at most one command and there is no sequence
# left to step through. (compat/retag_swift_classes.sh is the one
# translation that is still several commands -- one per binary -- and it has
# always run its own lines itself, because it needs each file's own exit code
# and its own stdout, and one code for the whole script is not that.)
#
# The whole translation is eval'd as ONE script rather than line by line,
# because a here-document only reaches `machotool`'s stdin if the shell running
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
# NO machotool COMMAND REPRODUCES IT ANY MORE, and that is the point of the
# conversion rather than a gap in it: a verb that writes an output opens FILE
# O_RDONLY, so it has no opinion about whether FILE is writable -- it never
# writes FILE. (`dylib`/`rpath`/`lc`/`segment` used to give this refusal for
# free, from mr_apply_file's own O_RDWR; now, like `machotool edit`, they read
# FILE O_RDONLY and only discover an unwritable OUT when they write it.) And
# rename_segment gates on `machotool info`, which is O_RDONLY too. So preserving
# the historical refusal is permanently this layer's job, which is why
# mw_prepare calls this before anything runs.
#
# Returns 1 rather than exiting, so the caller keeps the decision. The two
# strings are a contract, not a message: tests/wrapper_test.sh and
# tests/known-callers.sh pin them.
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

# ---- the install path ----------------------------------------------------
#
# NO machotool VERB WRITES THE FILE IT IS GIVEN: each is `machotool VERB FILE OUT
# ...`, and each refuses an OUT that is FILE. They were converted one at a time
# -- `minos` first, then `retag-swift`, then `dylib`, `rpath`, `lc` and
# `segment` together, then `grow`, and last `edit`, whose OUT was a `--output`
# flag until then; `declassify` always had the shape and now refuses an OUT that
# is its IN as well. The historical tools DID
# edit FILE in place, and their callers still expect that, so a wrapper whose
# verb has moved reproduces it in the only way that is safe: write a temp
# beside the real target, then mv it over. The five functions below are that
# sequence, shared rather than copied into each wrapper as they arrive:
#
#   mw_prepare FILE       -> MW_TARGET, MW_TMPFILE   (and the refusals)
#   mw_retranslate TOOL ARG...                       -> MW_CMDS naming the temp
#   mw_run_to_tmp                                    -> run it, reshape stdout
#   mw_finish                                        -> install, or discard

# mw_resolve PATH -- print the file PATH finally names once every symlink in
# its last component is followed. 10.9's readlink has no -f, so this follows
# one level at a time. The install lands on that file, so a FILE that is a
# symlink stays one.
mw_resolve() {
    mw_p=$1
    mw_hops=0
    while [ -L "$mw_p" ]; do
        mw_hops=$((mw_hops + 1))
        [ "$mw_hops" -le 32 ] || { printf '%s: too many levels of symbolic links\n' "$1" >&2; return 1; }
        mw_l=$(readlink "$mw_p") || return 1
        case $mw_l in
            /*) mw_p=$mw_l ;;
            *)  case $mw_p in */*) mw_p=${mw_p%/*}/$mw_l ;; *) mw_p=$mw_l ;; esac ;;
        esac
    done
    printf '%s\n' "$mw_p"
}

# mw_prepare FILE [new-ok] -- set MW_TARGET to the file FILE really is and
# MW_TMPFILE to a fresh name beside it, for machotool to write. Refuses what
# cannot be replaced safely: a FILE this user could not have written (the C
# tools opened it read-write, and mv would otherwise replace it anyway), and
# a FILE with other hard links, which mv would leave on the old content.
# With `new-ok`, a FILE that does not exist yet is fine (patch_macho's OUT).
mw_prepare() {
    if [ "${2:-}" = new-ok ] && [ ! -e "$1" ] && [ ! -L "$1" ]; then
        MW_TARGET=$1
    else
        mw_require_writable "$1" || return 1
        MW_TARGET=$(mw_resolve "$1") || return 1
        # REGULAR FILES ONLY. A directory's link count is always greater than
        # one (`.`, its parent's entry, and one per subdirectory), so without
        # this gate `add_version_min somedir` would be refused as a hard-link
        # problem, with a remedy -- break the link -- that means nothing. A
        # directory is not something this check has an opinion about at all:
        # it falls through to machotool. Measured (both `machotool minos d out 10.9`
        # and `machotool retag-swift d out`): `d: cannot open or read` -- open()
        # O_RDONLY succeeds on a directory, so the failure is mi_open's own
        # read, not an open() rejecting it the way the C tools' open(O_RDWR)
        # did.
        mw_links=1
        if [ -f "$MW_TARGET" ]; then
            mw_links=$(stat -f %l "$MW_TARGET" 2>/dev/null) || mw_links=1
        fi
        if [ "$mw_links" -gt 1 ]; then
            printf '%s: %s has %d hard links; replacing it would leave the others with the old content. Break the link first, or run machotool with an explicit output.\n' \
                "$MW_TOOL" "$1" "$mw_links" >&2
            return 1
        fi
    fi
    case $MW_TARGET in
        */*) mw_dirpart=${MW_TARGET%/*}; mw_basepart=${MW_TARGET##*/} ;;
        *)   mw_dirpart=.;               mw_basepart=$MW_TARGET ;;
    esac
    [ -n "$mw_dirpart" ] || mw_dirpart=/
    MW_TMPFILE="$mw_dirpart/.$mw_basepart.machotool-compat.$$"
    rm -f -- "$MW_TMPFILE"
    return 0
}

# mw_retranslate TOOL ARG... -- translate again, this time writing MW_TMPFILE.
# The same argv mw_translate accepted a moment ago, with only the output
# named, so it cannot fail on its own: if it does, the translation depends on
# something it must not, and that is the one difference this can observe --
# it inspects the exit status, not the text.
mw_retranslate() {
    MT_PROG0=$0
    MW_CMDS=$(MT_OUT=$MW_TMPFILE mt_translate "$@")
    mw_trc=$?
    unset MT_PROG0
    if [ "$mw_trc" -ne 0 ]; then
        printf '%s: internal error: the translation is not stable under a change of output\n' "$MW_TOOL" >&2
        return 1
    fi
    return 0
}

# mw_run_to_tmp -- run the translation (which writes MW_TMPFILE) with its
# stdout captured, then pass every line through except the "Wrote <temp> (N
# bytes)" one, which no C tool ever printed and which names a file no caller
# has heard of. Returns machotool's status.
#
# Matched on the whole "Wrote <temp> (" prefix rather than on "Wrote " alone,
# and anywhere in the output rather than only on the last line: `machotool
# segment` follows its write with `machotool segment: renamed=N`, so for a
# fix_macho -rename_seg the temp-naming line is not the last one. A line naming
# anything else still comes through -- that is somebody's contract, not this
# function's to edit.
#
# THE PREFIX REACHES awk THROUGH THE ENVIRONMENT, NOT THROUGH `-v`, and that is
# not a style choice: `awk -v x=VALUE` runs VALUE through the same escape
# processing a string literal gets, so a path containing a backslash arrives at
# awk as something else and the line this function exists to suppress leaks
# through. Measured, before this was ENVIRON: `change_dylib 'back\slash/f'`
# printed `Wrote back\slash/.f.machotool-compat.NNNNN (8528 bytes)` on stdout.
# ENVIRON's values are taken verbatim (POSIX awk, and 10.9's), so the prefix awk
# compares is the real temp path. The whole point of a temp beside the caller's
# file is that its name is the caller's to choose, backslashes included -- so
# every awk in this file passes its needle the same way, mw_translate's and
# mw_teach's program-word tests included.
mw_run_to_tmp() {
    mw_run >"$MW_T/out"
    mw_rc=$?
    MW_WROTE_PREFIX="Wrote $MW_TMPFILE (" \
        awk 'index($0, ENVIRON["MW_WROTE_PREFIX"]) != 1' "$MW_T/out"
    return "$mw_rc"
}

# mw_finish -- after machotool wrote MW_TMPFILE: if it differs from MW_TARGET,
# mv it over (atomic: same directory), else discard it -- the C tools wrote
# nothing when nothing changed. Sets MW_CHANGED. Returns 1 only if the mv
# failed, leaving MW_TARGET as it was.
mw_finish() {
    MW_CHANGED=0
    if [ -e "$MW_TARGET" ] && cmp -s -- "$MW_TMPFILE" "$MW_TARGET"; then
        rm -f -- "$MW_TMPFILE"; MW_TMPFILE=''
        return 0
    fi
    if ! mv -f -- "$MW_TMPFILE" "$MW_TARGET"; then
        printf '%s: %s: the rewrite succeeded but installing it failed\n' "$MW_TOOL" "$MW_TARGET" >&2
        rm -f -- "$MW_TMPFILE"; MW_TMPFILE=''
        return 1
    fi
    MW_TMPFILE=''
    MW_CHANGED=1
    return 0
}
