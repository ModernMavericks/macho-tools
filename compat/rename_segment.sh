#!/bin/sh
# rename_segment -- a /bin/sh wrapper around `macho9 segment FILE OLD NEW`.
#
#   rename_segment binary OLDNAME NEWNAME
#
# WHAT THIS REPLACED. compat/rename_segment.c was this tool's argument
# grammar, a thin-only mi_open + lseek/write driver, its exit 2 when nothing
# matched, and its one message, over mseg_rename_image (src/segname.h) -- the
# same function `macho9 segment` reaches through mr_apply_file. The rename
# itself is therefore the same code either way.
#
# Why the tool exists at all (10.9's libobjc looks for __objc_* sections in
# __DATA, and Xcode 10+ linkers put them in __DATA_CONST) is written down in
# src/segname.h's header, which outlives the C front-end this replaces.
#
# GRAMMAR. `rename_segment FILE OLD NEW` -> `macho9 segment FILE OLD NEW`.
# `argc != 4` and a NEW longer than the 16 bytes a segname field holds are
# both refused before any I/O, by compat/translate.sh, in rename_segment's own
# words.
#
# cli/macho9.c's cmd_segment lists THREE DELIBERATE DIVERGENCES a wrapper has
# to account for, and running the file through `macho9 info` first is what
# closes two of them at once -- it answers "is this the kind of file
# rename_segment would touch?" and "how many segments does OLD name?" from
# macho9's own stable output, without this wrapper re-deriving either.
#
#   1. EXIT 2 WHEN NOTHING MATCHED. mr_apply_file reports "nothing to change"
#      and exits 0; rename_segment exits 2. mseg_rename_image returns the
#      match count, and `macho9 info` prints one "  segname=NAME ..." line per
#      LC_SEGMENT_64, so the count is available here: zero matches means exit
#      2, before anything is run and without touching the file -- which is
#      also what rename_segment did (it wrote nothing in that case).
#   2. STDOUT. rename_segment prints exactly one line. `macho9 segment` prints
#      mr_apply_file's header-pad/updated chatter instead. So macho9's stdout
#      is SUPPRESSED and this wrapper prints rename_segment's own line, with
#      the count from step 1 -- byte-identical to the C tool's, on every
#      rename_segment row of tests/compat-matrix.tsv that produced output.
#   3. mg_plausible. mr_apply_file used to run it before writing, on every
#      operation, and refuse if it failed; rename_segment had no such gate.
#      NOT reproduced HERE, because it is no longer a divergence: src/rewrite.c
#      now skips that gate for a rename-only operation set, and says at the
#      site why that is a statement about what mg_plausible checks (an OFFSET
#      question) rather than a concession. A rename writes characters into
#      segname/sectname and moves nothing, so the gate could only ever
#      re-decide a property the input already had -- which it got wrong on 14
#      of the 16 thin binaries in a 120-file /usr/lib corpus.
#
#      This wrapper therefore sets NO environment variable and switches
#      nothing off. Every operation that can move an offset still meets the
#      gate, including every one compat/change_dylib.sh can reach.
#
# A FOURTH DIVERGENCE, not in that list, found by this task: THIN ONLY.
# rename_segment ran mi_open, which fails on a fat container, and printed
# "%s: not a readable 64-bit Mach-O" (exit 1). `macho9 segment` goes through
# mr_apply_file, which HANDLES fat containers -- so it would rename inside a
# fat file that rename_segment refused outright. `macho9 info` is thin-only in
# exactly rename_segment's sense (it is a bare mi_open), so gating on it
# reproduces the old refusal. This matters in practice: most binaries under
# /System/Library/Frameworks are fat, so without the gate tests/differential.sh
# would show this wrapper rewriting files the C tool would not have.
#
# A FIFTH DIVERGENCE, NOT CLOSED, and the one real gap this wrapper ships
# with: LC_LAZY_LOAD_DYLIB. mr_apply_file builds the library-ordinal map
# (mo_map_build, src/ordinals.c) up front, before it looks at what the
# operations actually are, and that builder REFUSES any image carrying an
# LC_LAZY_LOAD_DYLIB -- "it carries an ordinal like LC_LOAD_DYLIB does, but
# this codebase has never exercised renumbering it". A segment rename touches
# no ordinal at all, so the refusal cannot be protecting anything here; it is
# simply on the path. rename_segment, which never went near mr_apply_file at
# all, renamed such a binary happily.
#
# MEASURED, on /usr/lib/libxcselect.dylib -- the one file in
# tests/differential.sh's corpus that carries one:
#
#   compat/rename_segment.c (pre-wrapper)  renamed it, exit 0
#   macho9 segment                         refuses, exit 1
#   macho9 lc -delete uuid                 refuses too, with the SAME message
#   change_dylib (pre-wrapper)             refuses too, with the SAME message
#
# The last two lines are the point: this is NOT rename-specific and NOT
# something these wrappers introduced. mo_map_build has refused this file for
# every operation, through every front-end, for as long as the shared rewriter
# has existed. What changed is only that the rename now travels through that
# rewriter.
#
# It is the same SHAPE as divergence 3 -- an ordinal-related gate running on
# an operation set that cannot renumber -- but a different call site, so it
# does not fall out of that fix. The smallest fix would be to skip building
# the ordinal map when nothing in the operation set can renumber, which is a
# change to macho9, not to this wrapper. Reported rather than made.
#
# EXIT CODES. 0 renamed, 2 nothing matched, 1 everything else -- the three
# rename_segment had. Every nonzero from `macho9 segment` is mapped to 1: its
# own EX_REFUSED is 2, which HERE would be read as "nothing matched", the one
# thing it does not mean.
#
# THE WRITABILITY CHECK. rename_segment opened the file O_RDWR before it
# looked at it, so an unwritable (or absent) file failed immediately, with no
# analysis and no write. macho9 writes through wa_write_atomic, which can
# replace a read-only file whose DIRECTORY is writable -- so without this
# check the wrapper would rewrite files the C tool refused. `test -w` is not
# open(O_RDWR): it consults the real uid and does not see ACLs, so it can
# disagree at the edges. It agrees on the two cases that actually reach a
# caller (absent, and mode-denied), and both sides exit 1 either way.

MW_SELF=$(command -v "$0" 2>/dev/null) || MW_SELF=$0
MW_DIR=${MACHO9_COMPAT_DIR:-$(dirname "$MW_SELF")}
. "$MW_DIR/macho9-compat.sh"

mw_translate rename_segment "$@" || exit $?

mw_file=$1
mw_old=$2
mw_new=$3

if [ ! -e "$mw_file" ]; then
    printf 'open: No such file or directory\n' >&2
    exit 1
fi
if [ ! -w "$mw_file" ]; then
    printf 'open: Permission denied\n' >&2
    exit 1
fi

if ! macho9 info "$mw_file" >"$MW_T/info" 2>"$MW_T/infoerr"; then
    printf '%s: not a readable 64-bit Mach-O\n' "$mw_file" >&2
    exit 1
fi

# Exact string equality on the whole field, never a regex: OLD is caller data
# and may contain regex metacharacters. `macho9 info` prints one
# "  segname=NAME vmaddr=..." line per LC_SEGMENT_64, so awk's first
# whitespace-separated field is exactly "segname=" plus the name.
mw_n=$(awk -v want="segname=$mw_old" '$1 == want { n++ } END { print n + 0 }' "$MW_T/info")

[ "$mw_n" -eq 0 ] && exit 2

mw_run >/dev/null || exit 1
printf '%s: renamed %d segment(s) %s -> %s\n' "$mw_file" "$mw_n" "$mw_old" "$mw_new"
exit 0
