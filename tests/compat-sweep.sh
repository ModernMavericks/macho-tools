#!/bin/sh
# tests/compat-sweep.sh -- run EVERY enumerated argument combination of the six
# historical tools BOTH ways, on the same input, and record what each did.
#
#   sh tests/compat-sweep.sh <bindir> [outfile]
#
# The deliverable is the matrix it writes (default tests/compat-matrix.tsv),
# not a pass/fail. Every row says what the OLD tool did and what the
# compat/translate.sh -> macho9 translation did, so the two can be compared
# after the C sources are gone. Task 2 deletes them; from that commit on, these
# rows and the SHA-256s in them are the only surviving record of what the old
# binaries produced.
#
# WHY THIS IS NOT A ctest, and why differential.sh isn't either: it needs both
# families of binaries built, it takes minutes, and Task 2 removes half of what
# it drives. Run it by hand, on real 10.9, when the translation changes:
#
#   cmake --preset native-local && cmake --build --preset native-local
#   sh tests/compat-sweep.sh /private/tmp/mm-build/schmonz/macho-tools/native
#
# ---- what "exhaustive" means here ----------------------------------------
#
# Controller ruling M bounds it: every single flag, every ordered PAIR and
# every ordered TRIPLE of the enumerated flags, over a fixed small argument
# vocabulary, for all six tools. Order matters and is swept, because these
# parsers are position-sensitive loops. Singles mostly look clean; the plan's
# own precedent is that the surprises live in pairs and triples (`-change X`
# with `-delete X` produced a binary dyld refused, exited 0, and shipped that
# way for months, while each flag alone was fine).
#
#   change_dylib  10 flags -> 10 + 100 + 1000 = 1110
#   fix_macho      3 flags ->  3 +   9 +   27 =   39
#
# The other four tools have NO flags -- their whole argument surface is arity
# plus which files are named -- so for them "exhaustive" is enumerated by hand
# below (ARITY CASES), covering every arity the parser distinguishes and, for
# each, a matching / non-matching / non-Mach-O / absent argument.
#
# On top of that, EXTRA CASES holds hand-picked invocations that the fixed
# vocabulary structurally cannot reach: the real install.sh production line,
# the capacity caps (which need 17 and 33 repeats), and the chained rename
# `-rename_seg __DATA __X -rename_seg __X __Y`, whose two halves interact only
# because the first one's output is the second one's input.
#
# ---- the argument vocabulary, and why it is shaped this way --------------
#
# Every operation that names an EXISTING thing uses the SAME value in every
# slot -- one real dependency path, one real rpath, one real segment name. Every
# operation that names a NEW thing gets a slot-indexed value. That is
# deliberate: it is what makes `-change OLD N1 -delete OLD` (the historical
# bug) and `-delete OLD -reexport OLD` fall out of the generated cross product
# instead of needing to be remembered, while `-add P1 -add P2` still adds two
# distinct commands rather than the same one twice.
#
# Every vocabulary token is free of whitespace, so the generator can build an
# argv by unquoted word splitting. compat/translate.sh itself quotes correctly
# (tests/translate_test.sh covers a path with a space and a quote); this
# constraint is the SWEEP's, not the translator's.
#
# ---- how a row is judged -------------------------------------------------
#
# Both sides operate on a file named "f" in their own directory and are invoked
# with that RELATIVE path, because every one of these tools prints the path it
# was given -- differential.sh does the same, for the same reason.
#
#   old side   the compat binary, once
#   new side   compat/translate.sh, then each line it printed, in order,
#              stopping at the first nonzero exit
#
# What is compared is the OUTPUT BYTES and the EXIT CODE. Stdout is deliberately
# NOT compared: mr_apply_file prints a "header pad"/"updated" pair per pass, so
# one old invocation and a sequence of two or three macho9 ones cannot possibly
# print the same thing, and the plan's Task 0 evidence found no caller that
# parses these tools' stdout as data. The first line of each side's STDERR is
# recorded in the row instead, so a reader can see whether the two refused for
# the same reason -- which is the part that matters and the part a caller
# (install.sh's wrapper puts it in front of a human) actually sees.
#
# A row's class is then:
#
#   preserved      both exited 0 and the output bytes are identical
#   bytes-differ   both exited 0, bytes differ -- regression or deliberate fix
#   blocked        old exited 0, the new side did not -- THE class that matters
#   both-refuse    neither exited 0
#   improvement    old refused, the new side did the work
#   no-command     the translation is empty (the old invocation was a no-op)
#
# and any refusing class picks up a "+partial" suffix when the new side had
# ALREADY written the file before a later command in the sequence failed. That
# suffix is the split-into-a-sequence cost, measured.
#
# Read "improvement" carefully: it is a mechanical label for "old refused, new
# did not", and rename_segment's exit 2 for "nothing matched" lands in it. That
# one is a divergence the wrapper must REPRODUCE (a controller ruling says so),
# not an improvement to keep.
#
# "blocked" and "bytes-differ" are findings to report, not automatic blockers:
# the plan weights this sweep as DISCOVERY, with the known-caller end-to-end
# tests as the decisive gate.
#
# A SWEEP THAT REWRITES NOTHING PROVES NOTHING, so the summary reports how many
# invocations actually changed their input on each side -- differential.sh's
# own hard-won lesson, and the same reason it exits nonzero when that is zero.
set -u

BIN="${1:?usage: compat-sweep.sh <bindir> [outfile]}"
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUT="${2:-$ROOT/tests/compat-matrix.tsv}"

for t in macho9 change_dylib add_version_min rename_segment retag_swift_classes patch_macho fix_macho; do
    [ -x "$BIN/$t" ] || { echo "compat-sweep: $BIN/$t not found or not executable" >&2; exit 1; }
done
[ -r "$ROOT/compat/translate.sh" ] || { echo "compat-sweep: compat/translate.sh missing" >&2; exit 1; }

# Source the translator instead of exec'ing it per combination: same code
# either way (that is ruling L's whole point), one fewer process per row.
MT_SOURCED=1
export MT_SOURCED
. "$ROOT/compat/translate.sh"

T=$(mktemp -d "${TMPDIR:-/tmp}/macho-compat-sweep.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/A" "$T/B"

# ---- the base image -----------------------------------------------------
#
# tests/fixture.macho is a real 10.9-built executable, committed, so the matrix
# is reproducible from the repo alone. It carries one LC_LOAD_DYLIB, LC_UUID,
# LC_SOURCE_VERSION, LC_DYLIB_CODE_SIGN_DRS, a __DATA segment and 2576 bytes of
# header pad -- but NO LC_RPATH, which would collapse every -*-rpath row into
# "nothing matched". So the base gets one appended, ONCE, with macho9 (the same
# mr_apply_file both families reach), and its digest is printed into the matrix
# header so a later reader knows exactly what these rows describe.
#
# It also gets a SPARE LC_LOAD_DYLIB appended, and that spare -- not
# libSystem -- is what the generated -change/-delete/-reexport rows name.
# Deleting libSystem from this image is refused for a real, good reason
# ("symbol _printf still binds to the dylib being deleted"), and if the whole
# cross product named it then every -delete row would be testing that one
# refusal instead of the ordinal-renumbering rewrite the plan's own historical
# bug lived in. The spare has nothing bound to it, so -delete really deletes
# and really renumbers. The binding refusal is still swept, on libSystem, in
# EXTRA CASES.
cp "$ROOT/tests/fixture.macho" "$T/base" || exit 1
RP_OLD='@loader_path/../lib'
DY_OLD='@loader_path/spare0.dylib'
( cd "$T" && "$BIN/macho9" rpath base -append "$RP_OLD" \
           && "$BIN/macho9" dylib base -append "$DY_OLD" ) >/dev/null 2>&1 || {
    echo "compat-sweep: could not prepare the base image" >&2; exit 1; }

# A non-Mach-O and an absent path, for the arity cases.
printf 'not a mach-o at all\n' > "$T/notmacho"
ABSENT=/nonexistent/compat-sweep/no-such-file

# One digest per SIDE, not per file, because these six tools do not all write
# the same place: change_dylib and friends rewrite `f` in place, patch_macho
# reads `f` and writes `o`, and the non-Mach-O arity cases are handed `nm`.
# Hashing all three together gives one number that changes if the side changed
# anything at all, and stays equal to the base digest if it changed nothing.
digest() {
    { cat "$1/f"; [ -f "$1/o" ] && cat "$1/o"; cat "$1/nm"; } 2>/dev/null \
        | shasum -a 256 | cut -c1-16
}

# Put a side's directory back to the state every case starts from.
reset_side() {
    cp "$T/base" "$1/f"
    cp "$T/notmacho" "$1/nm"
    rm -f "$1/o"
}

reset_side "$T/A"
BASESHA=$(digest "$T/A")

# ---- vocabulary ---------------------------------------------------------
# DY_OLD (the spare appended above) and RP_OLD are set with the base image.
DY_BOUND='/usr/lib/libSystem.B.dylib'    # the real dependency _printf binds to
SEG_OLD='__DATA'                         # a segment the base image really has
# Slot-indexed NEW values. Short enough to fit the existing command, so a row
# is about the operation and not incidentally about header growth; the EXTRA
# CASES cover the long-path/-grow interaction on purpose.
DY_NEW1='@loader_path/../S1.dylib'; DY_NEW2='@loader_path/../S2.dylib'; DY_NEW3='@loader_path/../S3.dylib'
DY_ADD1='@loader_path/spare1.dylib'; DY_ADD2='@loader_path/spare2.dylib'; DY_ADD3='@loader_path/spare3.dylib'
DY_INS1='@loader_path/first1.dylib'; DY_INS2='@loader_path/first2.dylib'; DY_INS3='@loader_path/first3.dylib'
RP_NEW1='@loader_path/../lib1'; RP_NEW2='@loader_path/../lib2'; RP_NEW3='@loader_path/../lib3'
RP_ADD1='@executable_path/../F1'; RP_ADD2='@executable_path/../F2'; RP_ADD3='@executable_path/../F3'
# The three kinds the base image actually carries, so a strip really strips.
KIND1='uuid'; KIND2='codesig'; KIND3='source-version'
SEG_NEW1='__DATA_R1'; SEG_NEW2='__DATA_R2'; SEG_NEW3='__DATA_R3'

# cd_args FLAG SLOT -- print that flag and its operands for change_dylib.
cd_args() {
    case "$1$2" in
        -grow*)            echo "-grow" ;;
        -strip-lc1)        echo "-strip-lc $KIND1" ;;
        -strip-lc2)        echo "-strip-lc $KIND2" ;;
        -strip-lc3)        echo "-strip-lc $KIND3" ;;
        -add1)             echo "-add $DY_ADD1" ;;
        -add2)             echo "-add $DY_ADD2" ;;
        -add3)             echo "-add $DY_ADD3" ;;
        -insert1)          echo "-insert $DY_INS1" ;;
        -insert2)          echo "-insert $DY_INS2" ;;
        -insert3)          echo "-insert $DY_INS3" ;;
        -change1)          echo "-change $DY_OLD $DY_NEW1" ;;
        -change2)          echo "-change $DY_OLD $DY_NEW2" ;;
        -change3)          echo "-change $DY_OLD $DY_NEW3" ;;
        -delete*)          echo "-delete $DY_OLD" ;;
        -reexport*)        echo "-reexport $DY_OLD" ;;
        -add-rpath1)       echo "-add-rpath $RP_ADD1" ;;
        -add-rpath2)       echo "-add-rpath $RP_ADD2" ;;
        -add-rpath3)       echo "-add-rpath $RP_ADD3" ;;
        -change-rpath1)    echo "-change-rpath $RP_OLD $RP_NEW1" ;;
        -change-rpath2)    echo "-change-rpath $RP_OLD $RP_NEW2" ;;
        -change-rpath3)    echo "-change-rpath $RP_OLD $RP_NEW3" ;;
        -delete-rpath*)    echo "-delete-rpath $RP_OLD" ;;
        *) echo "compat-sweep: no vocabulary for change_dylib $1 slot $2" >&2; exit 1 ;;
    esac
}

# fm_args FLAG SLOT -- the same, for fix_macho.
fm_args() {
    case "$1$2" in
        -change1)               echo "-change $DY_OLD $DY_NEW1" ;;
        -change2)               echo "-change $DY_OLD $DY_NEW2" ;;
        -change3)               echo "-change $DY_OLD $DY_NEW3" ;;
        -strip_build_version*)  echo "-strip_build_version" ;;
        -rename_seg1)           echo "-rename_seg $SEG_OLD $SEG_NEW1" ;;
        -rename_seg2)           echo "-rename_seg $SEG_OLD $SEG_NEW2" ;;
        -rename_seg3)           echo "-rename_seg $SEG_OLD $SEG_NEW3" ;;
        *) echo "compat-sweep: no vocabulary for fix_macho $1 slot $2" >&2; exit 1 ;;
    esac
}

# The flag sets, ENUMERATED FROM THE PARSERS (compat/change_dylib.c's and
# compat/fix_macho.c's argv loops), not from either tool's usage text. Two
# places where they disagree, both recorded in the report:
#   * change_dylib's usage presents -grow as a standalone option, but `argc <
#     4` makes `change_dylib FILE -grow` a usage error.
#   * fix_macho's usage never mentions -rename_seg, which its parser accepts.
CD_FLAGS='-grow -strip-lc -add -insert -change -delete -reexport -add-rpath -change-rpath -delete-rpath'
FM_FLAGS='-change -strip_build_version -rename_seg'

# ---- the matrix ---------------------------------------------------------
ROWS="$T/rows"; : > "$ROWS"
total=0; n_preserved=0; n_bytes=0; n_blocked=0; n_bothref=0; n_improve=0
n_nocmd=0; n_nocmdbad=0; n_partial=0
old_mod=0; new_mod=0

# run_case TOOL ARGV...
#
# ARGV is the old tool's argv[1..], with the file already named "f". Runs both
# sides on identical copies of the base image and appends one matrix row.
run_case() {
    tool=$1; shift
    total=$((total + 1))

    reset_side "$T/A"; reset_side "$T/B"

    ( cd "$T/A" && "$BIN/$tool" "$@" ) >"$T/a.out" 2>"$T/a.err"; arc=$?

    # The translation. MT_PROG0 makes the usage lines name the tool rather than
    # this script, which is what a wrapper invoked as that tool would print.
    MT_PROG0=$tool
    cmds=$(mt_translate "$tool" "$@" 2>"$T/t.err"); trc=$?
    unset MT_PROG0

    ncmds=0
    [ -n "$cmds" ] && ncmds=$(printf '%s\n' "$cmds" | wc -l | tr -d ' ')

    : > "$T/b.out"; : > "$T/b.err"
    if [ "$trc" -ne 0 ]; then
        brc=$trc
        refuser=translate
        cp "$T/t.err" "$T/b.err"
    else
        refuser='-'
        brc=0
        if [ "$ncmds" -gt 0 ]; then
            printf '%s\n' "$cmds" > "$T/cmds"
            # PATH, not an absolute program word, so the translation recorded
            # in the matrix stays the portable text a wrapper would emit.
            ( cd "$T/B" && PATH="$BIN:$PATH" CMDS="$T/cmds" sh "$T/runner.sh" ) \
                >"$T/b.out" 2>"$T/b.err"
            brc=$?
            [ "$brc" -ne 0 ] && refuser=macho9
        fi
    fi

    asha=$(digest "$T/A")
    bsha=$(digest "$T/B")
    [ "$asha" != "$BASESHA" ] && old_mod=$((old_mod + 1))
    [ "$bsha" != "$BASESHA" ] && new_mod=$((new_mod + 1))

    if [ "$trc" -eq 0 ] && [ "$ncmds" -eq 0 ]; then
        # An empty translation claims the old invocation was a no-op. If the
        # old tool disagreed -- refused, or wrote something -- the claim is
        # wrong, and that is a defect in the translator, not a matrix row to
        # shrug at. Give it its own class so it cannot hide among the rest.
        if [ "$arc" -eq 0 ] && [ "$asha" = "$BASESHA" ]; then
            class=no-command; n_nocmd=$((n_nocmd + 1))
        else
            class=no-command-MISMATCH; n_nocmdbad=$((n_nocmdbad + 1))
        fi
    elif [ "$arc" -eq 0 ] && [ "$brc" -eq 0 ]; then
        if [ "$asha" = "$bsha" ]; then
            class=preserved; n_preserved=$((n_preserved + 1))
        else
            class=bytes-differ; n_bytes=$((n_bytes + 1))
        fi
    elif [ "$arc" -eq 0 ]; then
        class=blocked; n_blocked=$((n_blocked + 1))
    elif [ "$brc" -eq 0 ]; then
        class=improvement; n_improve=$((n_improve + 1))
    else
        class=both-refuse; n_bothref=$((n_bothref + 1))
    fi

    # A refusal that already wrote. One old invocation becomes a SEQUENCE
    # whenever it touches more than one family, and a step that fails after an
    # earlier step succeeded leaves a half-converted binary where the C tool
    # left the original untouched -- docs/PROPOSAL.md's "One friction worth
    # designing for", observed rather than argued. Marked on the class so it
    # cannot be read past.
    if [ "$brc" -ne 0 ] && [ "$bsha" != "$BASESHA" ]; then
        class="$class+partial"; n_partial=$((n_partial + 1))
    fi

    amsg=$(head -1 "$T/a.err" 2>/dev/null | tr '\t' ' ')
    bmsg=$(head -1 "$T/b.err" 2>/dev/null | tr '\t' ' ')
    [ -n "$amsg" ] || amsg='-'
    [ -n "$bmsg" ] || bmsg='-'
    tr_one=$(printf '%s' "$cmds" | tr '\n' ';' | sed 's/;$//')
    [ -n "$tr_one" ] || tr_one='-'
    argv=$*
    [ -n "$argv" ] || argv='(no arguments)'

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$tool" "$class" "$argv" "$tr_one" "$arc" "$asha" "$brc" "$bsha" \
        "$refuser" "$amsg" "$bmsg" >> "$ROWS"
}

# Runs the emitted command lines in order, stopping at the first nonzero exit
# and returning that exit code -- the contract compat/translate.sh's header
# states, in the one place the sweep needs it executed.
cat > "$T/runner.sh" <<'RUNNER'
while IFS= read -r line; do
    [ -n "$line" ] || continue
    eval "$line" || exit $?
done < "$CMDS"
exit 0
RUNNER

# ---- the generated cross product ----------------------------------------
echo "compat-sweep: sweeping change_dylib (singles, ordered pairs, ordered triples)"
for a in $CD_FLAGS; do
    run_case change_dylib f $(cd_args "$a" 1)
    for b in $CD_FLAGS; do
        run_case change_dylib f $(cd_args "$a" 1) $(cd_args "$b" 2)
        for c in $CD_FLAGS; do
            run_case change_dylib f $(cd_args "$a" 1) $(cd_args "$b" 2) $(cd_args "$c" 3)
        done
    done
done

echo "compat-sweep: sweeping fix_macho (singles, ordered pairs, ordered triples)"
for a in $FM_FLAGS; do
    run_case fix_macho f $(fm_args "$a" 1)
    for b in $FM_FLAGS; do
        run_case fix_macho f $(fm_args "$a" 1) $(fm_args "$b" 2)
        for c in $FM_FLAGS; do
            run_case fix_macho f $(fm_args "$a" 1) $(fm_args "$b" 2) $(fm_args "$c" 3)
        done
    done
done

# ---- ARITY CASES: the four flagless tools -------------------------------
#
# These four have no flags at all, so their entire argument surface is how many
# arguments they were handed and what those arguments name. Every arity their
# parser distinguishes is here, and for the interesting ones a matching, a
# non-Mach-O and an absent argument.
echo "compat-sweep: sweeping the four flagless tools (arity and argument kind)"
run_case add_version_min                                  # argc 1: usage error
run_case add_version_min f                                # argc 2: the only real form
run_case add_version_min nm
run_case add_version_min "$ABSENT"
run_case add_version_min f f                              # argc 3: usage error

run_case patch_macho                                      # argc 1
run_case patch_macho f                                    # argc 2
run_case patch_macho f o                                  # argc 3: the real form
run_case patch_macho f f                                  # IN == OUT, documented as safe
run_case patch_macho nm o
run_case patch_macho "$ABSENT" o
run_case patch_macho f o extra                            # argc 4

run_case rename_segment                                   # argc 1
run_case rename_segment f                                 # argc 2
run_case rename_segment f "$SEG_OLD"                      # argc 3
run_case rename_segment f "$SEG_OLD" "$SEG_NEW1"          # argc 4: matches
run_case rename_segment f __NOPE __ALSONOPE               # nothing matches -> exit 2
run_case rename_segment f "$SEG_OLD" 1234567890123456     # exactly 16: fits
run_case rename_segment f "$SEG_OLD" 12345678901234567    # 17: refused
run_case rename_segment f "$SEG_OLD" ''                   # empty NEW name
run_case rename_segment nm "$SEG_OLD" "$SEG_NEW1"
run_case rename_segment "$ABSENT" "$SEG_OLD" "$SEG_NEW1"
run_case rename_segment f "$SEG_OLD" "$SEG_NEW1" extra    # argc 5

run_case retag_swift_classes                              # argc 1: usage error
run_case retag_swift_classes f                            # one file
run_case retag_swift_classes nm                           # a non-Mach-O: skipped
run_case retag_swift_classes "$ABSENT"                    # absent: MSWIFT_ERROR
run_case retag_swift_classes f f                          # two files
run_case retag_swift_classes f nm                         # one good, one skipped
run_case retag_swift_classes f "$ABSENT"                  # one good, one failing
run_case retag_swift_classes f nm f                       # three files

# ---- EXTRA CASES --------------------------------------------------------
#
# Invocations the fixed vocabulary structurally cannot reach.
echo "compat-sweep: sweeping the hand-picked extra cases"

# install.sh's production line, verbatim but for the file name -- the single
# most important translation in this task.
run_case change_dylib f -strip-lc uuid -strip-lc codesig \
    -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib' \
    -change /usr/lib/libicucore.A.dylib '@loader_path/../I.dylib' \
    -change /usr/lib/libc++.1.dylib '@loader_path/../c++.1.dylib'
# tests/characterize.sh's line, this repo's own CI equivalence gate.
run_case change_dylib f -strip-lc uuid -strip-lc codesig \
    -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib'
# mavericks-claude-ongoing's local -insert-using line.
run_case change_dylib f -strip-lc uuid -strip-lc codesig \
    -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib' \
    -insert '@loader_path/libavxemu.dylib'
# The historical bug: -change X and -delete X together, which shipped a binary
# dyld refused for months. (The generated cross product already reaches this
# pair on the spare; repeated here so it is findable by name.)
run_case change_dylib f -change "$DY_OLD" "$DY_NEW1" -delete "$DY_OLD"
run_case change_dylib f -delete "$DY_OLD" -change "$DY_OLD" "$DY_NEW1"

# The same operations against the dependency this image really binds to, where
# deleting is refused for a real reason ("symbol _printf still binds to the
# dylib being deleted") rather than an artificial one. Both sides must refuse,
# and a mixed-family invocation that refuses is where the split-into-a-sequence
# cost shows up as a "+partial" row.
run_case change_dylib f -delete "$DY_BOUND"
run_case change_dylib f -change "$DY_BOUND" "$DY_NEW1" -delete "$DY_BOUND"
run_case change_dylib f -strip-lc uuid -delete "$DY_BOUND"
run_case change_dylib f -reexport "$DY_BOUND"
run_case fix_macho f -change "$DY_BOUND" "$DY_NEW1"

# Two over-long replacement paths. LONGPAD fits in the base image's 2576-byte
# header pad but NOT in the existing 56-byte LC_LOAD_DYLIB, which is exactly
# where change_dylib (rebuilds the table) and fix_macho (writes in place) part
# company. LONGGROW exceeds the pad as well, so it needs -grow / --allow-grow.
LONGPAD="@loader_path/$(printf 'y%.0s' $(seq 1 300)).dylib"
LONGGROW="@loader_path/$(printf 'y%.0s' $(seq 1 3000)).dylib"
run_case change_dylib f -change "$DY_OLD" "$LONGPAD"
run_case change_dylib f -change "$DY_OLD" "$LONGGROW"
run_case change_dylib f -grow -change "$DY_OLD" "$LONGGROW"
run_case change_dylib f -strip-lc uuid -change "$DY_OLD" "$LONGGROW"
run_case fix_macho f -change "$DY_OLD" "$LONGPAD"
run_case fix_macho f -change "$DY_OLD" "$DY_NEW1" -strip_build_version -rename_seg "$SEG_OLD" "$SEG_NEW1"

# The capacity caps: exactly at, and one past, each one. change_dylib prints
# the origin message; compat/translate.sh must print the same text (ruling from
# Task 0.5 -- routing through macho9 would print macho9's own wording).
i=0; strip16=''; while [ $i -lt 16 ]; do strip16="$strip16 -strip-lc uuid"; i=$((i+1)); done
run_case change_dylib f $strip16
run_case change_dylib f $strip16 -strip-lc uuid
i=0; add32=''; while [ $i -lt 32 ]; do add32="$add32 -add $DY_ADD1"; i=$((i+1)); done
run_case change_dylib f $add32
run_case change_dylib f $add32 -add "$DY_ADD1"
i=0; ins32=''; while [ $i -lt 32 ]; do ins32="$ins32 -insert $DY_INS1"; i=$((i+1)); done
run_case change_dylib f $ins32 -insert "$DY_INS1"
i=0; ch32=''; while [ $i -lt 32 ]; do ch32="$ch32 -change $DY_OLD $DY_NEW1"; i=$((i+1)); done
run_case change_dylib f $ch32
run_case change_dylib f $ch32 -change "$DY_OLD" "$DY_NEW1"
# The SHARED array: -change/-delete/-reexport all fill one 32-entry array, so
# 16 of one plus 17 of another overflows even though neither reaches 32 alone.
i=0; ch16=''; while [ $i -lt 16 ]; do ch16="$ch16 -change $DY_OLD $DY_NEW1"; i=$((i+1)); done
i=0; del17=''; while [ $i -lt 17 ]; do del17="$del17 -delete $DY_OLD"; i=$((i+1)); done
run_case change_dylib f $ch16 $del17
i=0; radd32=''; while [ $i -lt 32 ]; do radd32="$radd32 -add-rpath $RP_ADD1"; i=$((i+1)); done
run_case change_dylib f $radd32 -add-rpath "$RP_ADD1"
i=0; rch32=''; while [ $i -lt 32 ]; do rch32="$rch32 -change-rpath $RP_OLD $RP_NEW1"; i=$((i+1)); done
run_case change_dylib f $rch32 -change-rpath "$RP_OLD" "$RP_NEW1"

# Incomplete operands: the parser's `i + N < argc` guards. Every one of these
# falls through to the tool's "bad arg"/"Unknown option" branch NAMING THE
# FLAG, not the missing operand, which the translation has to reproduce.
run_case change_dylib f -change "$DY_OLD"
run_case change_dylib f -add
run_case change_dylib f -strip-lc
run_case change_dylib f -change-rpath "$RP_OLD"
run_case change_dylib f -strip-lc no-such-kind
run_case change_dylib f -bogus x
run_case change_dylib f -change "$DY_OLD" "$DY_NEW1" -bogus
run_case fix_macho f -change "$DY_OLD"
run_case fix_macho f -rename_seg "$SEG_OLD"
run_case fix_macho f -bogus
run_case fix_macho f -rename_seg "$SEG_OLD" 12345678901234567

# Every -strip-lc KIND through both binaries. The generated cross product only
# ever reaches uuid, codesig and source-version (the three the base image
# carries, so a strip really strips); build-version and code-sign-drs are here
# so no row of the vocabulary table goes unexercised. Two of these five find
# nothing to strip, and both sides have to agree about that too.
for k in uuid codesig source-version build-version code-sign-drs; do
    run_case change_dylib f -strip-lc "$k" -strip-lc "$k"
done

# The chained rename: fix_macho breaks out of its rename loop on the first
# match and does NOT re-examine the segment, so the second pair never fires.
# The translation runs two separate `macho9 segment` passes, and the second
# one's input is the first one's output.
run_case fix_macho f -rename_seg "$SEG_OLD" __X -rename_seg __X __Y
run_case rename_segment f "$SEG_OLD" "$SEG_OLD"

# ---- report -------------------------------------------------------------
{
    echo "# tests/compat-matrix.tsv -- generated by tests/compat-sweep.sh; DO NOT EDIT BY HAND."
    echo "#"
    echo "# Every enumerated argument combination of the six historical tools, run BOTH"
    echo "# ways on the same input: the old binary once, and compat/translate.sh's macho9"
    echo "# command line(s) in the order it printed them. After Task 2 deletes the C"
    echo "# sources this file is the surviving record of what those binaries did."
    echo "#"
    echo "# base image: tests/fixture.macho + one appended LC_RPATH ($RP_OLD)"
    echo "#             + one appended LC_LOAD_DYLIB ($DY_OLD)"
    echo "#             sha256[0:16] = $BASESHA"
    echo "# generated:  $(date -u '+%Y-%m-%dT%H:%M:%SZ') on $(uname -srm)"
    echo "#"
    echo "# columns: tool  class  old_argv  translation  old_rc  old_sha  new_rc  new_sha  refuser  old_msg  new_msg"
    echo "#   old_argv     the old tool's argv[1..], file named 'f'"
    echo "#   translation  the macho9 command lines, ';'-joined, or '-' for none"
    echo "#   *_sha        sha256[0:16] of that side's file AFTER the run; equal to the"
    echo "#                base digest above means the run changed nothing"
    echo "#   refuser      which side said no: translate (the old grammar's own"
    echo "#                refusal, reproduced), macho9, or '-'"
    echo "#   *_msg        first line of that side's stderr"
    echo "#"
    echo "# classification"
    printf '#   %-20s %6d  both exited 0, identical output bytes\n' preserved "$n_preserved"
    printf '#   %-20s %6d  both exited 0, DIFFERENT output bytes\n' bytes-differ "$n_bytes"
    printf '#   %-20s %6d  old exited 0, the new side did not -- REGRESSION-SHAPED\n' blocked "$n_blocked"
    printf '#   %-20s %6d  neither exited 0\n' both-refuse "$n_bothref"
    printf '#   %-20s %6d  old refused, the new side did the work\n' improvement "$n_improve"
    printf '#   %-20s %6d  the translation is empty (the old invocation was a no-op)\n' no-command "$n_nocmd"
    printf '#   %-20s %6d  empty translation, but the old tool did NOT no-op -- A DEFECT\n' no-command-MISMATCH "$n_nocmdbad"
    printf '#   %-20s %6d  (of the refusing rows above) the new side had already written\n' +partial "$n_partial"
    printf '#   %-20s %6d\n' TOTAL "$total"
    echo "#"
    printf '# inputs actually rewritten: old=%d new=%d (of %d)\n' "$old_mod" "$new_mod" "$total"
    echo "#"
    cat "$ROWS"
} > "$OUT"

echo "compat-sweep: combinations=$total preserved=$n_preserved bytes-differ=$n_bytes blocked=$n_blocked both-refuse=$n_bothref improvement=$n_improve no-command=$n_nocmd no-command-MISMATCH=$n_nocmdbad partial-writes=$n_partial"
echo "compat-sweep: inputs actually rewritten: old=$old_mod new=$new_mod"
echo "compat-sweep: matrix written to $OUT"
if [ "$old_mod" -eq 0 ] || [ "$new_mod" -eq 0 ]; then
    echo "compat-sweep: INCONCLUSIVE -- one side never rewrote its input, so agreement" >&2
    echo "  here proves nothing. Check the base image and the vocabulary." >&2
    exit 1
fi
exit 0
