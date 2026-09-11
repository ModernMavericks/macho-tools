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
# (1) where it examined the input and declined on purpose -- not a readable
# 64-bit Mach-O, no chained fixups to convert, any of declassify.h's LIMITS --
# and EX_FAIL (2) for an operational failure. So: ANY nonzero becomes 1. Zero
# stays zero. tests/leaf-tool-crashes.sh depends on this, checking for exit 1
# on a fixture whose refusal reaches macho9 as EX_REFUSED.
#
# THE WRITE, AND THE FOURTH OBSERVABLE. patch_macho created OUT with
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
# `macho9 declassify` writes through wa_write_atomic, which mkstemps beside
# OUT, fchmods 0755 and renames -- so it produces 0755 regardless of umask,
# regardless of OUT's previous mode, and always a NEW inode. Every one of the
# four bullets above differs. File mode and inode are a fourth observable
# alongside bytes, exit code and stdout, and this wrapper reproduces all four
# rather than enumerating them:
#
#   macho9 writes a TEMP file, and this wrapper installs it over OUT with
#   `cat TEMP > OUT` -- writing THROUGH the path, exactly as the C tool's
#   open(O_TRUNC) did. An existing OUT keeps its inode, mode, hard links and
#   xattrs; an IN == OUT run keeps them too; an unwritable OUT fails. Only for
#   an OUT that does not exist yet does anything have to be chosen, and there
#   the wrapper creates it and chmods it to `0755 & ~umask` -- the mode the C
#   tool's open() would have produced -- before writing a byte into it.
#
#   What that gives up is the same one thing mw_run_atomic gives up
#   (compat/macho9-compat.sh): the final copy is not atomic. Neither was the C
#   tool's open(O_TRUNC)+write, so this matches it rather than diverging from
#   it -- macho9's atomic write happens, but into the temp.
#
#   The temp path is reached by re-translating the same argv with it in place
#   of OUT, never by string-editing the emitted line -- same rule
#   mw_run_atomic follows and for the same reason.
#
# STDOUT. Everything md_declassify itself prints is identical on both sides (it
# is the same function). Two adjustments:
#
#   * macho9's trailing "Wrote <path> (N bytes)" line is DROPPED, always,
#     because it now names the temp file rather than OUT.
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
# tests/wrapper_test.sh pins both paths' stdout and all four mode cases.
#
# 10.9's linker cannot emit chained fixups, so the CONVERTING path is exercised
# by tests/chained-fixups.sh on a modern host (it SKIPs here) rather than
# natively; the mode assertions do not depend on which path ran.

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

mw_in=$1
mw_out=$2

# Run the conversion into a temp, so installing it over OUT can be the same
# write-through-the-path the C tool did. Re-translated rather than
# string-edited; the first translation of this argv already succeeded, so a
# failure here means the translation is not stable under a change of file name
# and nothing should run.
MT_PROG0=$0
MW_CMDS=$(mt_translate patch_macho "$mw_in" "$MW_T/converted")
mw_trc=$?
unset MT_PROG0
if [ "$mw_trc" -ne 0 ]; then
    printf '%s: internal error: the translation is not stable under a change of file name\n' \
        "$MW_TOOL" >&2
    exit 1
fi

mw_run >"$MW_T/out" 2>"$MW_T/err"
mw_rc=$?
cat "$MW_T/err" >&2

# A refusal leaves OUT untouched -- md_declassify runs to completion before the
# C tool ever opened OUT, so this matches, and it is why the temp exists.
if [ "$mw_rc" -ne 0 ]; then
    cat "$MW_T/out"
    exit 1
fi

# Drop macho9's own "Wrote ..." line: it names the temp.
if [ "$(sed -n '$p' "$MW_T/out" | cut -c1-6)" = 'Wrote ' ]; then
    sed '$d' "$MW_T/out"
else
    cat "$MW_T/out"
fi

# Create OUT with the mode the C tool's open(..., 0755) would have produced,
# BEFORE any content goes into it, and only when it does not already exist --
# an existing OUT keeps its own mode, because open() does not change one.
if [ ! -e "$mw_out" ]; then
    # printf '' rather than `:` -- `:` is a POSIX SPECIAL BUILTIN, and a
    # redirection error on one exits a non-interactive shell on the spot.
    # Under /bin/sh that prints the shell's own diagnostic alongside this one;
    # under ksh this one never runs at all. printf is a regular builtin, so a
    # failure comes back here to be reported in this tool's own words.
    #
    # `2>/dev/null` BEFORE the create, not after: redirections are applied
    # left to right, so putting it first means the shell's own "cannot create"
    # diagnostic for the failing redirection lands there instead of on the
    # caller's stderr, leaving just this tool's message -- one line, as the C
    # tool's perror("create output") was. Verified under /bin/sh and /bin/ksh.
    # umask 077 for the CREATE only, not the eventual mode: a plain `>`
    # redirect requests mode 0666, which umask can only NARROW, never widen
    # -- it can never produce the execute bits 0755 needs, so a chmod after
    # the fact is unavoidable in plain /bin/sh. Forcing the strictest
    # possible umask for JUST the create means the window between create and
    # chmod is 0600 (nothing for group/other) rather than "0666 & ~the
    # caller's real umask", which for a permissive umask (0, say) would leave
    # OUT briefly world-WRITABLE -- more permissive than open(..., 0755)
    # ever produces. The real umask, captured below before this subshell can
    # touch it, is what decides the FINAL mode; this only protects the gap.
    mw_umask=$(umask)
    if ! ( umask 077; printf '' 2>/dev/null > "$mw_out" ); then
        printf 'create output: cannot create %s\n' "$mw_out" >&2
        exit 1
    fi
    if ! chmod "$(printf '%o' "$(( 0755 & ~0$mw_umask ))")" "$mw_out" 2>/dev/null; then
        printf '%s: WARNING: could not chmod %s to %s; leaving it at the more\n' \
            "$0" "$mw_out" "$(printf '%o' "$(( 0755 & ~0$mw_umask ))")" >&2
        printf '%s: restrictive mode the create step used instead\n' "$0" >&2
    fi
fi

# Install through the path: inode, mode, hard links and xattrs all survive,
# and a symlinked OUT is followed to its target -- exactly what
# open(O_WRONLY|O_TRUNC) did.
if ! cat "$MW_T/converted" > "$mw_out"; then
    printf 'write: cannot write %s\n' "$mw_out" >&2
    exit 1
fi

# patch_macho named the file it wrote only when it had CONVERTED something.
if ! grep -q '^Already patched' "$MW_T/out"; then
    printf 'Wrote %s (%s bytes)\n' "$mw_out" "$(wc -c < "$mw_out" | tr -d ' ')"
fi
exit 0
