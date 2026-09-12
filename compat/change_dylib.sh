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
#     -strip-lc KIND        macho9 lc    FILE OUT -delete KIND
#     -change O N           macho9 dylib FILE OUT -replace O N
#     -delete P                          ... -delete P
#     -reexport P                        ... -reexport P
#     -add P                             ... -append P
#     -insert P                          ... -insert P
#     -change-rpath O N     macho9 rpath FILE OUT -replace O N
#     -delete-rpath P                    ... -delete P
#     -add-rpath P                       ... -append P
#     -grow                 --allow-grow on the dylib/rpath lines
#
# -- one verb, batching that family's flags, when the invocation touches one
# family. An invocation touching MORE THAN ONE becomes a single
# `macho9 edit FILE OUT -` with the operations as statements on stdin, ordered
# load-command, dylib, rpath, because deleting load commands hands header pad
# back and the other two consume it. compat/translate.sh's emission comment
# has the statement order within each family, and the one -change shape it
# refuses on that path because no sequence reproduces the old batch.
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
# wrapper runs the emitted `macho9 dylib`/`rpath`/`lc`/`edit` command and
# exits whatever it exits, PAST THEIR OWN ARGUMENT CHECKS -- see the
# paragraph below for what those checks make unreachable here. The codes
# this wrapper produces itself are all 1 -- mw_prepare's absent, unwritable
# and hard-linked refusals, and a failed install -- which is the only failure
# code change_dylib ever had (tests/compat-matrix.tsv's change_dylib rows are
# a flat 1 on every captured failure). The C tool returned
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
# `lc`/`dylib`/`rpath`/`edit` commands can make, unless MACHO_NO_VERIFY is
# set in the environment. Apart from that fold, a considered refusal still
# exits 1 here, matching the C tool by coincidence, not construction; an
# operational failure now exits 2, where the C tool always exited a flat 1
# -- see compat/README.md's "drop-in" section for this as a named exception.
# `macho9 edit` speaks the same vocabulary for the same reasons: me_run
# returns MR_REFUSED or MR_FAIL straight from the statement that produced
# it, so the multi-family path is not a separate exit-code regime.
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
# STDOUT. Measured over all 1110 generated change_dylib combinations plus the
# hand-picked ones (tests/compat-matrix.tsv). The row counts below are that
# measurement, taken while a multi-family invocation was still a SEQUENCE of
# verbs; what those rows now run is one `macho9 edit`, so the counts still say
# how many invocations are of each shape, and the second bullet describes a
# different difference than it used to.
#
#   ONE emitted command  -- 459 rows -- stdout is byte-identical. Both sides
#       are one mr_apply_file pass over the same file with the same ops. Two
#       lines of macho9's are reshaped to get there, and both are consequences
#       of the verb writing a temp instead of FILE: mw_run_to_tmp drops its
#       "Wrote <temp> (N bytes)" line, which names a file no caller has heard
#       of, and this wrapper prints "Updated FILE (N bytes)" itself after the
#       install, only when the bytes changed -- the same line mr_apply_file
#       used to print, under the same condition, naming the same path.
#   MORE THAN ONE FAMILY -- 669 rows -- one `macho9 edit`, and stdout DIFFERS,
#       unavoidably: each statement is its own pass over the image, so a
#       "header pad ..." / "updated ..." pair is printed PER STATEMENT where
#       one invocation printed one pair. The closing "Updated FILE (N bytes)"
#       line IS there -- this wrapper prints it, on the same terms as above,
#       rather than `edit` printing it. Every line that IS there names FILE,
#       because that is the path macho9 was handed. Reproducing the C tool's
#       exact transcript would mean suppressing macho9's output and inventing
#       a plausible one, which is worse than a difference.
#   NO command at all -- `change_dylib FILE -grow -grow` -- 2 rows: the C tool
#       still ran an empty mr_apply_file pass and printed
#       its "header pad ..." and "nothing to change." lines; the translation
#       is empty by design (there is no macho9 command that means "do
#       nothing"), so nothing is printed. No caller does this.
#
# No caller found by Task 0 parses this tool's stdout as data; the ones that
# look at it at all redirect it to /dev/null.
#
# ATOMICITY OF A MIXED-FAMILY INVOCATION. install.sh's production line strips
# two load commands AND rewrites three dylib paths. tests/compat-sweep.sh
# measured what splitting that one atomic rewrite into a sequence of verbs
# cost: two rows where the C tool refused having written nothing, while the
# sequence refused having already written. One `macho9 edit` closes that at
# the source rather than around it -- me_run reads the image once, applies
# every statement to it in memory, verifies, and writes once, so a refusal at
# any statement leaves FILE exactly as it was. That is the C tool's shape, not
# an approximation of it. (It now writes the temp this wrapper installs, not
# FILE, so a refusal leaves no temp to install either.)
#
# ---- THE IN-PLACE EDIT ---------------------------------------------------
#
# change_dylib rewrote FILE; `macho9 dylib`/`rpath`/`lc` do not write the file
# they are given. So this wrapper does what the old tool looked like it did,
# safely, through the shared sequence macho9-compat.sh's "the install path"
# section documents: mw_prepare names a temp beside the file FILE really is,
# mw_retranslate re-emits the command with that temp as its output,
# mw_run_to_tmp runs it, and mw_finish mv's the temp over the target -- or
# discards it when the bytes did not change, since the C tool wrote nothing in
# that case. `macho9 edit`, the multi-family path, takes that temp as its OUT
# positional like every other verb here, so both shapes install identically.
#
# THE REFUSALS THIS BUYS, all of them mw_prepare's and all exit 1:
#
#   AN ABSENT OR UNWRITABLE FILE. change_dylib open()ed FILE O_RDWR before it
#     looked at anything, so either failed immediately with perror("open"),
#     having changed nothing. No macho9 command reproduces that any more: a
#     verb that writes an output opens FILE O_RDONLY and has no opinion about
#     whether FILE is writable, and `macho9 edit` installs by rename, which
#     needs the DIRECTORY writable and never consults FILE's own mode
#     (measured before this check existed: a mode-444 binary replaced, fresh
#     inode, exit 0 -- a silent rewrite of a file its owner marked
#     read-only). mw_require_writable, inside mw_prepare, gives the C tool's
#     own two strings instead. `test -e`/`test -w` are not open(O_RDWR) --
#     they consult the real uid and do not see ACLs -- so they can disagree at
#     the edges; they agree on the two cases that reach a caller, and both
#     follow a symlink, which is what is wanted, since the install lands on
#     the symlink's target and it is that file's mode that decides.
#   A HARD-LINKED FILE. New, and the one behaviour a caller can see that no
#     version of change_dylib had: the C tool wrote through its own
#     descriptor, so every link saw the change, while an install by mv would
#     leave the others on the old content. Refused rather than silently split,
#     which is the trade every wrapper on this path makes; macho9-compat.sh's
#     mw_prepare has the message and the remedy.
#
# ONE MORE CONSEQUENCE WORTH NAMING: creating a temp beside FILE and renaming
# it needs the DIRECTORY writable, where the C tool needed only FILE itself to
# be. So a writable binary inside a read-only directory, which change_dylib
# patched, now fails -- `mkstemp: Permission denied`, from macho9's own write
# of the temp, with FILE untouched. compat/add_version_min.sh's header records
# the same shape for the same reason.

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

mw_prepare "$1" || exit 1

# NOTHING TO RUN. `change_dylib FILE -grow -grow` asks for no operation at all,
# so compat/translate.sh emits no command (its "exit 0 with zero lines" case)
# and there is no temp for macho9 to write or for mw_finish to install. The C
# tool ran an empty rewrite pass over FILE and printed its "header pad ..." and
# "nothing to change." lines; nothing prints them here, which is the divergence
# translate.sh's "no command at all" bullet already records. The refusals above
# still apply: the C tool opened FILE O_RDWR before doing nothing.
[ "$MW_NCMDS" -eq 0 ] && exit 0

mw_retranslate change_dylib "$@" || exit 1
mw_run_to_tmp
mw_rc=$?
[ "$mw_rc" -eq 0 ] || exit "$mw_rc"
mw_finish || exit 1
# The C tool's closing line, in its own words, only when the bytes changed --
# see "STDOUT" above. It names FILE as the caller spelled it, which is what
# mr_apply_file's own "Updated %s" printed when this verb still wrote FILE.
[ "$MW_CHANGED" -eq 1 ] \
    && printf 'Updated %s (%s bytes)\n' "$1" "$(wc -c < "$MW_TARGET" | tr -d ' ')"
exit 0
