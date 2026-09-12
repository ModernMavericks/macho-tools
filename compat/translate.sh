#!/bin/sh
# compat/translate.sh -- turn an OLD-grammar invocation into the macho9
# command line(s) it is equivalent to, and PRINT them. Nothing here opens,
# reads, executes or rewrites anything: this file is a pure function from one
# argv to a list of command lines.
#
#   sh compat/translate.sh TOOL ARG...        print the equivalent, exit 0
#   MT_SOURCED=1 . compat/translate.sh        load mt_translate() and helpers
#
# TOOL is one of the six historical binaries: change_dylib, add_version_min,
# patch_macho, rename_segment, retag_swift_classes, fix_macho. ARG... is that
# tool's own argv[1..], verbatim.
#
# WHY THIS IS NOT A RENAME. docs/PROPOSAL.md rejected `-add_rpath`, bare
# `-rpath`, `-change` and `-add` as macho9 synonyms on purpose, because they
# "would advertise an interchangeability that does not exist, on exactly the
# binaries where it does not hold." So every line this file emits is a CLAIM
# that two spellings mean the same thing, and tests/translate_test.sh asserts
# each claim's exact text. It lives here, in shell, and NOT inside macho9:
# macho9 must not learn the grammar it deliberately refused (controller ruling
# L). Task 2's wrappers source this file, so the translation that was tested is
# literally the translation that ships.
#
# ---- output contract -----------------------------------------------------
#
#   * Zero or more COMMANDS on stdout, as `eval`-safe shell text. A command
#     begins with $MACHO9 (default: the bare word `macho9`) at the start of a
#     line. Arguments are single-quoted only when they contain something
#     outside [A-Za-z0-9_@%+=:,./-], so the common case stays readable and the
#     hostile case stays correct.
#   * A command is USUALLY one line, but `macho9 edit FILE -` carries its
#     statements in a here-document, so it spans several: the `edit` line, the
#     statements, and the `MACHO9_EDIT` terminator. Counting commands means
#     counting lines that START with the program word, not counting lines --
#     which is what macho9-compat.sh's mw_translate does.
#   * Every tool here but retag_swift_classes emits AT MOST ONE command: an
#     invocation that would have needed more is one `macho9 edit FILE -`
#     instead. So there is no sequence to run and nothing to stop part way
#     through -- macho9-compat.sh's mw_run evaluates what is printed and
#     returns its exit code. (retag_swift_classes is variadic over FILES and
#     emits one `retag-swift` per file; its wrapper runs those itself, because
#     it needs each file's own exit code and stdout.) The ORDER still matters
#     within the one command -- see "ordering" below.
#   * Exit 0 with ZERO lines means "this old invocation was a no-op on the
#     file; there is no macho9 command to run." (change_dylib accepts
#     `-grow` with no operations; it prints its header-pad line and changes
#     nothing.) It is NOT an error, and it is deliberately not translated as
#     some adjacent command that would do something.
#   * Exit 2 means NO EQUIVALENT: this argv is one the old tool accepted but
#     that no macho9 command line means the same thing as. Today there is
#     exactly one -- an unknown TOOL name. (There were two: fix_macho's
#     chained -rename_seg was the other, until the ruling recorded at
#     mt_tr_fix_macho's -rename_seg arm made chaining a behaviour to ADOPT
#     rather than to preserve.) Nothing goes to stdout; emitting a
#     plausible-looking command that would do something else is exactly what
#     the plan forbids.
#   * Exit 1 means REFUSED, with a message on stderr and nothing on stdout, so
#     a wrapper can refuse by just forwarding this exit code and never runs a
#     half-translated command. Almost every case is one the OLD TOOL ITSELF
#     would have refused -- a usage error, an unknown flag, an unknown
#     -strip-lc kind, a capacity cap, an over-long segment name -- and carries
#     the origin tool's exact message. There is ONE refusal of this file's
#     own, which the old tools accepted: a -change chain on the edit-script
#     path, refused by mt_chain_check, whose comment has the reasoning. It
#     exits 1 rather than 2 because 1 is the only failure code change_dylib
#     and fix_macho ever had, and 2 would invent a third outcome for a
#     two-outcome grammar. Both wrappers forward a translation's code raw --
#     fix_macho.sh's fold of every nonzero to 1 applies to mw_run's code, not
#     to this one -- so whatever this returns is what the caller sees.
#
# The one exception to "the old tool's message" is $MT_PROG, which stands in
# for argv[0] in a usage line; mt_translate defaults it to the tool's own name.
#
# CAPACITY CAPS ARE THIS FILE'S JOB, not macho9's. cli/macho9.c caps at exactly
# the same numbers (MR_MAX_OPS=32 shared by -change/-delete/-reexport, and
# separately by -add, -insert, -add-rpath, and -change-rpath/-delete-rpath;
# MR_MAX_STRIP=16 for -strip-lc) but prints DIFFERENT text on purpose, so that
# the new grammar never leaks the old flag spellings -- both cap sites in
# cli/macho9.c carry a comment saying so and telling whoever writes the wrapper
# to enforce the caps here and print the origin text. That is what mt_room does.
#
# fix_macho's two caps come here for a second reason as well: its -rename_seg
# array has NO macho9 counterpart at all (macho9 sees one rename at a time --
# its own `segment` verb takes one pair, and an edit script's `segment rename`
# statement is one pair too -- so nothing downstream counts them), and its
# `changes[32]` / `renames[16]` were the same unbounded fixed-size arrays
# docs/PROPOSAL.md records smashing the stack in change_dylib -- "Repeated
# options wrote past their fixed-size arrays; 33 -change flags smashed the
# stack -- fixed, PR #9",
# a fix that only ever covered change_dylib. compat/fix_macho.c grew a bounds
# check of its own before it was retired; mt_room below is where that check
# lives now, in fix_macho's own words, so retiring the C file did not take the
# refusal with it.
#
# ---- the grammar mapping -------------------------------------------------
#
# Each row gives the VERB form, which is what an invocation needing only that
# row's family emits. An invocation needing more than one row's worth emits
# one `macho9 edit FILE -` instead, whose statements are in the third column.
#
#   change_dylib FILE ...            macho9 ...                  statement
#     -change O N                      dylib FILE -replace O N     dylib replace O N
#     -delete P                        dylib FILE -delete P        dylib delete P
#     -reexport P                      dylib FILE -reexport P      dylib reexport P
#     -add P                           dylib FILE -append P        dylib append P
#     -insert P                        dylib FILE -insert P        dylib insert P
#     -change-rpath O N                rpath FILE -replace O N     rpath replace O N
#     -delete-rpath P                  rpath FILE -delete P        rpath delete P
#     -add-rpath P                     rpath FILE -append P        rpath append P
#     -strip-lc KIND                   lc    FILE -delete KIND     load-command delete KIND
#     -grow                            --allow-grow on the         allow-grow
#                                      dylib/rpath lines
#
#   fix_macho FILE ...
#     -change O N                      dylib   FILE -replace O N   dylib replace O N
#     -strip_build_version             lc      FILE -delete build-version
#                                                                  load-command delete build-version
#     -rename_seg O N                  segment FILE O N            segment rename O N
#
#   add_version_min FILE               minos      FILE 10.9
#   patch_macho IN OUT                 declassify IN OUT
#   rename_segment FILE O N            segment    FILE O N
#   retag_swift_classes F1 F2 F3       retag-swift F1
#                                      retag-swift F2
#                                      retag-swift F3
#
# ---- ordering, and the one command an old invocation becomes -------------
#
# change_dylib and fix_macho each apply EVERY operation in ONE pass over the
# load-command table, and write ONCE. macho9 has a verb per family, so an
# invocation touching more than one family has no single verb to become. It
# becomes ONE `macho9 edit FILE -` instead, with the operations as statements
# on stdin -- which is again one read, one pass per statement over an image
# held in memory, and one write. The order emitted is:
#
#   1. load-command delete  -- deleting load commands SHRINKS the table and
#                  hands header pad back. Anything that needs room must run
#                  after it.
#   2. dylib    -- library ordinals are the delicate part (a wrong ordinal is
#                  a dyld failure, not a silent mis-bind), so they are settled
#                  while the image is closest to the one the old tool saw.
#   3. rpath    -- LC_RPATH carries no ordinal, so it is the safest to move
#                  last. Between 2 and 3 the order is a free choice; it is
#                  fixed here so the emitted text is deterministic.
#   4. segment  -- fix_macho only. A rename changes no sizes, so it cannot
#                  compete for header pad with anything above.
#
# An invocation that needs only ONE of those still gets that family's own
# verb, whose flags are applied as a BATCH: there is nothing for a script to
# sequence, and the verb line is the one a human would type.
#
# WITHIN a family, a batch and a sequence are not automatically the same
# thing, and mt_tr_change_dylib's emission comment says exactly which order
# makes them agree and which single shape it refuses because no order can.
#
# ---- divergences a wrapper must still handle itself ----------------------
#
# These are behavioural, not expressibility: the translation exists, but the
# emitted commands do not by themselves reproduce the old observable. Each is
# documented at its site in cli/macho9.c, and each is a row in
# tests/compat-matrix.tsv.
#
#   rename_segment exits 2 when nothing matched; `macho9 segment` exits 0
#     (mr_apply_file's "nothing to change."). The emitted line CANNOT carry
#     that distinction -- reproducing it needs an out-of-band check, which is
#     Task 2's decision, not this file's.
#   macho9 segment prints header-pad chatter that rename_segment has neither
#     of, handles a fat container that rename_segment refused outright, and
#     -- like every mr_apply_file caller -- refuses a binary carrying
#     LC_LAZY_LOAD_DYLIB that rename_segment, which never built an ordinal
#     map, renamed without complaint. It does NOT additionally run
#     mg_plausible: src/rewrite.c skips that gate for a rename-only operation
#     set, so it is no longer one of this verb's divergences from
#     rename_segment.
#   macho9 retag-swift refuses (exit 1) a non-Mach-O argument that
#     retag_swift_classes skipped silently, and exits 2 on the raced path
#     where retag_swift_classes exited 0.
#   macho9 declassify uses exit 2 (EX_FAIL) for an operational failure where
#     patch_macho returns its same flat 1 -- a considered refusal, unlike
#     that case, now exits 1 on both sides, by coincidence, not construction
#     -- writes atomically, and names the file it wrote even on the
#     pass-through.
#   fix_macho is the one tool whose divergences are NOT closed by its wrapper,
#     because the repo owner ruled them improvements to ADOPT: a longer
#     replacement path is now rewritten using header pad instead of refused, a
#     chained -rename_seg now chains, the write-back is atomic, a fat slice
#     macho9 cannot handle refuses the whole file instead of being skipped,
#     and a -change aimed at the dylib's own install name now matches nothing
#     instead of rewriting LC_ID_DYLIB. compat/fix_macho.sh's header states
#     all five as deliberate changes, with their reasons; this file simply
#     translates, as it does for every other tool.
#
# This file refuses exactly one shape of its own: a -change chain on the
# edit-script path (mt_chain_check, whose comment has the reasoning). There
# used to be a second -- `fix_macho -rename_seg A B -rename_seg B C` -- and
# mt_tr_fix_macho's -rename_seg arm records why it existed and what reversed
# it. The two are not the same question: a rename statement and a `macho9
# segment` pass chain identically, which is the behaviour that was adopted,
# while a -change chain is a shape the OLD tool's single batch applied and no
# sequence reproduces.

# ---- quoting -------------------------------------------------------------
#
# One argument, quoted only if it needs it. The safe set is the usual
# shell-quoting one; note that a path containing a NEWLINE cannot survive the
# command substitution mt_qargs is used through, and nothing in these tools'
# grammar has ever accepted one.
#
# THE SAME QUOTING SERVES THE EDIT SCRIPT, because ms_split (src/script.c)
# reads a statement's words by the same rules a shell does: whitespace
# separates, `'...'` is literal, `"..."` and `\` escape, and an unquoted `#`
# begins a comment. Every one of those characters is outside the safe set
# above, so anything that could be read as syntax is already inside single
# quotes by the time the parser sees it. The one shape the two disagree on is
# a path carrying a control byte other than tab: a shell passes it through and
# ms_parse refuses the line ("control character"). No tool in this grammar has
# ever been handed one.
mt_quote() {
    case $1 in
        '') printf "''" ;;
        *[!A-Za-z0-9_@%+=:,./-]*)
            printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")" ;;
        *) printf '%s' "$1" ;;
    esac
}

# Every argument, quoted, each preceded by one space -- so a caller can append
# the result to a partially built command line without tracking separators.
mt_qargs() {
    for mt_a in "$@"; do
        printf ' '
        mt_quote "$mt_a"
    done
}

# The origin tool's own diagnostic, on stderr, and a nonzero return.
mt_die() {
    printf '%s\n' "$1" >&2
    return 1
}

# change_dylib's CD_ROOM: refuse BEFORE the write that would overflow, naming
# the flag that overflowed, in change_dylib's exact words.
#   mt_room <current count> <max> <flag spelling>
mt_room() {
    [ "$1" -eq "$2" ] || return 0
    printf 'too many %s (max %d)\n' "$3" "$2" >&2
    return 1
}

# THE ONE SHAPE A SEQUENCE CANNOT REPRODUCE.
#
#   mt_chain_check <flag spelling> <pairs>
#
# <pairs> is every replacement this family collected, OLD and NEW on
# alternating lines, in flag order. Refuses when one pair's NEW is another
# pair's OLD -- a chain (`-change a b -change b c`) or a swap (`-change a b
# -change b a`).
#
# WHY. The old tools apply a whole family's operations as ONE batch against
# the original image: every command is compared against the paths the image
# had, so `-change a b -change b c` rewrites the a's to b and the ORIGINAL b's
# to c, and the b's the first pair just produced are not revisited. An edit
# script applies its statements in sequence, each seeing what the one before
# left, so `dylib replace a b` followed by `dylib replace b c` rewrites the
# new b's as well and lands on c. No emission order fixes it: reversing the
# two just moves which pair is over-applied.
#
# So this is the one place the translation has no equivalent to emit, and it
# says so rather than emitting something adjacent. It is checked ONLY on the
# edit-script path: a single-family invocation still uses the verb, whose
# batch is the old tool's own semantics.
mt_chain_check() {
    mt_cc_i=0
    while IFS= read -r mt_cc_o1 && IFS= read -r mt_cc_n1; do
        mt_cc_i=$((mt_cc_i + 1))
        mt_cc_j=0
        while IFS= read -r mt_cc_o2 && IFS= read -r mt_cc_n2; do
            mt_cc_j=$((mt_cc_j + 1))
            [ "$mt_cc_i" -ne "$mt_cc_j" ] || continue
            [ "$mt_cc_n1" = "$mt_cc_o2" ] || continue
            mt_die "$1 $mt_cc_o1 $mt_cc_n1 and $1 $mt_cc_n1 $mt_cc_n2 chain: run them as separate invocations"
            return 1
        done <<MT_CHAIN_INNER
$2
MT_CHAIN_INNER
    done <<MT_CHAIN_OUTER
$2
MT_CHAIN_OUTER
    return 0
}

# Caps, from src/rewrite.h. -change/-delete/-reexport SHARE one array of 32,
# as do -change-rpath/-delete-rpath; -add, -insert and -add-rpath each get
# their own 32; -strip-lc gets 16.
MT_MAX_OPS=32
MT_MAX_STRIP=16
# fix_macho's own second array, from compat/fix_macho.c's FM_MAX_RENAMES. No
# shared header has an opinion about segment renames -- macho9 never sees more
# than one at a time -- so this 16 is fix_macho's alone and lives here.
MT_MAX_RENAMES=16

# change_dylib's -strip-lc vocabulary, from src/lc_kinds.c. This is the OLD
# tool's table, frozen: it is what change_dylib accepted, and reproducing its
# refusal is the point. What THIS build of macho9 accepts is a separate
# question, answered by `macho9 --capabilities` (kinds=...) -- and
# tests/translate_test.sh asserts the two agree rather than assuming it.
MT_STRIP_KINDS='uuid codesig source-version build-version code-sign-drs'

# What a reproduced usage line prints where the C tool printed argv[0].
# mt_translate resets this to the tool's own name on every call; the default
# here only matters if a caller reaches one of the mt_tr_* functions directly,
# and exists so `set -u` cannot turn that into a shell error instead of a
# usage line.
MT_PROG=${MT_PROG:-translate.sh}

# ---- change_dylib --------------------------------------------------------
mt_cd_usage() {
    printf 'Usage: %s input [-grow] [-change old new] [-delete path] [-reexport path] [-add path] [-insert path] [-strip-lc name] [-change-rpath old new] [-delete-rpath path] [-add-rpath path] ...\n' "$MT_PROG" >&2
    printf '  -strip-lc kinds:' >&2
    for mt_k in $MT_STRIP_KINDS; do printf ' %s' "$mt_k" >&2; done
    printf '\n' >&2
    return 1
}

mt_tr_change_dylib() {
    # `argc < 4` in change_dylib.c -- program name plus fewer than three
    # arguments. So `change_dylib FILE -grow` is a USAGE ERROR even though the
    # usage text presents -grow as an optional standalone flag.
    [ $# -ge 3 ] || { mt_cd_usage; return 1; }

    mt_file=$1; shift
    mt_lc='' mt_dy='' mt_rp=''
    # The same operations a second time, as edit-script statements, bucketed
    # by kind so the emission can order them (see the emission comment below).
    # Collected unconditionally: which of the two forms gets printed is not
    # known until the argv has been walked.
    mt_st_lc='' mt_st_dydel='' mt_st_dyrepl='' mt_st_dyapp='' mt_st_dyins=''
    mt_st_rpdel='' mt_st_rprepl='' mt_st_rpapp=''
    # Every replacement's OLD and NEW, on alternating lines, for mt_chain_check.
    mt_pairs_dy='' mt_pairs_rp=''
    mt_nchanges=0 mt_nadds=0 mt_ninserts=0
    mt_nrchanges=0 mt_nradds=0 mt_nstrip=0
    mt_grow=''

    while [ $# -gt 0 ]; do
        case $1 in
        -grow)
            mt_grow=' --allow-grow'; shift ;;
        -strip-lc)
            [ $# -ge 2 ] || { mt_die "bad arg: $1"; return 1; }
            mt_found=0
            for mt_k in $MT_STRIP_KINDS; do
                [ "$2" = "$mt_k" ] && { mt_found=1; break; }
            done
            [ "$mt_found" -eq 1 ] || { mt_die "unknown -strip-lc kind: $2"; return 1; }
            mt_room "$mt_nstrip" "$MT_MAX_STRIP" -strip-lc || return 1
            mt_nstrip=$((mt_nstrip + 1))
            mt_lc="$mt_lc$(mt_qargs -delete "$2")"
            mt_st_lc="$mt_st_lc$(printf 'load-command delete%s' "$(mt_qargs "$2")")
"
            shift 2 ;;
        -add)
            [ $# -ge 2 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nadds" "$MT_MAX_OPS" -add || return 1
            mt_nadds=$((mt_nadds + 1))
            mt_dy="$mt_dy$(mt_qargs -append "$2")"
            mt_st_dyapp="$mt_st_dyapp$(printf 'dylib append%s' "$(mt_qargs "$2")")
"
            shift 2 ;;
        -insert)
            [ $# -ge 2 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_ninserts" "$MT_MAX_OPS" -insert || return 1
            mt_ninserts=$((mt_ninserts + 1))
            mt_dy="$mt_dy$(mt_qargs -insert "$2")"
            # PREPENDED, not appended: see the emission comment below. Every
            # insert goes to the front of the table, so a sequence has to run
            # them backwards to leave them in the order the batch would.
            mt_st_dyins="$(printf 'dylib insert%s' "$(mt_qargs "$2")")
$mt_st_dyins"
            shift 2 ;;
        -change)
            [ $# -ge 3 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nchanges" "$MT_MAX_OPS" -change || return 1
            mt_nchanges=$((mt_nchanges + 1))
            mt_dy="$mt_dy$(mt_qargs -replace "$2" "$3")"
            mt_st_dyrepl="$mt_st_dyrepl$(printf 'dylib replace%s' "$(mt_qargs "$2" "$3")")
"
            mt_pairs_dy="$mt_pairs_dy$2
$3
"
            shift 3 ;;
        -delete)
            [ $# -ge 2 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nchanges" "$MT_MAX_OPS" -delete || return 1
            mt_nchanges=$((mt_nchanges + 1))
            mt_dy="$mt_dy$(mt_qargs -delete "$2")"
            mt_st_dydel="$mt_st_dydel$(printf 'dylib delete%s' "$(mt_qargs "$2")")
"
            shift 2 ;;
        -reexport)
            [ $# -ge 2 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nchanges" "$MT_MAX_OPS" -reexport || return 1
            mt_nchanges=$((mt_nchanges + 1))
            mt_dy="$mt_dy$(mt_qargs -reexport "$2")"
            mt_st_dydel="$mt_st_dydel$(printf 'dylib reexport%s' "$(mt_qargs "$2")")
"
            shift 2 ;;
        -add-rpath)
            [ $# -ge 2 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nradds" "$MT_MAX_OPS" -add-rpath || return 1
            mt_nradds=$((mt_nradds + 1))
            mt_rp="$mt_rp$(mt_qargs -append "$2")"
            mt_st_rpapp="$mt_st_rpapp$(printf 'rpath append%s' "$(mt_qargs "$2")")
"
            shift 2 ;;
        -change-rpath)
            [ $# -ge 3 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nrchanges" "$MT_MAX_OPS" -change-rpath || return 1
            mt_nrchanges=$((mt_nrchanges + 1))
            mt_rp="$mt_rp$(mt_qargs -replace "$2" "$3")"
            mt_st_rprepl="$mt_st_rprepl$(printf 'rpath replace%s' "$(mt_qargs "$2" "$3")")
"
            mt_pairs_rp="$mt_pairs_rp$2
$3
"
            shift 3 ;;
        -delete-rpath)
            [ $# -ge 2 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nrchanges" "$MT_MAX_OPS" -delete-rpath || return 1
            mt_nrchanges=$((mt_nrchanges + 1))
            mt_rp="$mt_rp$(mt_qargs -delete "$2")"
            mt_st_rpdel="$mt_st_rpdel$(printf 'rpath delete%s' "$(mt_qargs "$2")")
"
            shift 2 ;;
        *)
            mt_die "bad arg: $1"; return 1 ;;
        esac
    done

    # ONE FAMILY: that family's verb, whose flags macho9 applies as a BATCH --
    # the old tool's own semantics, and the line a human would type.
    #
    # -grow is deliberately dropped from the `lc` line: `macho9 lc` has no
    # --allow-grow, and it does not need one -- deleting load commands only
    # ever shrinks the table, so the growth path change_dylib's allow_grow
    # unlocks is unreachable from a strip-only pass.
    mt_pre="$(mt_pre_word)"
    mt_nfam=0
    [ -n "$mt_lc" ] && mt_nfam=$((mt_nfam + 1))
    [ -n "$mt_dy" ] && mt_nfam=$((mt_nfam + 1))
    [ -n "$mt_rp" ] && mt_nfam=$((mt_nfam + 1))
    if [ "$mt_nfam" -le 1 ]; then
        [ -n "$mt_lc" ] && printf '%s lc%s%s\n' "$mt_pre" "$(mt_qargs "$mt_file")" "$mt_lc"
        [ -n "$mt_dy" ] && printf '%s dylib%s%s%s\n' "$mt_pre" "$(mt_qargs "$mt_file")" "$mt_grow" "$mt_dy"
        [ -n "$mt_rp" ] && printf '%s rpath%s%s%s\n' "$mt_pre" "$(mt_qargs "$mt_file")" "$mt_grow" "$mt_rp"
        return 0
    fi

    # MORE THAN ONE FAMILY: one `macho9 edit FILE -`, statements on stdin.
    #
    # WHY THIS ORDER. A verb applies all of one family's operations as a batch
    # against the ORIGINAL image; an edit script applies statements in
    # SEQUENCE, each seeing what the one before left. They agree when each
    # family's statements are emitted like this, and that is what the buckets
    # above are for:
    #
    #   1. every `load-command delete` first, as the verb sequence did it,
    #      because deleting commands hands header pad back and everything
    #      else may need the room;
    #   2. per family (dylib, then rpath): every delete and reexport, then
    #      every replace, then every append, then every insert IN REVERSE
    #      FLAG ORDER. Deletes go first because they renumber; appends and
    #      inserts add nothing for a replace to find. The reversal is the
    #      one that is not obvious: each insert goes to the FRONT, so as a
    #      batch `-insert A -insert B` leaves A at ordinal 1 and B at 2,
    #      and a sequence reproduces that only by inserting B and then A.
    #
    # And one shape no order reproduces -- a -change whose NEW is another
    # -change's OLD -- which mt_chain_check refuses here, and only here.
    mt_chain_check -change "$mt_pairs_dy" || return 1
    mt_chain_check -change-rpath "$mt_pairs_rp" || return 1
    printf '%s edit%s - <<'"'"'MACHO9_EDIT'"'"'\n' "$mt_pre" "$(mt_qargs "$mt_file")"
    [ -n "$mt_grow" ] && printf 'allow-grow\n'
    printf '%s%s%s%s%s%s%s%s' "$mt_st_lc" "$mt_st_dydel" "$mt_st_dyrepl" "$mt_st_dyapp" \
        "$mt_st_dyins" "$mt_st_rpdel" "$mt_st_rprepl" "$mt_st_rpapp"
    printf 'MACHO9_EDIT\n'
    return 0
}

# ---- fix_macho -----------------------------------------------------------
mt_fm_usage() {
    printf 'Usage: %s <file> [-change old new] [-strip_build_version]\n' "$MT_PROG" >&2
    return 1
}

mt_tr_fix_macho() {
    # `argc < 3`: program name plus fewer than two arguments.
    [ $# -ge 2 ] || { mt_fm_usage; return 1; }

    mt_file=$1; shift
    mt_lc='' mt_dy='' mt_seg=''
    # The same operations as edit-script statements, and every -change's OLD
    # and NEW on alternating lines for mt_chain_check -- see
    # mt_tr_change_dylib, which collects both for the same reason.
    mt_st_lc='' mt_st_dyrepl='' mt_st_seg='' mt_pairs_dy=''
    mt_nchanges=0 mt_nrenames=0

    while [ $# -gt 0 ]; do
        case $1 in
        -change)
            # fix_macho's own condition is `i + 2 < argc`, so a trailing
            # `-change OLD` with no NEW falls through to "Unknown option:
            # -change" -- naming the flag, not the missing operand.
            [ $# -ge 3 ] || { mt_die "Unknown option: $1"; return 1; }
            # fix_macho's FM_MAX_CHANGES was change_dylib's own MR_MAX_OPS, and
            # its FM_ROOM printed change_dylib's exact "too many %s (max %d)" --
            # so mt_room's text is already fix_macho's text, with fix_macho's
            # flag spelling in it.
            mt_room "$mt_nchanges" "$MT_MAX_OPS" -change || return 1
            mt_nchanges=$((mt_nchanges + 1))
            mt_dy="$mt_dy$(mt_qargs -replace "$2" "$3")"
            mt_st_dyrepl="$mt_st_dyrepl$(printf 'dylib replace%s' "$(mt_qargs "$2" "$3")")
"
            mt_pairs_dy="$mt_pairs_dy$2
$3
"
            shift 3 ;;
        -strip_build_version)
            # Only ever one LC kind, and fix_macho's flag is a boolean, so a
            # repeat adds nothing. Emitting `-delete build-version` twice
            # would be a different command for the same intent -- so both
            # forms below ASSIGN rather than append.
            mt_lc="$(mt_qargs -delete build-version)"
            mt_st_lc='load-command delete build-version
'
            shift ;;
        -rename_seg)
            [ $# -ge 3 ] || { mt_die "Unknown option: $1"; return 1; }
            # Same 16-byte segname limit fix_macho checks here, before any
            # I/O, in its own words (which differ from rename_segment's).
            # `${#3}` counts CHARACTERS, not bytes -- fix_macho's strlen()
            # counted bytes. A 16-character multibyte NEW segname whose
            # encoding runs longer than 16 bytes passes this check where
            # fix_macho refused it. Both paths still end at fix_macho.sh
            # exiting 1: macho9's mseg_name_fits (src/segname.c), called from
            # cmd_segment (cli/macho9.c), catches the over-length name
            # downstream and returns EX_REFUSED, which fix_macho.sh maps to 1
            # like every other nonzero macho9 exit -- with different text
            # than either shell message above. No code change follows from
            # this -- the observable exit code is the same either way, only
            # the wording differs earlier.
            [ "${#3}" -le 16 ] || { mt_die "new segment name longer than 16 bytes: $3"; return 1; }
            # fix_macho's renames[] held 16, and its FM_ROOM refused the 17th
            # in these same words. Nothing downstream counts these -- each
            # pair becomes its OWN `macho9 segment` invocation, so macho9 sees
            # one rename at a time and has no cap of its own to hit. Enforcing
            # it here is the only thing keeping that refusal alive.
            mt_room "$mt_nrenames" "$MT_MAX_RENAMES" -rename_seg || return 1
            mt_nrenames=$((mt_nrenames + 1))
            # A CHAINED RENAME (`-rename_seg A B -rename_seg B C`) USED TO
            # REFUSE HERE, with exit 2, via a helper called mt_fm_chain. That
            # refusal was DELIBERATE, not an oversight, and it was right at the
            # time: while compat/fix_macho.c still shipped, a wrapper had to
            # PRESERVE its behaviour, and the two answers differ -- fix_macho
            # applies every pair in ONE pass and gives each segment its FIRST
            # match, so the second pair never fires and it produces B, while
            # separate `macho9 segment` passes chain and produce C. Measured on
            # tests/fixture.macho with the real binaries: different bytes, both
            # exiting 0. Emitting the sequence anyway would have been exactly
            # the "plausible-looking command that would do something else" the
            # retirement plan forbids, so it refused instead.
            #
            # WHAT REVERSED IT: the repo owner's ruling, recorded in
            # docs/superpowers/plans/2026-09-10-report-what-macho9-did.md ("The
            # decision this plan rests on"), that fix_macho's divergences from
            # the shared drivers are improvements to ADOPT deliberately rather
            # than behaviour to preserve -- chaining is listed there as "doing
            # what was asked". compat/fix_macho.c is gone; there is no longer a
            # behaviour on the other side to preserve, so refusing a shape the
            # surviving implementation handles correctly would be the wrong
            # answer. compat/fix_macho.sh's header states the change as one of
            # its five deliberate divergences.
            mt_seg="$mt_seg$(mt_qargs "$2" "$3")
"
            mt_st_seg="$mt_st_seg$(printf 'segment rename%s' "$(mt_qargs "$2" "$3")")
"
            shift 3 ;;
        *)
            mt_die "Unknown option: $1"; return 1 ;;
        esac
    done

    mt_pre="$(mt_pre_word)"
    mt_fq="$(mt_qargs "$mt_file")"
    # No allow-grow anywhere, in either form: fix_macho had no -grow and never
    # enlarged a header, so nothing in its grammar can ask for one. `macho9
    # dylib` without --allow-grow still resizes a command into EXISTING header
    # pad, which fix_macho refused ("new path ... too long") -- the first of
    # the five adopted changes listed in compat/fix_macho.sh's header. Growing
    # the header outright is a further step, and this translation still does
    # not take it.
    #
    # `macho9 segment` takes ONE pair, so unlike change_dylib's three families
    # this tool's command count is not its family count: two renames are two
    # commands on their own. Anything that is more than one command becomes
    # one edit script, by the same rule and for the same reason.
    mt_ncmds=$mt_nrenames
    [ -n "$mt_lc" ] && mt_ncmds=$((mt_ncmds + 1))
    [ -n "$mt_dy" ] && mt_ncmds=$((mt_ncmds + 1))
    if [ "$mt_ncmds" -le 1 ]; then
        [ -n "$mt_lc" ] && printf '%s lc%s%s\n' "$mt_pre" "$mt_fq" "$mt_lc"
        [ -n "$mt_dy" ] && printf '%s dylib%s%s\n' "$mt_pre" "$mt_fq" "$mt_dy"
        if [ -n "$mt_seg" ]; then
            printf '%s' "$mt_seg" | while IFS= read -r mt_line; do
                [ -n "$mt_line" ] || continue
                printf '%s segment%s%s\n' "$mt_pre" "$mt_fq" "$mt_line"
            done
        fi
        return 0
    fi

    # lc, then dylib, then segment -- mt_tr_change_dylib's emission comment has
    # the reasoning for the first two, and a rename goes last because it
    # changes no sizes and so competes for header pad with nothing. Renames
    # stay in FLAG ORDER: unlike an insert, a rename statement and a `macho9
    # segment` pass are the same single operation, so a sequence of them is
    # already what the -rename_seg arm above says this tool now does.
    mt_chain_check -change "$mt_pairs_dy" || return 1
    printf '%s edit%s - <<'"'"'MACHO9_EDIT'"'"'\n' "$mt_pre" "$mt_fq"
    printf '%s%s%s' "$mt_st_lc" "$mt_st_dyrepl" "$mt_st_seg"
    printf 'MACHO9_EDIT\n'
    return 0
}

# ---- the four fixed-arity tools -----------------------------------------
mt_tr_add_version_min() {
    # `argc != 2`. The 10.9 floor is hardcoded in add_version_min itself
    # (mv_add_version_min), which is why the version appears here and not in
    # the old argv.
    [ $# -eq 1 ] || { printf 'Usage: %s binary\n' "$MT_PROG" >&2; return 1; }
    printf '%s minos%s 10.9\n' "$(mt_pre_word)" "$(mt_qargs "$1")"
}

mt_tr_patch_macho() {
    # `argc != 3`.
    [ $# -eq 2 ] || { printf 'Usage: %s input output\n' "$MT_PROG" >&2; return 1; }
    printf '%s declassify%s\n' "$(mt_pre_word)" "$(mt_qargs "$1" "$2")"
}

mt_tr_rename_segment() {
    # `argc != 4`, then the 16-byte segname check, in rename_segment's words.
    [ $# -eq 3 ] || { printf 'Usage: %s binary OLDNAME NEWNAME\n' "$MT_PROG" >&2; return 1; }
    [ "${#3}" -le 16 ] || { mt_die 'new segment name longer than 16 bytes'; return 1; }
    printf '%s segment%s\n' "$(mt_pre_word)" "$(mt_qargs "$1" "$2" "$3")"
}

mt_tr_retag_swift_classes() {
    # `argc < 2`. This is the one tool whose grammar is variadic over FILES
    # rather than over flags, and macho9 retag-swift takes exactly one file --
    # so the translation is a loop, one line per file, in argv order.
    [ $# -ge 1 ] || { printf 'Usage: %s binary [binary ...]\n' "$MT_PROG" >&2; return 1; }
    mt_pre="$(mt_pre_word)"
    for mt_f in "$@"; do
        printf '%s retag-swift%s\n' "$mt_pre" "$(mt_qargs "$mt_f")"
    done
}

# The macho9 program word, quoted, with mt_qargs' leading space trimmed.
mt_pre_word() {
    mt_p="$(mt_qargs "${MACHO9:-macho9}")"
    printf '%s' "${mt_p# }"
}

# ---- dispatcher ----------------------------------------------------------
#
# mt_translate TOOL ARG...
#
# TOOL is the historical binary's name. The usage lines print MT_PROG where the
# C tools printed argv[0]; it is set to TOOL on every call, and a wrapper that
# wants its own $0 there instead presets MT_PROG0.
mt_translate() {
    [ $# -ge 1 ] || { printf 'usage: translate.sh TOOL ARG...\n' >&2; return 2; }
    mt_tool=$1; shift
    # MT_PROG is recomputed on EVERY call, so a long-lived shell that sources
    # this file once and translates many invocations cannot leak one tool's
    # name into another's usage line. A wrapper that wants its own $0 there
    # presets MT_PROG0 instead.
    MT_PROG=${MT_PROG0:-$mt_tool}
    case $mt_tool in
        change_dylib)         mt_tr_change_dylib "$@" ;;
        add_version_min)      mt_tr_add_version_min "$@" ;;
        patch_macho)          mt_tr_patch_macho "$@" ;;
        rename_segment)       mt_tr_rename_segment "$@" ;;
        retag_swift_classes)  mt_tr_retag_swift_classes "$@" ;;
        fix_macho)            mt_tr_fix_macho "$@" ;;
        *)
            # Not one of the six. There is deliberately no fallback and no
            # guess: emitting a plausible-looking macho9 line for a tool this
            # file has never heard of is the one failure mode the plan names
            # outright.
            printf 'translate.sh: no equivalent -- unknown tool %s (expected one of: change_dylib add_version_min patch_macho rename_segment retag_swift_classes fix_macho)\n' "$mt_tool" >&2
            return 2 ;;
    esac
}

# Run directly, unless sourced with MT_SOURCED set (which is how Task 2's
# wrappers pull the functions in without triggering a translation).
if [ -z "${MT_SOURCED:-}" ]; then
    mt_translate "$@"
    exit $?
fi
