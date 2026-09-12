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
# -- one verb, batching that family's flags, when the invocation touches one
# family. An invocation touching MORE THAN ONE becomes a single
# `macho9 edit FILE -` with the operations as statements on stdin, ordered
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
# paragraph below for what those checks make unreachable here. The one code
# this wrapper produces itself is the unwritable-FILE guard's 2, and it is
# chosen to be the code the same file would have got from the command it
# stands in front of; "the unwritable-FILE guard" below has the reasoning.
# The C tool returned
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
#       are one mr_apply_file pass over the same file with the same ops.
#   MORE THAN ONE FAMILY -- 669 rows -- one `macho9 edit`, and stdout DIFFERS,
#       unavoidably: each statement is its own pass over the image, so a
#       "header pad ..." / "updated ..." pair is printed PER STATEMENT where
#       one invocation printed one pair, and the final "Updated FILE (N
#       bytes)" line is mr_apply_file's, which `edit` does not call, so it is
#       not there at all. Every line that IS there names FILE, because that is
#       the path macho9 was handed. Reproducing the C tool's exact transcript
#       would mean suppressing macho9's output and inventing a plausible one,
#       which is worse than a difference.
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
# two load commands AND rewrites three dylib paths. tests/compat-sweep.sh
# measured what splitting that one atomic rewrite into a sequence of verbs
# cost: two rows where the C tool refused having written nothing, while the
# sequence refused having already written. One `macho9 edit` closes that at
# the source rather than around it -- me_run reads the image once, applies
# every statement to it in memory, verifies, and writes once through
# wa_write_atomic, so a refusal at any statement leaves FILE exactly as it
# was. That is the C tool's shape, not an approximation of it.
#
# ---- the unwritable-FILE guard -------------------------------------------
#
# change_dylib open()ed FILE O_RDWR before it looked at anything, so a
# mode-denied FILE failed immediately, having changed nothing. A `dylib`,
# `rpath` or `lc` command still reproduces that for free -- mr_apply_file
# opens O_RDWR up front and perror()s "open" -- so the ONE emitted command is
# the whole story for an invocation touching one family.
#
# `macho9 edit` is the one that does not. me_run reads the image O_RDONLY and
# installs its result through wa_write_atomic, which mkstemps BESIDE FILE and
# renames over it -- an operation that needs the DIRECTORY writable and never
# consults FILE's own mode. Measured: a mode-444 binary is replaced (fresh
# inode, mode 444 carried onto it) and the run exits 0. That is a silent
# rewrite of a file its owner marked read-only, and the exact shape of edit
# this whole conversion exists to make visible rather than to introduce.
#
# So the wrapper refuses first, and refuses in the observable the other path
# already produces: `open: Permission denied` on stderr and exit 2, the same
# text and the same code mr_apply_file's own open failure gives, so the two
# paths agree rather than each having its own answer. Deliberately NOT
# mw_require_writable, which returns 1 (fix_macho's flat code, right there and
# wrong here) and which also captures an ABSENT FILE -- that one has no such
# problem and must keep reaching macho9, whose open failure reports it.
#
# `test -e`/`test -w` are not open(O_RDWR): they consult the real uid and do
# not see ACLs, so they can disagree with it at the edges -- the same caveat
# mw_require_writable's own header states, for the same reason. They agree on
# the case that reaches a caller, a file whose MODE denies writing. Both
# follow a symlink, which is what is wanted: wa_write_atomic resolves one and
# rewrites the target, so it is the target's mode that decides, and a symlink
# whose target does not exist is not `-e` and falls through to macho9.

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

# EXISTS and is not writable -- see "the unwritable-FILE guard" above. Both
# halves matter: an absent FILE falls through to macho9 on purpose.
if [ -e "$1" ] && [ ! -w "$1" ]; then
    printf 'open: Permission denied\n' >&2
    exit 2
fi

mw_run
exit $?
