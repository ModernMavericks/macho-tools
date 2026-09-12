#!/bin/sh
# add_version_min -- a /bin/sh wrapper around `macho9 minos FILE OUT 10.9`.
#
#   add_version_min binary
#
# WHAT THIS REPLACED. compat/add_version_min.c was eighteen lines: an argc
# check and a call to mv_add_version_min (src/version_min.h). `macho9 minos`
# calls that same function, so the only thing this wrapper has to reshape is
# WHERE THE RESULT LANDS.
#
# GRAMMAR. `add_version_min FILE` -> `macho9 minos FILE OUT 10.9`. The version
# is spelled out because the C tool hardcoded 10.9 (mv_add_version_min only
# knows that floor); `macho9 minos` refuses any other, which is why the
# translation can name it literally rather than passing something through.
#
# THE IN-PLACE EDIT. add_version_min rewrote FILE; macho9 never writes the
# file it is given. So this wrapper does what the old tool looked like it did,
# safely: mw_prepare names a temp beside the file FILE really is (following
# symlinks, refusing an unwritable FILE or one with other hard links),
# mw_retranslate re-emits the command with that temp as OUT, mw_run_to_tmp
# runs it, and mw_finish mv's the temp over the target -- or discards it when
# the bytes did not change, since the C tool wrote nothing in that case.
# macho9-compat.sh's "the install path" section has the reasoning for each
# step; all of it is shared, none of it is this wrapper's own.
#
# EXIT CODES. macho9's, forwarded unchanged, with the wrapper's own refusals
# at 1. The old C tool returned mv_add_version_min's own 0/1 (0 ok, 1 the flat
# "something went wrong" that function had no finer answer than); this wrapper
# forwards the SAME function's return today too, but its vocabulary is no
# longer that flat 0/1 (src/rewrite.h): 0 ok, MR_REFUSED (1) for a considered
# refusal -- "not a readable 64-bit Mach-O" (including mi_open's own
# MI_NOT_MACHO) or "no room for LC_VERSION_MIN_MACOSX" -- or MR_FAIL (2) for a
# genuine open/fstat failure, mi_open's own I/O errors (a second, independent
# open of the same path, which can fail on its own even though this function's
# own earlier open succeeded), or a failure to write OUT. A considered refusal
# still exits 1 here, matching the C tool by coincidence, not construction; an
# operational failure now exits 2, where the C tool always exited a flat 1
# -- see compat/README.md's "drop-in" section for this as a named
# exception. `macho9 minos`' two exit codes of its own are unreachable from
# here: EX_REFUSED=1 for a version other than 10.9, since this wrapper only
# ever emits 10.9, and EX_FAIL=2 for an OUT that is FILE, since mw_prepare
# names a temp that is neither. The wrapper's OWN refusals -- an absent or
# unwritable FILE (`open: ...`, mw_require_writable's words, which are the C
# tool's own open() failing), a hard-linked FILE, and a failed install -- all
# exit 1, the only failure code this tool ever had.
#
# STDOUT. What mv_add_version_min prints ("Added LC_VERSION_MIN_MACOSX 10.9
# (ncmds=..., sizeofcmds=...)", or "LC_VERSION_MIN_MACOSX already present;
# nothing to do.") -- byte-identical to the C tool's, and not by construction
# alone: every one of the five add_version_min rows in tests/compat-matrix.tsv
# compared equal on stdout. The one line this wrapper does suppress is
# `macho9 minos`'s closing "Wrote OUT (N bytes)", which names a temp file no
# caller has ever heard of and no C tool ever printed; mw_run_to_tmp drops it.
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
mw_prepare "$1" || exit 1
mw_retranslate add_version_min "$@" || exit 1
mw_run_to_tmp
mw_rc=$?
[ "$mw_rc" -eq 0 ] || exit "$mw_rc"
mw_finish || exit 1
exit 0
