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
# EXIT CODES. Forwarded unchanged, and no mapping is added here: this
# wrapper runs the emitted `macho9 dylib`/`rpath`/`lc` line and exits
# whatever it exits, PAST THEIR OWN ARGUMENT CHECKS -- see the paragraph
# below for what those checks make unreachable here. The C tool returned
# mr_apply_file's own 0/1 (0 ok, 1 the flat "something went wrong" that
# rewriter had no finer answer than); mr_apply_file's vocabulary is no
# longer that flat 0/1 (rewrite.h): 0 ok, MR_REFUSED (1) for a considered
# refusal -- examined the input and declined, rewrite.h's own comment on
# mr_apply_file lists the cases, and this was ALWAYS true of mr_apply_file's
# behavior, just not numerically visible under the scheme that shipped
# first -- or MR_FAIL (2) for a genuine open/fstat/read/write/malloc
# failure. ONE EXCEPTION this wrapper can reach, with or without `-grow`:
# an allocation failure INSIDE mg_grow_header or mg_plausible (src/grow.c)
# is folded into MR_REFUSED, not MR_FAIL, same as every other reason either
# one refuses -- rewrite.c's own comment on that fold has the full
# reasoning. This wrapper reaches mg_grow_header only through
# `--allow-grow` (which `-grow` becomes), and only when the new load
# commands overflow the pad; mg_plausible needs no flag at all --
# mr_process_thin runs it on every rewrite that is not a pure segment
# rename (mr_is_rename_only), which is every rewrite this wrapper's
# `lc`/`dylib`/`rpath` lines can make, unless MACHO_NO_VERIFY is set in the
# environment. Apart from that fold, and the multi-verb seam described
# below, a considered refusal still exits 1 here, matching the C tool by
# coincidence, not construction; an operational failure now exits 2, where
# the C tool always exited a flat 1 -- see compat/README.md's "drop-in"
# section for this as a named exception.
#
# --fatal-warnings is a SEPARATE fact, not what makes the paragraph above
# true or conditional: this translation never emits that flag -- change_
# dylib's own grammar has no spelling for it, and never will, since
# `-change` matching nothing has always exited 0 and that is compat
# surface -- so the ONE mr_apply_file behavior that flag specifically adds
# (turning "an operation matched nothing" from a report into MR_REFUSED) is
# simply never reached through this wrapper. If translate.sh ever grows a
# --fatal-warnings-shaped flag, this paragraph is the one to update.
# Argument checks are unreachable from here regardless -- the translation
# validates every -strip-lc KIND against the same table (src/lc_kinds.c)
# before emitting, and never emits a verb with no operation.
#
# ONE-VERB VS MULTI-VERB: "forwarded unchanged, no mapping" is exactly true
# only when this translation emits a single line -- macho9-compat.sh's
# mw_run_atomic hands straight to mw_run and returns its raw exit code
# unmapped. A run needing more than one family (`-change` AND `-strip-lc`
# together, say) goes through mw_run_atomic's copy-aside-and-install dance
# instead, which has THREE hardcoded `return 1`s of its own that are NOT
# macho9's exit code at all -- a failed `cp` aside, an unstable
# re-translation under the temp file's name, or a failed install back over
# the original -- and the first two fire before macho9 ever runs, the last
# one after. An absent FILE is the case where this is visible: single-verb,
# it reaches mr_apply_file's own open() and exits 2 (MR_FAIL); multi-verb,
# `cp -p` fails on the same absent file BEFORE any macho9 command runs, and
# mw_run_atomic's hardcoded path returns 1. Not a bug to fix here --
# mw_run_atomic's own `return 1`s are exactly right for the historical-
# mapping wrappers (fix_macho.sh) that share it -- just a real seam this
# wrapper's own "forwarded unchanged" claim has to be read around.
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
