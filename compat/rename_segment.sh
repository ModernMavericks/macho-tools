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
#   3. mg_plausible. mr_apply_file runs it before writing and refuses if it
#      fails; rename_segment had no such gate. REPRODUCED, by setting
#      MACHO_NO_VERIFY=1 for the `macho9 segment` run -- the one thing that
#      variable gates (src/rewrite.c's "Last gate before the bytes reach
#      disk") and nothing else. That is not a decision taken lightly, so here
#      is the whole argument:
#
#      MEASURED. tests/differential.sh, run over 120 real Mach-Os from
#      /usr/lib and friends against a pre-wrapper build, reported this verb
#      refusing 14 thin 64-bit system dylibs that rename_segment renamed
#      happily -- "refusing to modify f -- it would carry base-relative
#      offsets that name no known function". Different exit code AND different
#      bytes, on 14 of the 16 thin inputs in that corpus. Left in, the wrapper
#      would simply stop working on most real binaries.
#
#      AND THE GATE CANNOT BE PROTECTING ANYTHING HERE. mg_plausible needs no
#      "before" image: it judges the FINAL bytes. A segment rename edits
#      segname/sectname CONTENT only -- never a cmd, never a cmdsize, never an
#      offset (src/segname.h says so, and it is why mr_build_lcs can apply it
#      to an already-copied command) -- so it cannot make a plausible image
#      implausible. A refusal here is therefore always about a property the
#      input ALREADY had, which is exactly what cli/macho9.c's own divergence
#      note says: "not because the rename is unsafe, but because the image was
#      already implausible before anything touched it". Those 14 dylibs are
#      stock 10.9 system libraries; the heuristic is wrong about them, and
#      tests/change_dylib_test.sh's case 8 already passes MACHO_NO_VERIFY=1
#      for the same reason on a fixture of its own.
#
#      SCOPE. Only this wrapper sets it, and only when the caller has not
#      already spoken. compat/change_dylib.sh does NOT: change_dylib always
#      went through mr_apply_file, so it always had this gate, and taking it
#      away there would be a real change rather than a reproduction.
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
# simply on the path. rename_segment, which never went near an ordinal map,
# renamed such a binary happily.
#
# MEASURED: exactly one file in tests/differential.sh's 120-file /usr/lib
# corpus (/usr/lib/libxcselect.dylib). Different exit code and different bytes
# there. Not closed here because closing it means changing `macho9 segment` --
# skipping the ordinal map when the operation set contains nothing that can
# renumber -- and changing macho9 is not this wrapper's business. Reported as
# a finding instead; there is no MACHO_NO_VERIFY-shaped escape hatch for it.
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

# MACHO_NO_VERIFY: see divergence 3 in this file's header for why, and for
# why change_dylib's wrapper deliberately does not do this. Exported rather
# than set as a command prefix because the run goes through `eval`, and only
# when the caller has expressed no opinion of their own.
MACHO_NO_VERIFY=${MACHO_NO_VERIFY:-1}
export MACHO_NO_VERIFY

mw_run >/dev/null || exit 1
printf '%s: renamed %d segment(s) %s -> %s\n' "$mw_file" "$mw_n" "$mw_old" "$mw_new"
exit 0
