#!/bin/sh
# patch_macho -- a /bin/sh wrapper around `macho9 declassify IN OUT`.
#
#   patch_macho input output
#
# WHAT THIS REPLACED. compat/patch_macho.c was this tool's `IN OUT` grammar,
# its own open+write of the output file, its messages and its exit code, over
# src/declassify.c's md_declassify -- the same function `macho9 declassify`
# calls. The conversion (chained fixups lowered to LC_DYLD_INFO_ONLY) is
# therefore the same code either way, and the OUTPUT FILE'S BYTES are
# identical by construction: both front-ends write the very buffer
# md_declassify hands back.
#
# This is the tool mavericksforever.com/claude/install.sh's generated
# /usr/local/bin/claude wrapper runs FIRST, and the tool whose idempotency
# that wrapper depends on (an already-converted binary passes through
# unchanged). Both are covered by tests/known-callers.sh.
#
# GRAMMAR. `patch_macho IN OUT` -> `macho9 declassify IN OUT`. Nothing else;
# `argc != 3` is a usage error on both sides, reproduced by
# compat/translate.sh in patch_macho's own words.
#
# EXIT CODES -- MAPPED. patch_macho returns a FLAT 1 for everything that goes
# wrong. `macho9 declassify` tells two kinds of wrong apart (cli/macho9.c's
# cmd_declassify, "FOUR DELIBERATE DIVERGENCES FROM patch_macho"): EX_REFUSED
# (2) where it examined the input and declined on purpose -- not a readable
# 64-bit Mach-O, no chained fixups to convert, any of declassify.h's LIMITS --
# and 1 for an operational failure. So: ANY nonzero becomes 1. Zero stays
# zero. tests/leaf-tool-crashes.sh depends on this, checking for exit 1 on a
# fixture whose refusal reaches macho9 as EX_REFUSED.
#
# STDOUT -- ONE LINE SUPPRESSED. Everything md_declassify itself prints is
# identical on both sides (it is the same function). The single difference,
# measured over tests/compat-matrix.tsv's patch_macho rows and reproduced by
# hand: on the PASS-THROUGH path -- an input that is already converted --
# `macho9 declassify` prints a "Wrote OUT (N bytes)" line that patch_macho
# never printed. cmd_declassify names that as a deliberate divergence: "a verb
# that copies a file without saying so is the silent-success shape
# docs/PROPOSAL.md's `verify` section exists to rule out."
#
# patch_macho reports that line only when it actually CONVERTED something, so
# this wrapper drops it again when the run was a pass-through. A pass-through
# is recognized by md_declassify's own "Already patched ... passing through."
# line -- macho9's stable stdout, the same oracle tests/cli_test.sh asserts
# against, and explicitly not otool/nm text (tests/README.md's second lesson).
# Only the LAST line is ever dropped, and only if it starts with "Wrote ", so
# a future message change degrades to printing one extra line rather than
# eating a real one. tests/wrapper_test.sh pins both halves.
#
# On the CONVERTING path nothing is dropped: patch_macho's own "Wrote %s (%zu
# bytes)" and cmd_declassify's are the same format string with the same two
# values, so passing macho9's through is byte-identical. 10.9's linker cannot
# emit chained fixups, so that path is exercised by tests/chained-fixups.sh on
# a modern host (it SKIPs here) rather than natively.
#
# STDERR. macho9's, plus the teaching message. It differs from patch_macho's
# in one enumerated place: for an unreadable input patch_macho printed
# "IN: not a readable 64-bit Mach-O" and macho9 prefixes and expands that.
# No caller byte-compares stderr; the one that shows it (install.sh's wrapper)
# shows it to a human.
#
# THE WRITE ITSELF. patch_macho created OUT with open(O_CREAT|O_TRUNC, 0755)
# and wrote into it; macho9 goes through wa_write_atomic (same mode, same
# bytes, no half-written OUT on failure). One consequence worth naming: where
# OUT already exists and is not writable but its directory is, patch_macho
# failed and macho9 succeeds.

MW_SELF=$(command -v "$0" 2>/dev/null) || MW_SELF=$0
MW_DIR=${MACHO9_COMPAT_DIR:-$(dirname "$MW_SELF")}
. "$MW_DIR/macho9-compat.sh"

mw_translate patch_macho "$@" || exit $?

mw_run >"$MW_T/out" 2>"$MW_T/err"
mw_rc=$?
cat "$MW_T/err" >&2

if [ "$mw_rc" -ne 0 ]; then
    cat "$MW_T/out"
    exit 1
fi

if grep -q '^Already patched' "$MW_T/out" &&
   [ "$(sed -n '$p' "$MW_T/out" | cut -c1-6)" = 'Wrote ' ]; then
    sed '$d' "$MW_T/out"
else
    cat "$MW_T/out"
fi
exit 0
