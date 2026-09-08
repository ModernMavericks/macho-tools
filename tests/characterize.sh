#!/bin/sh
# Characterization: run the whole pipeline over a committed fixture and hash the
# result. The tools are deterministic file transformers, so the OUTPUT is the
# invariant worth pinning -- not the tool binaries, which can never match across
# a 2014 clang and a 2026 one.
#
#   sh tests/characterize.sh <bindir>        print the digest
#   sh tests/characterize.sh <bindir> check  compare against tests/EXPECTED
#
# This is what makes "native on 10.9" and "cross from a modern host"
# comparable: build both ways, run this, and the digests must match. A
# cross-built tool is x86_64 with a 10.9 floor, which still runs on a modern
# host (a deployment target is a floor, not a ceiling), so CI can execute it.
set -e
BIN="${1:?usage: characterize.sh <bindir> [check]}"
MODE="${2:-print}"
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d /tmp/macho-characterize.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM

cp "$HERE/fixture.macho" "$T/in"
"$BIN/patch_macho"     "$T/in" "$T/out" >/dev/null
"$BIN/add_version_min" "$T/out"         >/dev/null
"$BIN/change_dylib"    "$T/out" -strip-lc uuid -strip-lc codesig \
    -change "/usr/lib/libSystem.B.dylib" "@loader_path/../S.dylib" >/dev/null
"$BIN/rename_segment"  "$T/out" >/dev/null 2>&1 || true

DIGEST=$(shasum -a 256 < "$T/out" | cut -d' ' -f1)
if [ "$MODE" = check ]; then
    WANT=$(cat "$HERE/EXPECTED")
    if [ "$DIGEST" = "$WANT" ]; then
        echo "characterize: OK ($DIGEST)"
    else
        echo "characterize: MISMATCH" >&2
        echo "  expected $WANT" >&2
        echo "  got      $DIGEST" >&2
        echo "  The pipeline's output changed. Either a tool's behaviour changed" >&2
        echo "  (update EXPECTED deliberately, in the same commit) or this build" >&2
        echo "  is not equivalent to the reference one." >&2
        exit 1
    fi
else
    echo "$DIGEST"
fi
