#!/bin/sh
# fix_macho -- a /bin/sh wrapper around machotool's `lc`, `dylib` and `segment`.
#
#   fix_macho <file> [-change old new] [-strip_build_version]
#             [-rename_seg old new] ...
#
# WHAT THIS REPLACED. compat/fix_macho.c was this tool's argument grammar over
# its own hand-rolled load-command walk: a `-change` that wrote a new path INTO
# the existing dylib command, a `-strip_build_version` that memmove'd
# LC_BUILD_VERSION out of the table, a `-rename_seg` that strncpy'd into
# segname/sectname, a fat loop of its own, and an lseek+write back over the
# original file.
#
# GRAMMAR. compat/translate.sh holds the whole mapping and the reasoning; in
# brief, one verb per family --
#
#     -strip_build_version  machotool lc      FILE OUT -delete build-version
#     -change O N           machotool dylib   FILE OUT -replace O N
#     -rename_seg O N       machotool segment FILE OUT O N    (one line per pair)
#
# -- when the invocation is ONE command's worth. Anything more than that --
# which includes two -rename_seg pairs, since `machotool segment` takes one --
# becomes a single `machotool edit FILE OUT -` with the operations as statements on
# stdin, ordered load-command, dylib, segment, because deleting a load command
# hands header pad back and the dylib rewrite consumes it. A rename changes no
# sizes, so it can only go last. `argc < 3`, a trailing `-change`/`-rename_seg`
# with a missing operand, an unknown flag, a NEW segment name longer than the
# 16 bytes a segname field holds, and both capacity caps are all refused before
# any I/O, by compat/translate.sh, in fix_macho's own words.
#
# THE CAPACITY CAPS ARE ENFORCED IN THE TRANSLATION. fix_macho held -change in
# a `changes[32]` and -rename_seg in a `renames[16]`, and for most of its life
# neither had a bounds check -- the same defect docs/PROPOSAL.md records being
# found and fixed in change_dylib alone ("Repeated options wrote past their
# fixed-size arrays; 33 -change flags smashed the stack -- fixed, PR #9"). The
# C file grew an FM_ROOM check before it retired; mt_room in
# compat/translate.sh is where that check lives now, printing the same "too
# many -change (max 32)" / "too many -rename_seg (max 16)" and refusing before
# anything runs. The -rename_seg cap in particular exists NOWHERE ELSE: machotool
# sees one rename at a time either way -- its `segment` verb takes one pair,
# and an edit script's `segment rename` statement is one pair -- so it has no
# cap of its own to hit.
#
# ---- DELIBERATE DIVERGENCES FROM fix_macho -------------------------------
#
# The other five wrappers close their tool's divergences. This one does NOT,
# and that is the point: the repo owner ruled these five differences
# improvements to ADOPT rather than behaviour to preserve
# (docs/superpowers/plans/2026-09-10-report-what-macho9-did.md, "The decision
# this plan rests on"). Every one of them is a case where fix_macho and the
# shared rewriter give different answers, and the shared rewriter's is better.
#
#   1. A REPLACEMENT PATH LONGER THAN THE EXISTING COMMAND now SUCCEEDS.
#      fix_macho wrote the new path into the existing LC_LOAD_DYLIB and
#      refused if it did not fit ("new path '...' too long (320 > 32)", exit 1,
#      file untouched). `machotool dylib -replace` rebuilds the load-command
#      table and fits the longer path into existing header pad, exit 0.
#      WHY ADOPTING IT IS RIGHT: the limit was an artifact of a rewriter that
#      never learned to resize a command, not a safety property. Nothing in
#      docs/PROPOSAL.md records a reason for it, and the previous plan states
#      the rule outright -- "machotool does NOT have to inherit the old tools'
#      artificial limits". Note the translation still emits no --allow-grow:
#      this uses pad the image already has, and does not enlarge the header.
#
#   2. A CHAINED -rename_seg now CHAINS. `-rename_seg __DATA __X -rename_seg
#      __X __Y` produced __X under fix_macho, which applied every pair in ONE
#      pass and gave each segment its FIRST match, so the second pair never
#      fired. Each pair is its own pass here -- its own `segment rename`
#      statement in the emitted edit script -- and the second reads the
#      first's result, so it produces __Y.
#      WHY ADOPTING IT IS RIGHT: doing what was asked. compat/translate.sh
#      REFUSED this shape outright until now -- correctly, while a wrapper had
#      to preserve fix_macho's answer -- and its -rename_seg arm records why
#      that refusal existed and what reversed it.
#
#   3. THE WRITE-BACK IS ATOMIC. fix_macho lseek'd to 0 and wrote the whole
#      file back over itself, so a crash, a full disk or a kill mid-write left
#      a corrupt binary. machotool never writes the file at all now: it writes a
#      temp beside it (wa_write_new, src/atomic_write.h -- mkstemp + rename,
#      carrying FILE's mode, owner and xattrs) and this wrapper installs that
#      temp with one mv, in the same directory. So the caller's file is either
#      wholly old or wholly new, and a failure anywhere leaves it wholly old.
#      WHY ADOPTING IT IS RIGHT: these tools exist to make binaries loadable;
#      a half-written one is the failure they are supposed to prevent.
#      NO CAVEAT ANY MORE. An invocation worth more than one command is one
#      `machotool edit FILE OUT -`, and me_run (src/edit.c) reads the
#      image once, applies every statement to it in memory, verifies, and
#      writes once -- so a refusal at any statement leaves the temp unwritten
#      and FILE exactly as it was, with no second write to be caught between.
#
#   4. A FAT SLICE WHOSE EDIT FAILS now REFUSES THE WHOLE FILE.
#      fix_macho's fat loop treated EVERY per-slice failure the same way: its
#      process_macho returned -1 whether the slice was not a Mach-O at all or
#      was one whose edit it refused ("new path too long", "malformed dylib
#      load command"), and the loop printed "  Skipping arch %u" and carried
#      on, exiting 0 having rewritten the slices it did understand -- a
#      partially converted universal binary reported as a success.
#      mr_process_fat (src/rewrite.c) splits those two cases: MR_SKIP for a
#      slice that is not a 64-bit Mach-O, MR_ERROR for one that IS and whose
#      edit failed, and only MR_ERROR refuses -- "refusing the whole fat file
#      -- a partial rewrite would leave its slices inconsistent".
#      WHY ADOPTING IT IS RIGHT: refuse rather than guess, the codebase's
#      standing rule, and the "silent success" failure docs/PROPOSAL.md's
#      "verify" section is about.
#      MEASURED, AND NARROWER THAN THE PLAN'S TABLE SAYS. That table describes
#      this as "refuses the whole file" against fix_macho's "prints Skipping
#      arch %u and carries on", which reads as covering both cases. It does
#      not: a slice that is simply NOT a 64-bit Mach-O is left unchanged here
#      exactly as fix_macho left it, with a different message ("not a 64-bit
#      Mach-O; leaving this slice unchanged") and exit 0.
#      tests/wrapper_test.sh asserts that skip explicitly, on a hand-built
#      two-slice container, so the distinction cannot be quietly widened.
#
#   5. A `-change` AIMED AT THIS DYLIB'S OWN INSTALL NAME now MATCHES
#      NOTHING, instead of rewriting it. compat/fix_macho.c's match block
#      opened on `mo_is_ordinal_lc(lc->cmd) || lc->cmd == LC_ID_DYLIB` and
#      then ran the `changes[]` comparison loop with NO LC_ID_DYLIB
#      exclusion -- so `fix_macho -change <this dylib's own install name>
#      NEW` rewrote the dylib's identity, even though the file's own comment
#      said "nothing in `changes` is ever meant to match it": the code
#      matched it anyway. src/rewrite.c:220 guards it (`if (lc->cmd !=
#      LC_ID_DYLIB) { /* never rewrite this dylib's own identity */`).
#      MEASURED, on copies of the same `-install_name /tmp/aaa/libfoo.dylib`
#      dylib:
#
#          old:  Changed: /tmp/aaa/libfoo.dylib -> /tmp/bbb/libfoo.dylib
#                File updated: a.dylib          rc=0   otool -D -> /tmp/bbb/libfoo.dylib
#          new:  machotool: /tmp/aaa/libfoo.dylib matched nothing
#                b.dylib: nothing to change.    rc=0   otool -D -> /tmp/aaa/libfoo.dylib
#          cmp a.dylib b.dylib -> differ
#
#      BOTH SIDES EXIT 0 AND THE BYTES DIFFER, and nothing on stderr named
#      the reason -- which is exactly the invisible edit this whole plan
#      exists to make visible.
#      WHY ADOPTING IT IS RIGHT: (a) install_name_tool spells identity `-id`
#      and its `-change` never touches LC_ID_DYLIB -- machotool matches the
#      tool everyone already knows; (b) fix_macho.c's own comment stated the
#      contract machotool now enforces, so this is the C being fixed, not the C
#      being contradicted; (c) silently rewriting a dylib's own install name
#      from an operation the caller aimed at a DEPENDENCY is precisely the
#      invisible edit this whole plan exists to make visible.
#      tests/wrapper_test.sh asserts this on a dylib fixture: a `-change`
#      naming the dylib's own install name leaves LC_ID_DYLIB unchanged and
#      is reported unmatched, while a `-change` in the SAME invocation aimed
#      at a real dependency still lands.
#
# STDOUT IS NOT REPRODUCED, and that is deliberate too. fix_macho printed
# "Processing thin Mach-O:" / "Processing arch N at offset M:" / "  Changed: X
# -> Y" / "  Removed LC_BUILD_VERSION (N bytes)" / "  Renamed segment 'A' ->
# 'B'" / "File updated: F" / "No changes needed: F". None of it survives; what
# a caller sees now is machotool's own reporting, plus -- for the one line that
# carried information a caller could act on -- the unmatched report this
# plan's Task 1 added:
#
#     machotool: /usr/lib/libFoo.dylib matched nothing        (a -change)
#     machotool: no load command of kind build-version to delete
#
# on STDERR, per operation, naming the operation that matched nothing. That is
# strictly more than "No changes needed: F" said, which could not distinguish
# which of several operations missed. `machotool --fatal-warnings` would turn that
# report into a refusal; THIS WRAPPER MUST NOT PASS IT. fix_macho exited 0 when
# an operation matched nothing, that is compat surface, and
# tests/known-callers.sh and tests/wrapper_test.sh are the gates.
#
# Those two lines are quoted verbatim, `machotool:` prefix and all, because
# that is what a caller really sees -- the report names the operation in
# machotool's grammar, which is the grammar this wrapper teaches (src/rewrite.c's
# mr_report_unmatched says why the prefix is the tool's name and not argv[0]).
# Neither tests/EXPECTED nor tests/known-callers.sh's sha256s have an opinion
# -- both hash converted file bytes with the tools' output sent to /dev/null.
# What reads these two lines is tests/wrapper_test.sh.
#
# ---- two more differences, which are NOT on the adopted list -------------
#
# Both are consequences of travelling through mr_apply_file at all rather than
# choices this conversion made, both are shared with every other verb that
# rewrites, and both are reported here rather than worked around:
#
#   mg_plausible. mr_apply_file runs that gate before writing (except for a
#     rename-only operation set, which src/rewrite.c skips because the gate
#     asks an OFFSET question and a rename moves no offset). fix_macho had no
#     such gate, so an image the gate rejects is one this refuses and fix_macho
#     rewrote. It is a check on the INPUT, not on what the rewrite did.
#   LC_LAZY_LOAD_DYLIB. mo_map_build (src/ordinals.c) refuses any image
#     carrying one, up front, before it looks at what the operations are.
#     fix_macho never built an ordinal map and rewrote such an image happily.
#     compat/rename_segment.sh's header has the measurement (on
#     /usr/lib/libxcselect.dylib) and the note that the smallest fix is a
#     change to machotool, not to a wrapper.
#
# ---- exit codes ----------------------------------------------------------
#
# 0 and 1, the only two fix_macho had -- so every nonzero from machotool is
# mapped to 1. It is the same mapping compat/rename_segment.sh and
# compat/patch_macho.sh make and for the same reason: machotool's own EX_FAIL
# is 2, a value no fix_macho caller has ever seen, and forwarding it would
# invent a third outcome for a grammar that has two. (EX_REFUSED, 1, is not
# the problem -- it already coincides with fix_macho's own flat failure
# code, for any of the ordinary considered refusals this translation's
# `dylib`/`lc`/`segment`/`edit` commands CAN reach -- bad magic, no room to
# grow, and
# the rest of rewrite.h's list, none of which need --fatal-warnings. The
# ONE mr_apply_file refusal genuinely unreachable here is the
# --fatal-warnings-specific one, "an operation matched nothing" promoted
# to MR_REFUSED -- this translation never emits that flag, so that
# particular trigger never fires. It would not have needed mapping either
# way: it is 1, same as everything else this mapping already collapses to
# 1.)
#
# ---- the in-place edit ---------------------------------------------------
#
# fix_macho rewrote the file it was given; `machotool dylib`/`lc`/`segment` do
# not. So this wrapper takes the shared install path -- mw_prepare names a
# temp beside the file FILE really is, mw_retranslate re-emits the command
# with that temp as its output, mw_run_to_tmp runs it and drops the "Wrote
# <temp>" line no C tool ever printed, and mw_finish mv's the temp over the
# target or discards it when the bytes did not change. machotool-compat.sh's "the
# install path" section has the reasoning for each step. `machotool edit`, which
# every invocation worth more than one command becomes, takes that temp as its
# OUT positional like every other verb here, so both shapes install
# identically.
#
# THE WRITABILITY CHECK comes with it, inside mw_prepare. fix_macho opened the
# file O_RDWR before it looked at it, so an absent or unwritable file failed
# immediately, with no analysis and no write. No machotool command reproduces that
# any more -- a verb that writes an output opens FILE O_RDONLY, and `machotool
# edit` finds out it cannot write only when it writes, at the END of the run,
# with a different message -- so mw_require_writable is the only thing that
# does. `test -w` is not open(O_RDWR) -- it consults the real uid and does not
# see ACLs -- so it can disagree at the edges; it agrees on the two cases that
# actually reach a caller (absent, and mode-denied), and both sides exit 1
# either way.
#
# A HARD-LINKED FILE IS NOW REFUSED (exit 1), which is the one behaviour here
# that no version of fix_macho had: it wrote through its own descriptor, so
# every link saw the change, while an install by mv would leave the others on
# the old content. Every wrapper on this install path makes the same trade;
# mw_prepare has the message and the remedy. And creating a temp beside FILE
# needs the DIRECTORY writable, where the C tool needed only FILE itself to be,
# so a writable binary in a read-only directory now fails (`mkstemp:
# Permission denied`, from machotool's own write of the temp) with FILE untouched.

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

mw_translate fix_macho "$@" || exit $?

mw_file=$1

mw_prepare "$mw_file" || exit 1
mw_retranslate fix_macho "$@" || exit 1

mw_run_to_tmp
mw_frc=$?
# Every nonzero becomes 1: see "exit codes" above. Named mw_frc rather than
# reusing mw_rc, which machotool-compat.sh owns.
[ "$mw_frc" -eq 0 ] || exit 1
mw_finish || exit 1
# The line this wrapper has always ended a changed run with, printed by
# mr_apply_file while the verbs still wrote FILE and printed here now -- see
# "the in-place edit" above. Not fix_macho's own "File updated: F", which this
# wrapper has never reproduced (see "STDOUT IS NOT REPRODUCED").
[ "$MW_CHANGED" -eq 1 ] \
    && printf 'Updated %s (%s bytes)\n' "$mw_file" "$(wc -c < "$MW_TARGET" | tr -d ' ')"
exit 0
