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
# HOW LONG IT TAKES. Every sweep is 17 invocations x 2 builds per input, each
# reading and rewriting the whole file, plus three SHA-256s. On real 10.9
# hardware that is minutes for /usr/lib and /usr/bin, but the default roots
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
    for t in macho9 change_dylib add_version_min rename_segment retag_swift_classes; do
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
    if [ -n "$bad" ]; then
        diffs=$((diffs + 1))
        { echo "=== $1 ->$bad"
          diff "$T/a.out" "$T/b.out" | head -10
          diff "$T/a.err" "$T/b.err" | head -10; } >> "$REPORT"
    fi
    return 0
}

# macho9 VERB f ARGS...
m9() {
    verb="$1"; shift
    total=$((total + 1))
    cp "$SRC" "$T/A/f"; cp "$SRC" "$T/B/f"
    ( cd "$T/A" && "$REF/macho9" "$verb" f "$@" ) >"$T/a.out" 2>"$T/a.err"; arc=$?
    ( cd "$T/B" && "$NEW/macho9" "$verb" f "$@" ) >"$T/b.out" 2>"$T/b.err"; brc=$?
    compare "macho9 $verb $SRC $*"
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
    first=$("$REF/macho9" info "$T/probe" 2>/dev/null | sed -n 's/^  ordinal=[0-9]* path=//p' | head -1)
    [ -n "$first" ] || first="/usr/lib/libSystem.B.dylib"

    m9 dylib -replace "$first" "@loader_path/renamed.dylib"
    m9 dylib -append "@loader_path/libspare.dylib"
    m9 dylib -insert "@loader_path/libspare.dylib"
    m9 dylib -delete "$first"
    m9 dylib -reexport "$first"
    m9 dylib --allow-grow -replace "$first" "$longpath"
    m9 rpath -append /tmp/macho9diff
    m9 rpath -replace /usr/lib /tmp/macho9diff2
    m9 lc -delete uuid
    m9 lc -delete codesig -delete uuid
    m9 minos 10.9
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
    tool retag_swift_classes
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
