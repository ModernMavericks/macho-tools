#!/bin/sh
# rename_segment -- a /bin/sh wrapper around `machotool segment FILE OUT OLD NEW`.
#
#   rename_segment binary OLDNAME NEWNAME
#
# WHAT THIS REPLACED. compat/rename_segment.c was this tool's argument
# grammar, a thin-only mi_open + lseek/write driver, its exit 2 when nothing
# matched, and its one message, over its own mi_each_lc loop calling
# mseg_rename_lc (src/segname.h) on every matching command -- the same
# per-command function `machotool segment` calls, once per command, from inside
# mr_apply_file's own load-command walk. (rename_segment.c's own image-wide
# loop, mseg_rename_image, was deleted along with rename_segment.c itself:
# nothing else ever called it.) The rename itself is therefore the same code
# either way.
#
# Why the tool exists at all (10.9's libobjc looks for __objc_* sections in
# __DATA, and Xcode 10+ linkers put them in __DATA_CONST) is written down in
# src/segname.h's header, which outlives the C front-end this replaces.
#
# GRAMMAR. `rename_segment FILE OLD NEW` -> `machotool segment FILE OUT OLD NEW`.
# `argc != 4` and a NEW longer than the 16 bytes a segname field holds are
# both refused before any I/O, by compat/translate.sh, in rename_segment's own
# words.
#
# cli/machotool.c's cmd_segment lists FIVE DELIBERATE DIVERGENCES a wrapper has
# to account for, plus a note on a sixth that used to be on that list and no
# longer is (mg_plausible, below). The first of the five -- that the verb reads
# FILE and writes OUT rather than rewriting FILE -- is closed by "the in-place
# edit" at the end of this header; the rest are numbered below.
#
#   1. EXIT 2 WHEN NOTHING MATCHED, and 2. THE ONE-LINE MESSAGE. Both need the
#      same number: how many LC_SEGMENT_64s the rename actually matched.
#      rename_segment got it from mseg_rename_image's return value; this
#      wrapper gets it from `machotool segment`, which prints
#
#          machotool segment: renamed=<N>
#
#      -- the tool's own name, since the line is part of machotool's own
#      grammar and moved with the rename. No digest protects this text:
#      tests/EXPECTED and tests/known-callers.sh's sha256s hash converted
#      file bytes with the tools' output sent to /dev/null. What pins it is
#      four greps in three files, and they are the whole list:
#
#        * the sed below, the ONLY ONE THAT IS NOT A TEST -- production code
#          a caller depends on for the count;
#        * tests/wrapper_test.sh's two unmatched-report assertions, which
#          match whole lines beginning `machotool: `; and
#        * tests/cli_test.sh's `^machotool edit: ` prefix check.
#
#      Each of the four carries this same list, so the set is findable from
#      any one of them, and all four have to move with the strings they read.
#
#      on success -- one line, key=value, in the shape --capabilities already
#      established, and advertised as `verb segment reports=renamed` so this
#      wrapper can check the build provides it rather than assume. machotool's own
#      stdout is otherwise SUPPRESSED and this wrapper prints rename_segment's
#      single line with that count, byte-identical to the C tool's.
#
#      IT IS NOT DERIVED FROM `machotool info`. An earlier version of this wrapper
#      counted "  segname=NAME ..." lines out of that dump with awk, and it was
#      wrong twice over, both cases reachable and both measured: mseg_rename_lc
#      matches with strncmp over the 16-byte segname field, so an OLD LONGER
#      than 16 bytes whose first 16 match is a match the field-splitting count
#      missed, and a segname CONTAINING WHITESPACE (legal, and producible with
#      `machotool segment f out __TEXT 'A B'`) split across awk fields and missed too.
#      Both made this wrapper exit 2, leaving the file untouched, where the C
#      tool renamed and exited 0. tests/wrapper_test.sh pins both.
#
#      That was tests/README.md's second lesson -- never parse human-readable
#      output as an oracle -- applied to `machotool info` instead of to `otool`.
#      The count now comes from the code that did the matching.
#   3. THIN ONLY. rename_segment ran mi_open, which fails on a fat container,
#      and printed "%s: not a readable 64-bit Mach-O" (exit 1). `machotool
#      segment` goes through mr_apply_file, which HANDLES fat containers --
#      so it would rename inside a fat file that rename_segment refused
#      outright. `machotool info` is thin-only in exactly rename_segment's sense
#      (it is a bare mi_open), so gating on its EXIT STATUS reproduces the
#      old refusal. Its output is not read: the exit status is the whole
#      signal, which is the difference between using a machine-readable
#      result and parsing a human-readable one. This matters in practice:
#      most binaries under /System/Library/Frameworks are fat, so without the
#      gate tests/differential.sh would show this wrapper rewriting files the
#      C tool would not have.
#
#   4. LC_LAZY_LOAD_DYLIB, NOT CLOSED, and the one real gap this wrapper
#      ships with. mr_apply_file builds the library-ordinal map
#      (mo_map_build, src/ordinals.c) up front, before it looks at what the
#      operations actually are, and that builder REFUSES any image carrying
#      an LC_LAZY_LOAD_DYLIB -- "it carries an ordinal like LC_LOAD_DYLIB
#      does, but this codebase has never exercised renumbering it". A
#      segment rename touches no ordinal at all, so the refusal cannot be
#      protecting anything here; it is simply on the path. rename_segment,
#      which never went near mr_apply_file at all, renamed such a binary
#      happily.
#
#      MEASURED, on /usr/lib/libxcselect.dylib -- the one file in
#      tests/differential.sh's corpus that carries one:
#
#        compat/rename_segment.c (pre-wrapper)  renamed it, exit 0
#        machotool segment                         refuses, exit 1
#        machotool lc -delete uuid                 refuses too, with the SAME message
#        change_dylib (pre-wrapper)             refuses too, with the SAME message
#
#      The last two lines are the point: this is NOT rename-specific and NOT
#      something these wrappers introduced. mo_map_build has refused this
#      file for every operation, through every front-end, for as long as the
#      shared rewriter has existed. What changed is only that the rename now
#      travels through that rewriter.
#
#      It is the same SHAPE as the mg_plausible gate described below -- an
#      offset-related gate running on an operation set that cannot move
#      offsets -- but a different call site, so it does not fall out of
#      that fix. The smallest fix would be to skip building the ordinal map
#      when nothing in the operation set can renumber, which is a change to
#      machotool, not to this wrapper. Reported rather than made.
#
# mg_plausible USED TO BE a fifth divergence and no longer is: mr_apply_file
# used to run it before writing, on every operation, and refuse if it failed;
# rename_segment had no such gate. NOT reproduced HERE, because it is no
# longer a divergence: src/rewrite.c now skips that gate for a rename-only
# operation set, and says at the site why that is a statement about what
# mg_plausible checks (an OFFSET question) rather than a concession. A rename
# writes characters into segname/sectname and moves nothing, so the gate
# could only ever re-decide a property the input already had -- which it got
# wrong on 14 of the 16 thin binaries in a 120-file /usr/lib corpus.
#
# This wrapper therefore sets NO environment variable and switches nothing
# off. Every operation that can move an offset still meets the gate,
# including every one compat/change_dylib.sh can reach.
#
# EXIT CODES. 0 renamed, 2 nothing matched, 1 everything else -- the three
# rename_segment had. Every nonzero from `machotool segment` is mapped to 1
# rather than read directly, on purpose: "nothing matched" here is decided
# from the match COUNT (`mw_n -eq 0`, below), never from machotool's own exit
# code, so a coincidence between the two numberings is never load-bearing.
# That is just as well -- machotool's own EX_REFUSED is 1, the SAME number this
# wrapper uses for "everything else", not for "nothing matched" (its 2); this
# wrapper's mapping does not depend on which way that coincidence runs.
#
# THE IN-PLACE EDIT. rename_segment rewrote the binary it was given; `machotool
# segment FILE OUT OLD NEW` does not write the file it is given. So this
# wrapper takes the shared install path around its existing flow: mw_prepare
# names a temp beside the file FILE really is, mw_retranslate re-emits the
# command with that temp as OUT, and mw_finish mv's the temp over the target
# -- but only once the count says something was renamed, since the old grammar
# reported "nothing matched" as exit 2 with the file untouched.
# machotool-compat.sh's "the install path" section has the reasoning for each
# step.
#
# THE WRITABILITY CHECK comes with mw_prepare. rename_segment opened the file
# O_RDWR before it looked at it, so an unwritable (or absent) file failed
# immediately, with no analysis and no write. `machotool segment` opens FILE
# O_RDONLY now and has no opinion about whether FILE is writable, and the
# install by mv needs only the DIRECTORY writable -- so without this check the
# wrapper would rewrite files the C tool refused. `test -w` is not
# open(O_RDWR): it consults the real uid and does not see ACLs, so it can
# disagree at the edges. It agrees on the two cases that actually reach a
# caller (absent, and mode-denied), and both sides exit 1 either way.
#
# A HARD-LINKED FILE IS NOW REFUSED (exit 1), the one behaviour here the C tool
# did not have: it wrote through its own descriptor, so every link saw the
# rename, while an install by mv would leave the others on the old content.
# Every wrapper on this install path makes the same trade. And the temp needs
# the DIRECTORY writable, where the C tool needed only FILE itself to be, so a
# writable binary in a read-only directory now fails with FILE untouched.

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

mw_translate rename_segment "$@" || exit $?

mw_file=$1
mw_old=$2
mw_new=$3

mw_prepare "$mw_file" || exit 1

# THIN ONLY: `machotool info` is a bare mi_open, which is the gate
# rename_segment itself had. Only the exit status is used; the output is
# discarded, deliberately (see the FOURTH DIVERGENCE note above). Before the
# retranslate, so a fat FILE is refused without machotool ever being asked to
# write a temp for it.
if ! machotool info "$mw_file" >/dev/null 2>&1; then
    printf '%s: not a readable 64-bit Mach-O\n' "$mw_file" >&2
    exit 1
fi

mw_retranslate rename_segment "$@" || exit 1

mw_run >"$MW_T/segout" 2>&1 || { cat "$MW_T/segout" >&2; exit 1; }

# The match count, from the verb that did the matching. Anchored on the whole
# line, so nothing else machotool prints can be mistaken for it. This matches
# the line the binary PRINTS; divergence 1 in the header says what else reads
# machotool's emitted text and must move with it.
mw_n=$(sed -n 's/^machotool segment: renamed=\([0-9][0-9]*\)$/\1/p' "$MW_T/segout")
if [ -z "$mw_n" ]; then
    # The rename succeeded but this build's `machotool segment` did not report the
    # count, so there is no honest way to tell "renamed 0" (exit 2) from
    # "renamed some" (exit 0, with the number in the message). Fail loudly
    # rather than guess: `machotool --capabilities` advertises the signal as
    # `verb segment reports=renamed`, and a build without it is a mismatched
    # install, not a file this tool should report on.
    printf '%s: %s: this machotool did not report a rename count' "$MW_TOOL" "$mw_file" >&2
    printf ' (--capabilities should say "verb segment reports=renamed")\n' >&2
    cat "$MW_T/segout" >&2
    exit 1
fi

# NOTHING MATCHED: the old grammar's exit 2, and nothing is installed. machotool
# wrote the temp anyway -- a 0 exit means its output is the answer, even when
# that answer is a copy -- so the temp is exactly the bytes FILE already has
# and mw_finish would discard it. Not calling mw_finish at all says that more
# plainly, and mw_cleanup's EXIT trap removes the temp either way.
[ "$mw_n" -eq 0 ] && exit 2

mw_finish || exit 1

printf '%s: renamed %d segment(s) %s -> %s\n' "$mw_file" "$mw_n" "$mw_old" "$mw_new"
exit 0
