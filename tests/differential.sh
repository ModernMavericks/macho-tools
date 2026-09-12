#!/bin/sh
# tests/differential.sh — run TWO builds of these tools over the same real
# Mach-O binaries and prove they behave identically: same output bytes, same
# exit code, same stdout, same stderr, on every invocation.
#
#   sh tests/differential.sh <ref-bindir> <new-bindir> [corpus-root...]
#
# This is the check that a refactor which is supposed to change NOTHING really
# changed nothing. It was written for Task 0.5 (moving change_dylib's rewrite
# into src/rewrite.c so macho9 stops fork/exec'ing it) and it is expected to be
# useful for every later task in the compat-retirement plan, which are all the
# same shape: replace how the work is reached without changing what it does.
#
# WHY THIS IS NOT A ctest. It needs two builds of this repo at different
# commits, and a machine with hundreds of real Mach-O binaries on it. CI has
# neither. Run it by hand, on a real target machine, when you have changed how
# a rewrite is reached and want to show the rewrite itself did not move:
#
#   git stash                                  # or: git worktree add of the ref
#   cmake -S . -B /tmp/ref -DMAVERICKS_EXPECTED_MODE=native
#   cmake --build /tmp/ref
#   git stash pop
#   cmake --preset native-local && cmake --build --preset native-local
#   sh tests/differential.sh /tmp/ref <native-local bindir>
#
# WHAT IT SWEEPS. There are ~86,000 regular files under the default roots on a
# stock 10.9 install and identifying a Mach-O costs a process each, so this
# does not classify all of them. It walks the roots, takes an evenly spaced
# MACHO_DIFF_SCAN-sized sample of that list (default 8000 — spacing, not
# truncation, so the sample spans every root rather than just the first), keeps
# the ones whose first four bytes are a Mach-O magic (thin 32/64, either
# endianness, and the classic 32-bit-offset fat container), and sweeps the
# first MACHO_DIFF_MAX of those (default 300). It prints all of those numbers
# and a magic histogram, so what was actually covered is on the record next to
# the result rather than assumed.
#
# HOW IT COMPARES. Both sides operate on a file named "f" inside their OWN
# directory and are invoked with that RELATIVE path. Every one of these tools
# prints the path it was given, so without this the two sides would differ on
# every single line that names the file and the diff would be useless. Do not
# "simplify" this to two absolute paths.
#
# A DIFFERENTIAL THAT REWRITES NOTHING PROVES NOTHING, so the summary reports
# how many invocations actually changed their input, not just how many ran.
# If that number is near zero, the corpus or the operations are wrong.
#
# HOW LONG IT TAKES. Every sweep is 18 invocations x 2 builds per input (plus
# one more, `macho9 declassify`, on the new build alone), each reading and
# rewriting the whole file, plus three SHA-256s. On real 10.9 hardware that is
# minutes for /usr/lib and /usr/bin, but the default roots
# include /System/Library/Frameworks, whose binaries are large and mostly fat
# — a full default sweep can run for an hour. Bound it by lowering
# MACHO_DIFF_MAX, or by naming smaller roots:
#
#   sh tests/differential.sh /tmp/ref <bindir> /usr/lib /usr/bin /bin /sbin /usr/sbin
set -u

REF="${1:?usage: differential.sh <ref-bindir> <new-bindir> [corpus-root...]}"
NEW="${2:?usage: differential.sh <ref-bindir> <new-bindir> [corpus-root...]}"
shift 2
if [ "$#" -gt 0 ]; then
    ROOTS="$*"
else
    ROOTS="/usr/lib /usr/bin /bin /sbin /usr/sbin /System/Library/Frameworks"
fi
MAX="${MACHO_DIFF_MAX:-300}"
SCAN="${MACHO_DIFF_SCAN:-8000}"

for d in "$REF" "$NEW"; do
    for t in machotool change_dylib add_version_min rename_segment retag_swift_classes patch_macho; do
        [ -x "$d/$t" ] || { echo "differential: $d/$t not found or not executable" >&2; exit 1; }
    done
done

T=$(mktemp -d "${TMPDIR:-/tmp}/macho-differential.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/A" "$T/B"

# ---- corpus -------------------------------------------------------------
# `od` rather than file(1): its output is a stable four bytes, and this
# codebase's own rule is never to parse a human-readable tool's text (see
# tests/README.md). The magics are spelled as the byte sequence od prints,
# which is the file's byte order, not the host's.
echo "differential: walking $ROOTS"
find $ROOTS -type f 2>/dev/null > "$T/allfiles" || true
nall=$(wc -l < "$T/allfiles" | tr -d ' ')
[ "$nall" -gt 0 ] || { echo "differential: no files found under $ROOTS" >&2; exit 1; }
stride=$((nall / SCAN)); [ "$stride" -lt 1 ] && stride=1
awk -v s="$stride" 'NR % s == 0' "$T/allfiles" > "$T/candidates"
ncand=$(wc -l < "$T/candidates" | tr -d ' ')
: > "$T/machos"
while IFS= read -r f; do
    [ -r "$f" ] || continue
    case $(od -An -tx1 -N4 "$f" 2>/dev/null | tr -d ' ') in
        cffaedfe|cefaedfe|feedfacf|feedface|cafebabe|bebafeca) echo "$f" >> "$T/machos" ;;
    esac
done < "$T/candidates"
nmacho=$(wc -l < "$T/machos" | tr -d ' ')
head -"$MAX" "$T/machos" > "$T/corpus"
ncorpus=$(wc -l < "$T/corpus" | tr -d ' ')
[ "$ncorpus" -gt 0 ] || { echo "differential: no Mach-O files found under $ROOTS" >&2; exit 1; }

echo "differential: $nall files under the roots, 1 in $stride examined ($ncand),"
echo "differential: $nmacho of those are Mach-O, sweeping $ncorpus (max $MAX)"
echo "differential: corpus by magic:"
while IFS= read -r f; do od -An -tx1 -N4 "$f" | head -1 | tr -d ' '; done < "$T/corpus" \
    | sort | uniq -c | sed 's/^/    /'

# ---- comparison ---------------------------------------------------------
total=0; diffs=0; modified=0
REPORT="$T/report"; : > "$REPORT"

compare() {
    ah=$(shasum -a 256 < "$T/A/f" | cut -d' ' -f1)
    bh=$(shasum -a 256 < "$T/B/f" | cut -d' ' -f1)
    [ "$ah" != "$SRCHASH" ] && modified=$((modified + 1))
    bad=""
    [ "$arc" != "$brc" ] && bad="$bad exit($arc/$brc)"
    [ "$ah" != "$bh" ] && bad="$bad bytes"
    cmp -s "$T/a.out" "$T/b.out" || bad="$bad stdout"
    cmp -s "$T/a.err" "$T/b.err" || bad="$bad stderr"
    record "$1" "$bad"
    return 0
}

# One disagreement, on the record. Shared by compare() and conv() rather than
# written twice: the two sweeps compare different files, but a difference is
# reported the same way for both.
record() {
    [ -z "$2" ] && return 0
    diffs=$((diffs + 1))
    { echo "=== $1 ->$2"
      diff "$T/a.out" "$T/b.out" | head -10
      diff "$T/a.err" "$T/b.err" | head -10; } >> "$REPORT"
    return 0
}

# macho9 VERB f o ARGS... -- for a verb that READS f and writes a named
# output rather than rewriting f. What gets compared is `o`, plus the two
# sides agreeing that they left `f` alone.
#
# EVERY macho9 VERB THIS SWEEP DRIVES IS ONE OF THOSE NOW. There used to be a
# second helper, `m9`, for the verbs that rewrote the file they were given
# (dylib, rpath, lc, segment); all four take FILE OUT, so it had no callers
# left and is gone. `grow` has since taken FILE OUT too, leaving `edit` as the
# only verb that would need it back -- and `edit` is not in this sweep.
mtout() {
    verb="$1"; shift
    total=$((total + 1))
    cp "$SRC" "$T/A/f"; cp "$SRC" "$T/B/f"
    rm -f "$T/A/o" "$T/B/o"
    ( cd "$T/A" && "$REF/machotool" "$verb" f o "$@" ) >"$T/a.out" 2>"$T/a.err"; arc=$?
    ( cd "$T/B" && "$NEW/machotool" "$verb" f o "$@" ) >"$T/b.out" 2>"$T/b.err"; brc=$?
    bad=""
    [ "$arc" != "$brc" ] && bad="$bad exit($arc/$brc)"
    cmp -s "$T/a.out" "$T/b.out" || bad="$bad stdout"
    cmp -s "$T/a.err" "$T/b.err" || bad="$bad stderr"
    cmp -s "$SRC" "$T/A/f" && cmp -s "$SRC" "$T/B/f" || bad="$bad input-modified"
    if [ "$arc" -eq 0 ] || [ "$brc" -eq 0 ]; then
        cmp -s "$T/A/o" "$T/B/o" || bad="$bad bytes"
        cmp -s "$SRC" "$T/A/o" || modified=$((modified + 1))
    fi
    record "machotool $verb $SRC o $*" "$bad"
    return 0
}

# TOOL f ARGS...
tool() {
    tl="$1"; shift
    total=$((total + 1))
    cp "$SRC" "$T/A/f"; cp "$SRC" "$T/B/f"
    ( cd "$T/A" && "$REF/$tl" f "$@" ) >"$T/a.out" 2>"$T/a.err"; arc=$?
    ( cd "$T/B" && "$NEW/$tl" f "$@" ) >"$T/b.out" 2>"$T/b.err"; brc=$?
    compare "$tl $SRC $*"
}

# patch_macho f o -- and, on the NEW build only, `macho9 declassify f o9`.
#
# This tool does not rewrite its input: it reads IN and writes OUT, which is
# why the helpers above could not sweep it and, until Task 0.6b, nothing did.
# That task moved its chained-fixups conversion into src/declassify.c and gave
# macho9 a `declassify` verb over the same code, so two questions get asked
# here, both about bytes rather than exit status:
#
#   REF vs NEW patch_macho          did the extraction change what the tool
#                                    produces? (the same question every other
#                                    line of this sweep asks of its tool)
#   NEW patch_macho vs NEW macho9   do the two front-ends over that one
#                                    implementation really write the same
#                                    output? Asked of the NEW build only --
#                                    the REF build's `declassify` predates the
#                                    verb and is a stub that exits nonzero, so
#                                    comparing it across builds would report a
#                                    difference that is the point of the task.
#
# The macho9 half runs on EVERY file, not only the ones patch_macho converted:
# where patch_macho declines, declassify must decline too (with its own code --
# EX_REFUSED for a judgement about the input, EX_FAIL for an operational
# failure -- but never 0, which would be a silent success on an input the
# other front-end refused). Keeping it inside the success branch would have
# left the sentence below true of patch_macho and false of declassify.
#
# A 10.9 corpus has no chained fixups in it, so what this exercises on the
# target machine -- for BOTH front-ends -- is the read path, the pass-through,
# and the refusals: most of what moved, but not the conversion arithmetic,
# which is pinned by cli_test.sh's hand-built fixtures and by
# chained-fixups.sh on a host whose linker emits the format. `differing=0` on
# this line is not a claim about the conversion; it is a claim about
# everything around it.
conv() {
    total=$((total + 1))
    cp "$SRC" "$T/A/f"; cp "$SRC" "$T/B/f"
    rm -f "$T/A/o" "$T/B/o" "$T/B/o9"
    ( cd "$T/A" && "$REF/patch_macho" f o ) >"$T/a.out" 2>"$T/a.err"; arc=$?
    ( cd "$T/B" && "$NEW/patch_macho" f o ) >"$T/b.out" 2>"$T/b.err"; brc=$?
    bad=""
    [ "$arc" != "$brc" ] && bad="$bad exit($arc/$brc)"
    cmp -s "$T/a.out" "$T/b.out" || bad="$bad stdout"
    cmp -s "$T/a.err" "$T/b.err" || bad="$bad stderr"
    if [ "$arc" -eq 0 ] || [ "$brc" -eq 0 ]; then
        cmp -s "$T/A/o" "$T/B/o" || bad="$bad bytes"
        cmp -s "$SRC" "$T/A/o" || modified=$((modified + 1))
    fi
    ( cd "$T/B" && "$NEW/machotool" declassify f o9 ) >"$T/b9.out" 2>"$T/b9.err"; b9rc=$?
    if [ "$brc" -eq 0 ]; then
        [ "$b9rc" -eq 0 ] && cmp -s "$T/B/o" "$T/B/o9" || bad="$bad declassify($b9rc)"
    else
        [ "$b9rc" -ne 0 ] || bad="$bad declassify-took-a-refused-input"
    fi
    record "patch_macho $SRC (and machotool declassify)" "$bad"
}

# 3000 characters is comfortably past any plausible linker's default header
# pad, so the two --allow-grow/-grow cases really do exercise the growth path
# (and, without it, the refusal) rather than fitting by luck.
longpath="@loader_path/$(printf 'y%.0s' $(seq 1 3000)).dylib"

while IFS= read -r SRC; do
    [ -r "$SRC" ] || continue
    cp "$SRC" "$T/probe" 2>/dev/null || continue
    SRCHASH=$(shasum -a 256 < "$SRC" | cut -d' ' -f1)
    # The -replace/-delete/-reexport target has to be a dependency this file
    # really has, or those cases all collapse into "nothing matched". Read it
    # out of `macho9 info`'s stable output -- never otool's.
    first=$("$REF/machotool" info "$T/probe" 2>/dev/null | sed -n 's/^  ordinal=[0-9]* path=//p' | head -1)
    [ -n "$first" ] || first="/usr/lib/libSystem.B.dylib"

    mtout dylib -replace "$first" "@loader_path/renamed.dylib"
    mtout dylib -append "@loader_path/libspare.dylib"
    mtout dylib -insert "@loader_path/libspare.dylib"
    mtout dylib -delete "$first"
    mtout dylib -reexport "$first"
    mtout dylib --allow-grow -replace "$first" "$longpath"
    mtout rpath -append /tmp/machotooldiff
    mtout rpath -replace /usr/lib /tmp/machotooldiff2
    mtout lc -delete uuid
    mtout lc -delete codesig -delete uuid
    mtout minos 10.9
    tool change_dylib -change "$first" "@loader_path/renamed.dylib"
    tool change_dylib -strip-lc uuid -add "@loader_path/libspare.dylib"
    tool change_dylib -grow -change "$first" "$longpath"
    tool add_version_min
    # rename_segment and retag_swift_classes joined this sweep when Task 0.6a
    # moved their guts into src/segname.c and src/swift_retag.c -- the same
    # "the work moved, the behaviour must not" shape change_dylib and
    # add_version_min were already swept for. __DATA_R9 is a name nothing
    # ships, so the rename really does change bytes on any thin 64-bit input
    # (and reports its own refusal, identically on both sides, on the rest).
    tool rename_segment __DATA __DATA_R9
    # DO NOT OVER-TRUST THIS ONE. A 10.9 corpus predates Swift entirely, so no
    # binary in it carries a stable-ABI is-Swift tag: the retag arithmetic
    # never fires, this sweep never modifies an input, and what it actually
    # compares is the open / mi_open / find_section / "total:" paths. That is
    # worth comparing -- those are most of what moved into src/swift_retag.c --
    # but the tag flip itself is pinned by cli_test.sh's mkswift fixture, not
    # here, and a `differing=0` on this line says nothing about it.
    tool retag_swift_classes
    conv
done < "$T/corpus"

echo "differential: comparisons=$total differing=$diffs modified_inputs=$modified"
if [ "$diffs" -ne 0 ]; then
    echo "differential: FAILED -- the two builds do not agree:" >&2
    cat "$REPORT" >&2
    exit 1
fi
if [ "$modified" -eq 0 ]; then
    echo "differential: INCONCLUSIVE -- no invocation modified its input, so" >&2
    echo "  agreement here proves nothing. Check the corpus and the operations." >&2
    exit 1
fi
echo "differential: OK"
