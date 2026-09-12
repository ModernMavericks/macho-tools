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
# cmd_declassify, "FIVE DELIBERATE DIVERGENCES FROM patch_macho"): EX_REFUSED
# (1) where it examined the input and declined on purpose -- not a readable
# 64-bit Mach-O, no chained fixups to convert, any of declassify.h's LIMITS --
# and EX_FAIL (2) for an operational failure. So: ANY nonzero becomes 1. Zero
# stays zero. EX_REFUSED is already 1 and passes through unchanged -- which
# is all tests/leaf-tool-crashes.sh sees, checking for exit 1 on a fixture
# whose refusal reaches macho9 as EX_REFUSED. The mapping's real work is
# EX_FAIL (2) becoming 1, which tests/wrapper_test.sh checks on an absent IN
# and on an OUT whose directory cannot be written. The wrapper's OWN refusals
# -- an unwritable OUT, an OUT carrying other hard links, a failed install --
# exit 1 too, the only failure code this tool ever had.
#
# THE INSTALL, AND THE FOURTH OBSERVABLE. patch_macho created OUT with
# open(argv[2], O_WRONLY|O_CREAT|O_TRUNC, 0755) and wrote into it. That fixes
# more than the bytes:
#
#   * a FRESH OUT gets mode 0755 masked by the process umask -- 0700 under
#     `umask 077`, not 0755;
#   * an EXISTING OUT keeps whatever mode it already had, because open() does
#     not change the mode of a file it did not create;
#   * with IN == OUT it is the SAME INODE, so every hard link to it and every
#     xattr on it survives;
#   * and an existing OUT that is not writable makes it FAIL, even when the
#     directory is writable.
#
# `macho9 declassify` gives OUT the INPUT's mode (wa_write_new copies it),
# always a new inode, and refuses an OUT that is IN outright. So this wrapper
# does what the other five do -- macho9 writes a temp beside the real OUT
# (mw_prepare, with `new-ok`, since OUT need not exist yet), and mw_finish
# installs it with `mv`, atomically, or discards it when the bytes did not
# change -- with ONE step of its own before the install: the temp is chmod'ed
# to the mode the C tool would have left. OUT's own current mode when OUT
# exists (what `open()` preserved), and `0755 & ~umask` when it does not (what
# `open(..., 0755)` produced). File mode is a fourth observable alongside
# bytes, exit code and stdout, and it is reproduced exactly.
#
# WHAT THE ATOMIC INSTALL TRADES AWAY, which is the one behaviour a caller can
# see change: OUT's INODE, and with it OUT's hard links. `mv` gives OUT a fresh
# inode whenever the bytes differ, where `open(O_TRUNC)` wrote through the path
# and kept it. So an OUT with OTHER HARD LINKS is REFUSED (exit 1, mw_prepare's
# message) rather than silently split, exactly as it is for the five wrappers
# whose tools edited FILE in place -- and an unchanged run installs nothing at
# all, so an IN == OUT pass-through still keeps its inode, links and xattrs.
# What is gained is that OUT is never half-written: the C tool's open+write
# was not atomic, and neither was the `cat TEMP > OUT` this replaced. A
# dangling symlink at OUT is refused too (mw_prepare wants a writable OUT or no
# OUT at all), where the C tool created the link's target.
#
# THE OUT PRE-CHECKS NOW ANSWER BEFORE THE IN DIAGNOSIS, which only shows when
# IN and OUT are BOTH bad. The C tool ran md_declassify to completion and only
# then opened OUT, so a bad IN was always what it complained about; these checks
# have to come before macho9 runs, because they decide where macho9 writes. So
# `patch_macho notmacho unwritable_out` says `open: Permission denied` where the
# C tool said `notmacho: not a readable 64-bit Mach-O`, and `patch_macho
# notmacho adir` says `create output: Is a directory`. Exit 1 either way, on both
# sides, so no caller's control flow changes -- only which of two real problems
# is named first. compat/README.md's retag_swift_classes bullet records the same
# shape for the same reason: a wrapper's own pre-check answering ahead of the
# tool it wraps.
#
# The temp path is reached by re-translating the same argv with it in place
# of OUT, never by string-editing the emitted line: the file name reaches
# that line through mt_qargs' quoting, and unpicking that would be a second,
# worse parser.
#
# STDOUT. Everything md_declassify itself prints is identical on both sides (it
# is the same function). Two adjustments:
#
#   * macho9's trailing "Wrote <path> (N bytes)" line is DROPPED -- it names the
#     temp, and mw_run_to_tmp is what suppresses it, for every wrapper.
#   * On the CONVERTING path this wrapper prints `Wrote OUT (N bytes)` itself,
#     with N from OUT's size -- the same format string and the same two values
#     patch_macho printed. On the PASS-THROUGH path it prints nothing, because
#     patch_macho printed nothing: an already-converted input said only
#     "Already patched ... passing through." and never named the file it wrote.
#     cmd_declassify names that divergence deliberately ("a verb that copies a
#     file without saying so is the silent-success shape docs/PROPOSAL.md's
#     `verify` section exists to rule out").
#
# A pass-through is recognized by md_declassify's own "Already patched" line --
# macho9's stable stdout, the same oracle tests/cli_test.sh asserts against,
# and explicitly not otool/nm text (tests/README.md's second lesson).
#
# WHAT IS ACTUALLY COVERED, exactly, because a comment claiming more than that
# is the kind of defect this repo treats as a defect. tests/wrapper_test.sh pins
# the PASS-THROUGH path's stdout as a negative (no "Wrote " line at all) and the
# CONVERTING path's as the exact last line `Wrote OUT (N bytes)`, as EXACTLY ONE
# "Wrote " line -- so neither macho9's temp-naming line nor a doubled one can
# slip through -- and alongside md_declassify's own progress lines. Every mode
# case is pinned on both paths, and the IN == OUT install on both: the
# pass-through installing NOTHING (inode stands) and the conversion installing
# for real (new inode, converted bytes). Three mutations of that converting path
# -- installing the unconverted bytes, deleting the `Wrote OUT (N bytes)` line
# below, skipping the install when IN == OUT -- were each measured to leave
# every suite in this repo green before those assertions existed.
#
# 10.9's linker cannot emit chained fixups, so the CONVERTING path is reached
# with a HAND-BUILT fixture: tests/mkchained.c, shared by tests/wrapper_test.sh
# and tests/cli_test.sh, which is why that path runs natively here rather than
# only where tests/chained-fixups.sh -- whose fixture comes from the host linker,
# and which therefore SKIPs on 10.9 -- can run it.

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

mw_translate patch_macho "$@" || exit $?

mw_out=$2

# AN OUT THAT EXISTS BUT IS NOT A REGULAR FILE, refused here because neither of
# the two layers below would: macho9 writes a temp BESIDE OUT and never looks at
# OUT itself, and `mv` handed a directory as its destination moves the temp INTO
# it and reports success -- a run that exits 0 having created
# `OUT/.OUT.macho9-compat.PID` and nothing the caller asked for. The C tool's
# open(argv[2], O_WRONLY|O_CREAT|O_TRUNC, 0755) refused it with EISDIR, so the
# directory case gets that exact perror line back; anything else that is neither
# a regular file nor a directory (a fifo, a device) is not something this
# wrapper can install over with a rename either, and says so in its own words.
if [ -d "$mw_out" ]; then
    printf 'create output: Is a directory\n' >&2
    exit 1
fi
if [ -e "$mw_out" ] && [ ! -f "$mw_out" ]; then
    printf '%s: %s is not a regular file; refusing to replace it\n' "$MW_TOOL" "$mw_out" >&2
    exit 1
fi

# `new-ok`: this tool's OUT is the file it is asked to CREATE, so an OUT that
# does not exist yet is the ordinary case rather than the error it would be for
# the five wrappers whose argument is a binary to edit. Everything else
# mw_prepare refuses is shared with them.
mw_prepare "$mw_out" new-ok || exit 1
mw_retranslate patch_macho "$@" || exit 1

mw_run_to_tmp
mw_rc=$?
# A refusal leaves OUT untouched -- md_declassify runs to completion before the
# C tool ever opened OUT, so this matches, and it is why the temp exists.
[ "$mw_rc" -eq 0 ] || exit 1

# THE MODE THE C TOOL WOULD HAVE LEFT, applied to the temp before it is
# installed: macho9 gave it IN's mode, which is neither of the two answers
# open(argv[2], O_WRONLY|O_CREAT|O_TRUNC, 0755) gave.
if [ -e "$MW_TARGET" ]; then
    # An existing OUT keeps its own mode: open() does not change one on a file
    # it did not create. MW_TARGET, not $mw_out, so a symlinked OUT is asked
    # about its target -- the file the install lands on.
    mw_mode=$(stat -f %Lp "$MW_TARGET")
else
    # A fresh OUT gets what open(..., 0755) produced: 0755 narrowed by the
    # caller's umask -- 0700 under `umask 077`, not a bare 0755. `umask` prints
    # an octal number that may or may not carry a leading zero, so a `0` is
    # prepended to make the arithmetic octal either way.
    mw_umask=$(umask)
    mw_mode=$(printf '%o' "$(( 0755 & ~0$mw_umask ))")
fi
if ! chmod "$mw_mode" "$MW_TMPFILE" 2>/dev/null; then
    printf '%s: WARNING: could not chmod %s to %s; installing it with the mode\n' \
        "$0" "$mw_out" "$mw_mode" >&2
    printf '%s: macho9 gave it instead, which is the input file mode\n' "$0" >&2
fi

mw_finish || exit 1

# patch_macho named the file it wrote only when it had CONVERTED something.
if ! grep -q '^Already patched' "$MW_T/out"; then
    printf 'Wrote %s (%s bytes)\n' "$mw_out" "$(wc -c < "$mw_out" | tr -d ' ')"
fi
exit 0
