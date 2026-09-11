#!/bin/sh
# change_dylib -- a /bin/sh wrapper around macho9's `lc`, `dylib` and `rpath`.
#
#   change_dylib input [-grow] [-change old new] [-delete path]
#                 [-reexport path] [-add path] [-insert path]
#                 [-strip-lc name] [-change-rpath old new]
#                 [-delete-rpath path] [-add-rpath path] ...
#
# WHAT THIS REPLACED. compat/change_dylib.c was, by the end, ONLY this
# grammar: it parsed argv into an mr_ops and handed it to mr_apply_file
# (src/rewrite.h). macho9's `dylib`, `rpath` and `lc` verbs parse their own
# grammar into the same mr_ops and call the same function, so every byte of
# the rewrite -- and every line it prints -- is the same code either way.
#
# This is the tool with the most callers: mavericksforever.com/claude/
# install.sh's generated /usr/local/bin/claude wrapper, this repo's own
# characterize.sh and chained-fixups.sh, and a local script of the repo
# owner's. All four are replayed end to end by tests/known-callers.sh.
#
# GRAMMAR. compat/translate.sh holds the whole mapping and the reasoning; in
# brief, one family per verb --
#
#     -strip-lc KIND        macho9 lc    FILE -delete KIND
#     -change O N           macho9 dylib FILE -replace O N
#     -delete P                          ... -delete P
#     -reexport P                        ... -reexport P
#     -add P                             ... -append P
#     -insert P                          ... -insert P
#     -change-rpath O N     macho9 rpath FILE -replace O N
#     -delete-rpath P                    ... -delete P
#     -add-rpath P                       ... -append P
#     -grow                 --allow-grow on the dylib/rpath lines
#
# -- emitted in the order lc, dylib, rpath, because deleting load commands
# hands header pad back and the other two consume it.
#
# THE CAPACITY CAPS ARE ENFORCED IN THE TRANSLATION, not by macho9. Both cap
# sites in cli/macho9.c carry a comment saying so and addressing whoever wrote
# this wrapper: macho9 caps at the same numbers (MR_MAX_OPS=32, MR_MAX_STRIP=16)
# but names ITS grammar's flags, so passing its message through would print
# "too many -append" where change_dylib printed "too many -add". mt_room in
# compat/translate.sh prints the origin text and refuses before anything runs.
# (This is also what keeps the fixed-size arrays' historical stack smash --
# "33 -change flags smashed the stack", docs/PROPOSAL.md -- fixed rather than
# reintroduced in shell.)
#
# EXIT CODES. Forwarded unchanged, and no mapping is needed: change_dylib
# returned mr_apply_file's own 0/2 (0 ok, 2 an operational failure -- see
# cli/macho9.c's top-of-file comment for the scheme), and `dylib`, `rpath`
# and `lc` still do too PAST THEIR OWN ARGUMENT CHECKS -- for this wrapper
# specifically. That stopped being true of `dylib`/`rpath`/`lc` in general the
# moment macho9 grew `--fatal-warnings` (mr_apply_file can now also return
# MR_REFUSED, which cli/macho9.c forwards as EX_REFUSED=1): it stays true HERE
# only because this translation never emits that flag -- change_dylib's own
# grammar has no spelling for it, and never will, since `-change` matching
# nothing has always exited 0 and that is compat surface. If translate.sh
# ever grows a --fatal-warnings-shaped flag, this comment is the one to
# update. Argument checks are unreachable from here regardless -- the
# translation validates every -strip-lc KIND against the same table
# (src/lc_kinds.c) before emitting, and never emits a verb with no
# operation.
#
# STDOUT. Measured over all 1110 generated change_dylib combinations plus the
# hand-picked ones (tests/compat-matrix.tsv):
#
#   ONE emitted command  -- 459 rows -- stdout is byte-identical. Both sides
#       are one mr_apply_file pass over the same file with the same ops.
#   TWO OR THREE         -- 669 rows -- stdout DIFFERS, unavoidably: a
#       sequence prints one "header pad ..." / "updated ..." pair PER PASS
#       where one invocation printed one pair. Reproducing the C tool's exact
#       transcript would mean suppressing macho9's output and inventing a
#       plausible one, which is worse than a difference. The lines themselves
#       are the same lines, in the same order, with the per-pass pair
#       repeated; nothing is missing.
#       Those lines also name the TEMP COPY rather than FILE -- see below.
#   NO command at all -- `change_dylib FILE -grow` (and -grow repeated) --
#       2 rows: the C tool still ran an empty mr_apply_file pass and printed
#       its "header pad ..." and "nothing to change." lines; the translation
#       is empty by design (there is no macho9 command that means "do
#       nothing"), so nothing is printed. No caller does this.
#
# No caller found by Task 0 parses this tool's stdout as data; the ones that
# look at it at all redirect it to /dev/null.
#
# ATOMICITY OF A MIXED-FAMILY INVOCATION. install.sh's production line strips
# two load commands AND rewrites three dylib paths, which is two macho9
# commands. tests/compat-sweep.sh measured what splitting one atomic rewrite
# into a sequence costs: two rows where the C tool refused having written
# nothing, while the sequence refused having already written. mw_run_atomic
# (compat/macho9-compat.sh) closes that -- a sequence runs against a copy and
# the copy is installed only on full success -- and its header says exactly
# what that shell dance does NOT preserve that wa_write_atomic does.

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

mw_translate change_dylib "$@" || exit $?

mw_run_atomic change_dylib "$@"
exit $?
