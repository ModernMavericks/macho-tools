#!/bin/sh
# add_version_min -- a /bin/sh wrapper around `macho9 minos FILE 10.9`.
#
#   add_version_min binary
#
# WHAT THIS REPLACED. compat/add_version_min.c was eighteen lines: an argc
# check and a call to mv_add_version_min (src/version_min.h). `macho9 minos
# FILE 10.9` calls that same function, so this is the one tool of the six
# whose wrapper has nothing to reshape.
#
# GRAMMAR. `add_version_min FILE` -> `macho9 minos FILE 10.9`. The version is
# spelled out because the C tool hardcoded 10.9 (mv_add_version_min only knows
# that floor); `macho9 minos` refuses any other, which is why the translation
# can name it literally rather than passing something through.
#
# EXIT CODES. Forwarded unchanged, no mapping added. The old C tool returned
# mv_add_version_min's own 0/1 (0 ok, 1 the flat "something went wrong" that
# function had no finer answer than); this wrapper forwards the SAME
# function's return today too, but its vocabulary is no longer that flat 0/1
# (src/rewrite.h): 0 ok, MR_REFUSED (1) for a considered refusal -- "not a
# readable 64-bit Mach-O", the race guard, "no room for
# LC_VERSION_MIN_MACOSX" -- or MR_FAIL (2) for a genuine open/fstat/write
# failure. A considered refusal still exits 1 here, matching the C tool by
# coincidence, not construction; an operational failure now exits 2, where
# the C tool always exited a flat 1 -- see compat/README.md's "drop-in"
# section for this as a named exception. `macho9 minos`' one exit code of
# its own (EX_REFUSED=1, for a version other than 10.9) is unreachable from
# here, since this wrapper only ever emits 10.9.
#
# STDOUT. Byte-identical, and not by construction alone -- every one of the
# five add_version_min rows in tests/compat-matrix.tsv compared equal on
# stdout. Both front-ends print only what mv_add_version_min itself prints
# ("Added LC_VERSION_MIN_MACOSX 10.9 (ncmds=..., sizeofcmds=...)", or
# "LC_VERSION_MIN_MACOSX already present; nothing to do."), so there is
# nothing here to suppress or synthesize.
#
# STDERR. mv_add_version_min's own diagnostics, unchanged, plus the teaching
# message this wrapper prints ahead of them.

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

mw_translate add_version_min "$@" || exit $?
mw_run
exit $?
