#!/bin/sh
# fix_macho -- a /bin/sh wrapper around macho9's `lc`, `dylib` and `segment`.
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
#     -strip_build_version  macho9 lc      FILE -delete build-version
#     -change O N           macho9 dylib   FILE -replace O N
#     -rename_seg O N       macho9 segment FILE O N        (one line per pair)
#
# -- emitted in the order lc, dylib, segment, because deleting a load command
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
# anything runs. The -rename_seg cap in particular exists NOWHERE ELSE: each
# pair becomes its own `macho9 segment` invocation, so macho9 sees one rename
# at a time and has no cap of its own to hit.
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
#      file untouched). `macho9 dylib -replace` rebuilds the load-command
#      table and fits the longer path into existing header pad, exit 0.
#      WHY ADOPTING IT IS RIGHT: the limit was an artifact of a rewriter that
#      never learned to resize a command, not a safety property. Nothing in
#      docs/PROPOSAL.md records a reason for it, and the previous plan states
#      the rule outright -- "macho9 does NOT have to inherit the old tools'
#      artificial limits". Note the translation still emits no --allow-grow:
#      this uses pad the image already has, and does not enlarge the header.
#
#   2. A CHAINED -rename_seg now CHAINS. `-rename_seg __DATA __X -rename_seg
#      __X __Y` produced __X under fix_macho, which applied every pair in ONE
#      pass and gave each segment its FIRST match, so the second pair never
#      fired. Each pair is its own `macho9 segment` pass here, and the second
#      reads the first's output, so it produces __Y.
#      WHY ADOPTING IT IS RIGHT: doing what was asked. compat/translate.sh
#      REFUSED this shape outright until now -- correctly, while a wrapper had
#      to preserve fix_macho's answer -- and its -rename_seg arm records why
#      that refusal existed and what reversed it.
#
#   3. THE WRITE-BACK IS ATOMIC. fix_macho lseek'd to 0 and wrote the whole
#      file back over itself, so a crash, a full disk or a kill mid-write left
#      a corrupt binary. macho9 writes through wa_write_atomic
#      (src/atomic_write.h): mkstemp + rename, following a symlink to its
#      target, preserving xattrs, and writing in place when st_nlink > 1.
#      WHY ADOPTING IT IS RIGHT: these tools exist to make binaries loadable;
#      a half-written one is the failure they are supposed to prevent.
#      ONE CAVEAT, and it is this wrapper's own: an invocation that touches
#      MORE THAN ONE family is a SEQUENCE of macho9 commands, and mw_run_atomic
#      (compat/macho9-compat.sh) covers that with a copy-aside-and-install
#      dance whose final `cat COPY > FILE` is NOT itself atomic. Its header
#      says exactly what that does and does not preserve. A single-command
#      invocation -- which is every invocation touching one family -- gets
#      wa_write_atomic directly.
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
#          new:  macho9: /tmp/aaa/libfoo.dylib matched nothing
#                b.dylib: nothing to change.    rc=0   otool -D -> /tmp/aaa/libfoo.dylib
#          cmp a.dylib b.dylib -> differ
#
#      BOTH SIDES EXIT 0 AND THE BYTES DIFFER, and nothing on stderr named
#      the reason -- which is exactly the invisible edit this whole plan
#      exists to make visible.
#      WHY ADOPTING IT IS RIGHT: (a) install_name_tool spells identity `-id`
#      and its `-change` never touches LC_ID_DYLIB -- macho9 matches the
#      tool everyone already knows; (b) fix_macho.c's own comment stated the
#      contract macho9 now enforces, so this is the C being fixed, not the C
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
# a caller sees now is macho9's own reporting, plus -- for the one line that
# carried information a caller could act on -- the unmatched report this
# plan's Task 1 added:
#
#     macho9: /usr/lib/libFoo.dylib matched nothing        (a -change)
#     macho9: no load command of kind build-version to delete
#
# on STDERR, per operation, naming the operation that matched nothing. That is
# strictly more than "No changes needed: F" said, which could not distinguish
# which of several operations missed. `macho9 --fatal-warnings` would turn that
# report into a refusal; THIS WRAPPER MUST NOT PASS IT. fix_macho exited 0 when
# an operation matched nothing, that is compat surface, and
# tests/known-callers.sh and tests/wrapper_test.sh are the gates.
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
#     change to macho9, not to a wrapper.
#
# ---- exit codes ----------------------------------------------------------
#
# 0 and 1, the only two fix_macho had -- so every nonzero from macho9 is
# mapped to 1. It is the same mapping compat/rename_segment.sh and
# compat/patch_macho.sh make and for the same reason: macho9's own EX_REFUSED
# is 2, a value no fix_macho caller has ever seen, and forwarding it would
# invent a third outcome for a grammar that has two. (No line this translation
# emits can reach mr_apply_file's own MR_REFUSED anyway, because that needs
# --fatal-warnings and this never emits it.)
#
# ---- the writability check -----------------------------------------------
#
# fix_macho opened the file O_RDWR before it looked at it, so an absent or
# unwritable file failed immediately, with no analysis and no write.
# mr_apply_file opens O_RDWR up front too and perror()s "open" identically --
# so a SINGLE-command invocation needs nothing here. A MULTI-command one does:
# mw_run_atomic copies the file aside first and runs macho9 against the COPY,
# which is writable by construction, so an unwritable original would get all
# the way to the install step before failing, with a different message. The
# check below keeps both shapes failing where fix_macho did. `test -w` is not
# open(O_RDWR) -- it consults the real uid and does not see ACLs -- so it can
# disagree at the edges; it agrees on the two cases that actually reach a
# caller (absent, and mode-denied), and both sides exit 1 either way.

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

mw_translate fix_macho "$@" || exit $?

mw_file=$1

mw_require_writable "$mw_file" || exit $?

mw_run_atomic fix_macho "$@"
mw_frc=$?
# Every nonzero becomes 1: see "exit codes" above. Named mw_frc rather than
# reusing mw_rc, which macho9-compat.sh owns.
[ "$mw_frc" -eq 0 ] || exit 1
exit 0
