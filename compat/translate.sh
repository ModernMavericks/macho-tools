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
#   * Zero or more lines on stdout. Each line is a COMPLETE, `eval`-safe
#     command beginning with $MACHO9 (default: the bare word `macho9`).
#     Arguments are single-quoted only when they contain something outside
#     [A-Za-z0-9_@%+=:,./-], so the common case stays readable and the
#     hostile case stays correct.
#   * The lines are ORDERED. Run them in the order printed and stop at the
#     first nonzero exit; see "ordering" below for why the order matters.
#   * Exit 0 with ZERO lines means "this old invocation was a no-op on the
#     file; there is no macho9 command to run." (change_dylib accepts
#     `-grow` with no operations; it prints its header-pad line and changes
#     nothing.) It is NOT an error, and it is deliberately not translated as
#     some adjacent command that would do something.
#   * Exit 2 means NO EQUIVALENT: this argv is one the old tool accepted but
#     that no macho9 command line means the same thing as. Today there are two
#     -- an unknown TOOL name, and fix_macho's chained -rename_seg (see the
#     divergence list below). Nothing goes to stdout; emitting a
#     plausible-looking command that would do something else is exactly what
#     the plan forbids.
#   * Exit 1 means the OLD TOOL ITSELF would have refused this argv --
#     a usage error, an unknown flag, an unknown -strip-lc kind, a capacity
#     cap, an over-long segment name. The origin tool's exact message goes to
#     stderr and nothing goes to stdout. A wrapper can therefore refuse by
#     just forwarding this exit code, and it never runs a half-translated
#     command.
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
# ---- the grammar mapping -------------------------------------------------
#
#   change_dylib FILE ...            macho9 ...
#     -change O N                      dylib FILE -replace O N
#     -delete P                        dylib FILE -delete P
#     -reexport P                      dylib FILE -reexport P
#     -add P                           dylib FILE -append P
#     -insert P                        dylib FILE -insert P
#     -change-rpath O N                rpath FILE -replace O N
#     -delete-rpath P                  rpath FILE -delete P
#     -add-rpath P                     rpath FILE -append P
#     -strip-lc KIND                   lc    FILE -delete KIND
#     -grow                            --allow-grow on the dylib/rpath lines
#
#   fix_macho FILE ...
#     -change O N                      dylib   FILE -replace O N
#     -strip_build_version             lc      FILE -delete build-version
#     -rename_seg O N                  segment FILE O N          (one per pair)
#
#   add_version_min FILE               minos      FILE 10.9
#   patch_macho IN OUT                 declassify IN OUT
#   rename_segment FILE O N            segment    FILE O N
#   retag_swift_classes F1 F2 F3       retag-swift F1
#                                      retag-swift F2
#                                      retag-swift F3
#
# ---- ordering, and what splitting one call into several costs ------------
#
# change_dylib and fix_macho each apply EVERY operation in ONE pass over the
# load-command table. macho9 has a verb per family, so a single old invocation
# that touches more than one family becomes a SEQUENCE. The order emitted is:
#
#   1. lc       -- deleting load commands SHRINKS the table and hands header
#                  pad back. Anything that needs room must run after it.
#   2. dylib    -- library ordinals are the delicate part (a wrong ordinal is
#                  a dyld failure, not a silent mis-bind), so they are settled
#                  while the image is closest to the one the old tool saw.
#   3. rpath    -- LC_RPATH carries no ordinal, so it is the safest to move
#                  last. Between 2 and 3 the order is a free choice; it is
#                  fixed here so the emitted text is deterministic.
#   4. segment  -- fix_macho only. A rename changes no sizes, so it cannot
#                  compete for header pad with anything above.
#
# THIS IS NOT EQUIVALENT IN EVERY RESPECT, and docs/PROPOSAL.md says so under
# "One friction worth designing for". install.sh's production invocation mixes
# families -- it strips `uuid` and `codesig` to reclaim header bytes and then
# rewrites three dylib paths, and the stripping is what makes room for the
# rewriting. As two invocations that still works, but:
#
#   * the file is written TWICE, not once; and
#   * a failure BETWEEN the two leaves a partially-converted binary, where the
#     C tool left the original untouched.
#
# docs/PROPOSAL.md's answer is a `port` verb that does not exist. Until it
# does, the ordering above is a thing the caller must know, which is exactly
# why it is encoded here once rather than in each wrapper.
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
#   macho9 segment additionally runs mg_plausible and prints header-pad
#     chatter that rename_segment has neither of.
#   macho9 retag-swift refuses (exit 2) a non-Mach-O argument that
#     retag_swift_classes skipped silently, and exits 1 on the raced path
#     where retag_swift_classes exited 0.
#   macho9 declassify uses exit 2 where patch_macho returns a flat 1, writes
#     atomically, and names the file it wrote even on the pass-through.
#   fix_macho refuses a longer path where macho9 dylib rewrites it using
#     header pad, skips mg_plausible, tolerates a fat slice it cannot handle,
#     and writes non-atomically.
#
# There is one shape this file REFUSES outright rather than hands on, because
# no sequence of macho9 commands means the same thing: `fix_macho -rename_seg
# A B -rename_seg B C`, where a later pair renames a name an earlier pair
# produced. fix_macho's single pass gives each segment its FIRST match, so the
# later pair never fires; two `macho9 segment` passes chain, and the two
# binaries differ while both exit 0. See mt_fm_chain below for the measurement
# and for the three neighbouring shapes that are NOT affected.

# ---- quoting -------------------------------------------------------------
#
# One argument, quoted only if it needs it. The safe set is the usual
# shell-quoting one; note that a path containing a NEWLINE cannot survive the
# command substitution mt_qargs is used through, and nothing in these tools'
# grammar has ever accepted one.
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

# Caps, from src/rewrite.h. -change/-delete/-reexport SHARE one array of 32,
# as do -change-rpath/-delete-rpath; -add, -insert and -add-rpath each get
# their own 32; -strip-lc gets 16.
MT_MAX_OPS=32
MT_MAX_STRIP=16

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
            shift 2 ;;
        -add)
            [ $# -ge 2 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nadds" "$MT_MAX_OPS" -add || return 1
            mt_nadds=$((mt_nadds + 1))
            mt_dy="$mt_dy$(mt_qargs -append "$2")"
            shift 2 ;;
        -insert)
            [ $# -ge 2 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_ninserts" "$MT_MAX_OPS" -insert || return 1
            mt_ninserts=$((mt_ninserts + 1))
            mt_dy="$mt_dy$(mt_qargs -insert "$2")"
            shift 2 ;;
        -change)
            [ $# -ge 3 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nchanges" "$MT_MAX_OPS" -change || return 1
            mt_nchanges=$((mt_nchanges + 1))
            mt_dy="$mt_dy$(mt_qargs -replace "$2" "$3")"
            shift 3 ;;
        -delete)
            [ $# -ge 2 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nchanges" "$MT_MAX_OPS" -delete || return 1
            mt_nchanges=$((mt_nchanges + 1))
            mt_dy="$mt_dy$(mt_qargs -delete "$2")"
            shift 2 ;;
        -reexport)
            [ $# -ge 2 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nchanges" "$MT_MAX_OPS" -reexport || return 1
            mt_nchanges=$((mt_nchanges + 1))
            mt_dy="$mt_dy$(mt_qargs -reexport "$2")"
            shift 2 ;;
        -add-rpath)
            [ $# -ge 2 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nradds" "$MT_MAX_OPS" -add-rpath || return 1
            mt_nradds=$((mt_nradds + 1))
            mt_rp="$mt_rp$(mt_qargs -append "$2")"
            shift 2 ;;
        -change-rpath)
            [ $# -ge 3 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nrchanges" "$MT_MAX_OPS" -change-rpath || return 1
            mt_nrchanges=$((mt_nrchanges + 1))
            mt_rp="$mt_rp$(mt_qargs -replace "$2" "$3")"
            shift 3 ;;
        -delete-rpath)
            [ $# -ge 2 ] || { mt_die "bad arg: $1"; return 1; }
            mt_room "$mt_nrchanges" "$MT_MAX_OPS" -delete-rpath || return 1
            mt_nrchanges=$((mt_nrchanges + 1))
            mt_rp="$mt_rp$(mt_qargs -delete "$2")"
            shift 2 ;;
        *)
            mt_die "bad arg: $1"; return 1 ;;
        esac
    done

    # -grow is deliberately dropped from the `lc` line: `macho9 lc` has no
    # --allow-grow, and it does not need one -- deleting load commands only
    # ever shrinks the table, so the growth path change_dylib's allow_grow
    # unlocks is unreachable from a strip-only pass.
    mt_pre="$(mt_pre_word)"
    [ -n "$mt_lc" ] && printf '%s lc%s%s\n' "$mt_pre" "$(mt_qargs "$mt_file")" "$mt_lc"
    [ -n "$mt_dy" ] && printf '%s dylib%s%s%s\n' "$mt_pre" "$(mt_qargs "$mt_file")" "$mt_grow" "$mt_dy"
    [ -n "$mt_rp" ] && printf '%s rpath%s%s%s\n' "$mt_pre" "$(mt_qargs "$mt_file")" "$mt_grow" "$mt_rp"
    return 0
}

# ---- fix_macho -----------------------------------------------------------
mt_fm_usage() {
    printf 'Usage: %s <file> [-change old new] [-strip_build_version]\n' "$MT_PROG" >&2
    return 1
}

# CHAINED -rename_seg: no equivalent, so refuse.
#
# fix_macho applies EVERY -rename_seg pair in one pass over the load commands
# and `break`s out of its rename loop on the first match
# (compat/fix_macho.c's process_macho), so each segment gets the first pair
# matching its ORIGINAL name and a later pair naming a name an earlier pair
# produced never fires. The translation is one `macho9 segment` invocation per
# pair, and the second one reads the first one's OUTPUT -- so the chain that
# fix_macho refuses to follow, the sequence follows.
#
# Measured, on tests/fixture.macho, with the real binaries:
#
#   -rename_seg __DATA __X -rename_seg __X __Y
#       fix_macho     -> __X      (the second pair never fires)
#       the sequence  -> __Y      DIFFERENT BYTES, both exit 0
#
# That is a plausible-looking command line that does something else, which the
# plan forbids outright ("never a plausible-looking command that would do
# something else"; "never let a wrapper silently do something adjacent to what
# was asked"). Documenting a silent wrong answer does not satisfy either
# sentence, so this refuses instead.
#
# ONLY that shape refuses. Each of these was checked the same way and agrees
# byte-for-byte, so refusing them would be over-refusing:
#
#   -rename_seg __DATA __A -rename_seg __DATA __B   same OLD twice: fix_macho
#       takes the first, and the translation's second pass finds no __DATA
#       left to rename. Both end __A.
#   -rename_seg __DATA __B -rename_seg __TEXT __DATA   a later NEW equal to an
#       earlier OLD is fine: by the time the second pair is applied there is
#       no __DATA for it to collide with.
#   -rename_seg __DATA __A -rename_seg __TEXT __B   independent pairs.
#
# So the condition is exactly "some later pair's OLD equals some earlier pair's
# NEW", which a chain of three (__DATA -> __P -> __Q -> __R) also trips at its
# first link.
#
# mt_fm_chain OLD -- returns 1, having reported, if OLD is a name some earlier
# -rename_seg in this same invocation produced.
mt_fm_chain() {
    mt_ci=$IFS
    IFS='
'
    for mt_cn in $mt_segnews; do
        if [ "$1" = "$mt_cn" ]; then
            IFS=$mt_ci
            printf 'translate.sh: no equivalent -- -rename_seg %s renames a segment name an earlier -rename_seg in this same invocation produced; fix_macho applies every pair in ONE pass and gives each segment its FIRST match, so that later pair never fires, while separate macho9 segment passes would chain and produce a different binary\n' "$1" >&2
            return 1
        fi
    done
    IFS=$mt_ci
    return 0
}

mt_tr_fix_macho() {
    # `argc < 3`: program name plus fewer than two arguments.
    [ $# -ge 2 ] || { mt_fm_usage; return 1; }

    mt_file=$1; shift
    mt_lc='' mt_dy='' mt_seg='' mt_segnews=''

    while [ $# -gt 0 ]; do
        case $1 in
        -change)
            # fix_macho's own condition is `i + 2 < argc`, so a trailing
            # `-change OLD` with no NEW falls through to "Unknown option:
            # -change" -- naming the flag, not the missing operand.
            [ $# -ge 3 ] || { mt_die "Unknown option: $1"; return 1; }
            mt_dy="$mt_dy$(mt_qargs -replace "$2" "$3")"
            shift 3 ;;
        -strip_build_version)
            # Only ever one LC kind, and fix_macho's flag is a boolean, so a
            # repeat adds nothing. Emitting `-delete build-version` twice
            # would be a different command for the same intent.
            mt_lc="$(mt_qargs -delete build-version)"
            shift ;;
        -rename_seg)
            [ $# -ge 3 ] || { mt_die "Unknown option: $1"; return 1; }
            # Same 16-byte segname limit fix_macho checks here, before any
            # I/O, in its own words (which differ from rename_segment's).
            [ "${#3}" -le 16 ] || { mt_die "new segment name longer than 16 bytes: $3"; return 1; }
            # CHAINED RENAMES HAVE NO EQUIVALENT -- refuse. See the block
            # above mt_fm_chain for the mechanism and for exactly which
            # shapes are and are not affected.
            mt_fm_chain "$2" || return 2
            mt_segnews="$mt_segnews$3
"
            mt_seg="$mt_seg$(mt_qargs "$2" "$3")
"
            shift 3 ;;
        *)
            mt_die "Unknown option: $1"; return 1 ;;
        esac
    done

    mt_pre="$(mt_pre_word)"
    mt_fq="$(mt_qargs "$mt_file")"
    # No --allow-grow anywhere: fix_macho never grows a header. It refuses a
    # replacement that does not fit the EXISTING command, which is stricter
    # than `macho9 dylib` without --allow-grow (that one may still use header
    # pad). That difference is a matrix row, not something to paper over here.
    [ -n "$mt_lc" ] && printf '%s lc%s%s\n' "$mt_pre" "$mt_fq" "$mt_lc"
    [ -n "$mt_dy" ] && printf '%s dylib%s%s\n' "$mt_pre" "$mt_fq" "$mt_dy"
    if [ -n "$mt_seg" ]; then
        printf '%s' "$mt_seg" | while IFS= read -r mt_line; do
            [ -n "$mt_line" ] || continue
            printf '%s segment%s%s\n' "$mt_pre" "$mt_fq" "$mt_line"
        done
    fi
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
