#!/bin/sh
# tests/wrapper_test.sh -- the six /bin/sh wrappers' own behaviour: the
# grammar they translate, the exit codes they map, and the stdout they
# reshape.
#
#   sh tests/wrapper_test.sh <bindir>
#
# WHAT THIS IS FOR, AND WHAT IT IS NOT.
#
#   tests/translate_test.sh   pins the TEXT compat/translate.sh emits, and
#                             never runs macho9 on a file.
#   tests/known-callers.sh    replays the real callers end to end. That is the
#                             gate; a failure there blocks.
#   this file                 everything BETWEEN those two: each wrapper's
#                             exit-code mapping and its stdout, on the cases
#                             tests/compat-matrix.tsv identified as the ones
#                             where macho9 and the C tool disagreed. Each
#                             assertion below names the divergence it closes.
#
# Every expected value here was measured against the C binaries built from
# commit 91b30b3 (the last commit carrying all six compat/*.c files) on real
# 10.9 (Darwin 13.4), the same provenance tests/known-callers.sh's digests
# have (tests/README.md's "Not run by ctest" section has the full account).
# The divergences themselves are documented at their sites: the
# list at the top of compat/translate.sh, and the "DELIBERATE DIVERGENCES
# FROM <tool>" blocks in cli/macho9.c's cmd_segment, cmd_retag_swift and
# cmd_declassify.
#
# set -u, not set -e: same reason as every other shell test here.
set -u

BIN="${1:?usage: wrapper_test.sh <bindir>}"
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
FIXTURE="$HERE/fixture.macho"
CC="${CC:-clang}"

for t in macho9 patch_macho change_dylib add_version_min rename_segment retag_swift_classes fix_macho; do
    [ -x "$BIN/$t" ] || { echo "wrapper_test: $BIN/$t not found or not executable" >&2; exit 1; }
done

T=$(mktemp -d "${TMPDIR:-/tmp}/macho-wrapper-test.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT INT TERM

pass=0; fail=0
ok()   { echo "PASS $1"; pass=$((pass + 1)); }
bad()  { echo "FAIL $1: $2" >&2; fail=$((fail + 1)); }
skip() { echo "SKIP $1: $2"; }

fresh() { cp "$FIXTURE" "$T/f"; }
sha()   { shasum -a 256 < "$1" | cut -d' ' -f1; }

# strip_vm FILE -- remove FILE's LC_VERSION_MIN_MACOSX, so add_version_min has
# something to do to it. tests/fixture.macho is a real 10.9 binary and already
# carries one, and a wrapper that installed nothing would pass an "it landed"
# assertion just as well as one that installed correctly. The program that
# does it is tests/strip_version_min.c, shared with tests/cli_test.sh, which
# needs the same fixture for the same reason; built here on first use.
#
# A FAILURE HERE IS LOUD, via bad(), rather than a return code the callers
# below would have to check one by one: a silent strip failure leaves an
# unstripped fixture, against which every "the command is there afterward"
# assertion passes without the wrapper having done anything at all.
strip_vm() {
    [ -x "$T/strip_version_min" ] \
        || "$CC" -O2 -o "$T/strip_version_min" "$HERE/strip_version_min.c" 2>"$T/strip_vm.out" \
        || { bad "strip_vm" "cannot build $HERE/strip_version_min.c: $(cat "$T/strip_vm.out")"; return 1; }
    "$T/strip_version_min" "$1" >"$T/strip_vm.out" 2>&1 \
        || { bad "strip_vm" "$1: $(cat "$T/strip_vm.out")"; return 1; }
    return 0
}

# mkswift_fixture FILE -- write a fresh Mach-O with two Swift class records
# (both on the stable-ABI tag) to FILE, so retag_swift_classes has something
# real to retag. tests/fixture.macho (what `fresh` copies) has ZERO Swift
# class records, so every retag_swift_classes assertion that only ever used
# `fresh` could not tell "retagged for real" from "installed nothing at all"
# -- both print "total: 0". The program is tests/mkswift.c, shared with
# tests/cli_test.sh, which needs the identical fixture for the identical
# reason; built here on first use.
mkswift_fixture() {
    [ -x "$T/mkswift" ] \
        || "$CC" -O2 -o "$T/mkswift" "$HERE/mkswift.c" 2>"$T/mkswift.out" \
        || { bad "mkswift_fixture" "cannot build $HERE/mkswift.c: $(cat "$T/mkswift.out")"; return 1; }
    "$T/mkswift" make "$1" >"$T/mkswift.out" 2>&1 \
        || { bad "mkswift_fixture" "$1: $(cat "$T/mkswift.out")"; return 1; }
    return 0
}

# firstline_is <file> <exact text> -- string equality, never a regex. The
# usage lines below embed $BIN, a path this test does not choose, and a `grep`
# pattern containing one would treat whatever punctuation the build directory
# happens to have as syntax. The same goes for the bracketed operands in
# retag_swift_classes' usage line.
firstline_is() { [ "$(head -1 "$1")" = "$2" ]; }

# has_line <file> <exact line> -- for the cases where the teaching message
# comes FIRST (it does whenever the translation succeeded and the refusal
# happened afterwards, in the wrapper). -F and -x keep it a whole-line
# string comparison rather than a pattern.
has_line() { grep -qxF "$2" "$1"; }

# run TOOL ARG... -- run a wrapper from inside $T with a RELATIVE path, the
# way tests/compat-sweep.sh and tests/differential.sh do: every one of these
# tools prints the path it was given, so a relative one keeps the expected
# strings short and host-independent. Sets $rc, $T/out and $T/err.
run() {
    tool=$1; shift
    ( cd "$T" && "$BIN/$tool" "$@" ) >"$T/out" 2>"$T/err"
    rc=$?
    return 0
}

# ---- every name is there, and runs --------------------------------------
#
# The plan's first "what must be true when you are done": all six names still
# install and still run -- and, since fix_macho joined them, all SIX are
# /bin/sh scripts. That last part is the retirement plan's headline made
# checkable: if any of these six is an executable rather than a script, this
# repo is shipping a second Mach-O rewriting binary again.
for t in patch_macho change_dylib add_version_min rename_segment retag_swift_classes fix_macho; do
    if [ -x "$BIN/$t" ] && head -1 "$BIN/$t" | grep -q '^#!/bin/sh$'; then
        ok "$t: installed, executable, and a /bin/sh script"
    else
        bad "$t" "not an executable /bin/sh script in $BIN"
    fi
done

# ---- POSIX sh, not bash -------------------------------------------------
#
# 10.9's /bin/sh is bash 3.2 in sh mode, which accepts plenty a stricter POSIX
# shell does not. Parse every wrapper under /bin/sh, and re-run one whole
# invocation under ksh -- the same second-shell cross-check
# tests/translate_test.sh does, for the same reason: a bashism should fail
# here, not on somebody's machine.
for f in "$ROOT"/compat/*.sh; do
    if /bin/sh -n "$f" 2>"$T/synerr"; then
        ok "sh -n $(basename "$f")"
    else
        bad "sh -n $(basename "$f")" "$(cat "$T/synerr")"
    fi
done
if [ -x /bin/ksh ]; then
    for f in "$ROOT"/compat/*.sh; do
        /bin/ksh -n "$f" 2>"$T/synerr" \
            && ok "ksh -n $(basename "$f")" \
            || bad "ksh -n $(basename "$f")" "$(cat "$T/synerr")"
    done
    fresh
    ( cd "$T" && /bin/ksh "$BIN/change_dylib" f -strip-lc uuid \
        -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib' ) >"$T/out" 2>"$T/err"
    kshrc=$?
    [ "$kshrc" -eq 0 ] \
        && ok "ksh: a whole mixed-family change_dylib run behaves the same" \
        || bad "ksh" "exit $kshrc: $(cat "$T/err")"
else
    skip "the second-shell cross-check" "/bin/ksh is not present on this host"
fi

# ---- a wrapper finds macho9 next to itself, not on PATH -----------------
#
# The wrappers are meant to be dropped into a directory beside macho9, which
# is how install.sh's $MF directory is shaped. Running one with a PATH that
# does NOT contain the bindir is the check that it resolves macho9 from its
# own location.
fresh
( cd "$T" && PATH=/usr/bin:/bin "$BIN/add_version_min" f ) >"$T/out" 2>"$T/err"
rc=$?
[ "$rc" -eq 0 ] \
    && ok "a wrapper finds macho9 beside itself with macho9 absent from PATH" \
    || bad "macho9 resolution" "exit $rc: $(cat "$T/err")"

# ---- the teaching message is on STDERR, never on stdout -----------------
#
# The plan puts it on stderr precisely so stdout stays byte-identical for
# anything reading it, and every known caller redirects stdout to /dev/null.
fresh
run add_version_min f
grep -q 'macho9 minos f f.new 10.9' "$T/err" \
    && ok "teaching message: on stderr" \
    || bad "teaching message" "not on stderr: $(cat "$T/err")"
# The teaching form is COMPLETE: macho9 never writes its input, so the
# equivalent a reader is shown ends with the install step the wrapper does
# for itself. Without it the block would teach a command that leaves FILE
# untouched and a stray f.new beside it.
has_line "$T/err" '    mv -f f.new f' \
    && ok "teaching message: ... and it names the install step too" \
    || bad "teaching message" "no 'mv -f f.new f' line: $(cat "$T/err")"
grep -q 'macho9 minos' "$T/out" \
    && bad "teaching message" "leaked onto stdout: $(cat "$T/out")" \
    || ok "teaching message: not on stdout"

# ---- change_dylib -------------------------------------------------------
#
# ONE emitted command: stdout must be byte-identical to what mr_apply_file
# printed for the C tool, which is the same thing `macho9 dylib` prints for
# the same file and ops. Asserted by running both and comparing, rather than
# by pinning a transcript that a different fixture would invalidate.
# ONE LINE OF MACHO9'S IS RESHAPED, and this comparison accounts for it
# exactly rather than loosening: `macho9 dylib` says "Wrote OUT (N bytes)"
# about the output it wrote, and the wrapper -- which installed that output
# over FILE -- says "Updated FILE (N bytes)" instead, which is the line
# mr_apply_file itself printed while the verb still rewrote FILE. Every other
# line, and the resulting bytes, must match.
fresh
run change_dylib f -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib'
cdrc=$rc
cp "$T/out" "$T/cd.out"
cdsha=$(sha "$T/f")
fresh
( cd "$T" && "$BIN/macho9" dylib f f.m9out -replace /usr/lib/libSystem.B.dylib \
    '@loader_path/../S.dylib' ) >"$T/m9.out" 2>/dev/null
m9sha=$(sha "$T/f.m9out")
sed 's|^Wrote f\.m9out (|Updated f (|' "$T/m9.out" >"$T/m9.want"
[ "$cdrc" -eq 0 ] && cmp -s "$T/cd.out" "$T/m9.want" && [ "$cdsha" = "$m9sha" ] \
    && ok "change_dylib: a single-family run is byte-identical to macho9's, stdout included" \
    || bad "change_dylib single-family" "exit $cdrc; stdout or bytes differ from macho9 dylib's; wrapper said [$(cat "$T/cd.out")] want [$(cat "$T/m9.want")]"

# MORE THAN ONE FAMILY is ONE `macho9 edit FILE - --output <temp>`, and what
# these two assert is that NOTHING IS LEFT beside FILE afterwards and that every
# line macho9 printed names FILE. Not that no temp is created -- one is, and
# always was: it used to be a copy of FILE that a SEQUENCE of commands was run
# against (`.FILE.macho9-compat.PID`), and it is now the output the one command
# writes and mw_finish installs, under that same name. The difference the first
# assertion is about is that the name must not SURVIVE; the difference the
# second is about is that macho9 is handed FILE as its input and so labels its
# progress lines with FILE, where the copy-aside sequence labelled them with the
# copy.
# In a directory of its OWN, holding nothing but FILE, so "nothing new
# appeared" is exact: run in $T and a stray left by one of the many earlier
# change_dylib invocations here would already be in the before-listing and
# this would see nothing. Whole-listing equality rather than a search for a
# name -- it is the stronger question, and `grep -vxF` with a multi-line
# pattern list is unusable on this platform's BSD grep 2.5.1, which drops a
# pattern another pattern is a prefix of (so `.` in the list stops `..` from
# matching).
rm -rf "$T/stray"; mkdir "$T/stray"
cp "$FIXTURE" "$T/stray/f"
stray_before=$(ls -a "$T/stray")
( cd "$T/stray" && "$BIN/change_dylib" f -strip-lc uuid \
    -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib' ) >"$T/out" 2>"$T/err"
cdmixrc=$?
[ "$cdmixrc" -eq 0 ] && [ "$(ls -a "$T/stray")" = "$stray_before" ] \
    && ok "change_dylib: a multi-family run leaves no stray file beside FILE" \
    || bad "change_dylib multi-family strays" "exit $cdmixrc; the directory holds [$(ls -a "$T/stray" | tr '\n' ' ')], was [$(printf '%s\n' "$stray_before" | tr '\n' ' ')]"
[ -s "$T/out" ] && ! grep -q 'macho9-compat' "$T/out" && grep -q '^f: ' "$T/out" \
    && ok "change_dylib: a multi-family run's stdout names FILE, not a copy" \
    || bad "change_dylib multi-family stdout" "stdout: $(cat "$T/out")"
rm -rf "$T/stray"

# A BACKSLASH IN THE PATH. The temp mw_prepare names is derived from the
# caller's own path, so its name is the caller's to choose -- and the filter
# that suppresses macho9's "Wrote <temp> (N bytes)" line has to compare against
# that name exactly. It once did not: passing the prefix to awk with `-v` ran it
# through awk's string-escape processing, so for a path containing a backslash
# awk looked for something the line does not start with and the stray line
# reached stdout, naming a temp no caller has heard of and breaking the
# byte-identical claim the assertion above makes. Both wrappers here go through
# the SAME shared mw_run_to_tmp, so one of them would have been enough to catch
# it; both are asserted because both leaked.
rm -rf "$T/bs"; mkdir "$T/bs" "$T/bs/back\slash"
cp "$FIXTURE" "$T/bs/back\slash/f"; cp "$FIXTURE" "$T/bs/back\slash/g"
strip_vm "$T/bs/back\slash/g"
( cd "$T/bs" && "$BIN/change_dylib" 'back\slash/f' -strip-lc uuid ) >"$T/bs.out" 2>"$T/bs.err"
bsrc=$?
[ "$bsrc" -eq 0 ] && ! grep -q '^Wrote ' "$T/bs.out" \
    && has_line "$T/bs.out" 'Updated back\slash/f (8528 bytes)' \
    && ok "change_dylib: a path containing a backslash still suppresses the temp-naming line" \
    || bad "change_dylib backslash path" "exit $bsrc, stdout: $(cat "$T/bs.out")"
( cd "$T/bs" && "$BIN/add_version_min" 'back\slash/g' ) >"$T/bs2.out" 2>"$T/bs2.err"
bsrc2=$?
[ "$bsrc2" -eq 0 ] && ! grep -q '^Wrote ' "$T/bs2.out" \
    && ok "add_version_min: ... and so does every other wrapper on the shared path" \
    || bad "add_version_min backslash path" "exit $bsrc2, stdout: $(cat "$T/bs2.out")"
# The teaching message reaches awk the same way, for command COUNTING and for
# indenting the block, so it is measured on the same path rather than assumed.
grep -q '^    macho9 lc ' "$T/bs.err" \
    && ok "change_dylib: ... and the teaching block is still indented and counted" \
    || bad "change_dylib backslash path" "teaching message: $(cat "$T/bs.err")"
rm -rf "$T/bs"

# EVERY -insert GOES TO THE FRONT, so as ONE batch `-insert A -insert B` leaves
# A at ordinal 1 and B at ordinal 2. Reaching that through a SEQUENCE of
# statements takes emitting them backwards, which is what compat/translate.sh
# does and tests/translate_test.sh pins as text; this is the same claim
# measured on a real binary, through the wrapper, on the path that emits an
# edit script (-strip-lc makes it a second family).
fresh
run change_dylib f -insert /A -insert /B -strip-lc uuid
cdins_rc=$rc
cdins=$( ( cd "$T" && "$BIN/macho9" info f ) 2>/dev/null )
[ "$cdins_rc" -eq 0 ] \
    && printf '%s\n' "$cdins" | grep -qxF '  ordinal=1 path=/A' \
    && printf '%s\n' "$cdins" | grep -qxF '  ordinal=2 path=/B' \
    && ok "change_dylib: -insert A -insert B leaves A at ordinal 1 and B at ordinal 2" \
    || bad "change_dylib insert order" "exit $cdins_rc; ordinals: $(printf '%s\n' "$cdins" | sed -n 's/^  \(ordinal=[0-9]* path=.*\)$/\1/p' | tr '\n' ' ')"

# AN UNWRITABLE FILE IS REFUSED, ON BOTH PATHS, WITH THE SAME ANSWER.
# change_dylib open()ed FILE O_RDWR before it looked at anything, so mode 444
# failed immediately having changed nothing. NO macho9 COMMAND STILL DOES THAT:
# a verb that writes an output opens FILE O_RDONLY, and `macho9 edit` installs
# by mkstemp+rename beside FILE -- which needs the DIRECTORY writable and never
# consults FILE's mode, so without the wrapper's check a read-only binary is
# silently replaced (exit 0, fresh inode). mw_prepare is that check, on both
# paths. BYTES AND INODE, not just the exit code: a rename-based rewrite
# preserves the mode, so mode alone would not show it happened.
#
# EXIT 1, AND THESE TWO ASSERTIONS USED TO REQUIRE 2. The authority for a
# compat wrapper's failure code is THE C TOOL, not macho9's numbering: every
# change_dylib failure row in tests/compat-matrix.tsv -- the frozen measurement
# of the six tools as C binaries -- is a flat 1. The 2 came from a narrow guard
# added while the single-family path still inherited mr_apply_file's own
# open(O_RDWR) failure, i.e. macho9's code for an operational failure; that
# guard is gone and mw_prepare, which every other wrapper on this install path
# already uses, answers with the C tool's 1.
for cd_ro_args in "-strip-lc uuid" "-strip-lc uuid -change /usr/lib/libSystem.B.dylib /x/y.dylib"; do
    fresh
    chmod 444 "$T/f"
    cd_ro_sha=$(sha "$T/f"); cd_ro_ino=$(stat -f '%i' "$T/f")
    # shellcheck disable=SC2086
    run change_dylib f $cd_ro_args
    cd_ro_rc=$rc
    chmod 644 "$T/f"
    case $cd_ro_args in *-change*) cd_ro_which="multi-family" ;; *) cd_ro_which="single-family" ;; esac
    [ "$cd_ro_rc" -eq 1 ] && grep -qxF 'open: Permission denied' "$T/err" \
        && [ "$(sha "$T/f")" = "$cd_ro_sha" ] && [ "$(stat -f '%i' "$T/f")" = "$cd_ro_ino" ] \
        && ok "change_dylib: a $cd_ro_which run on an unwritable FILE exits 1 (the C tool's only failure code), saying so, having changed neither its bytes nor its inode" \
        || bad "change_dylib unwritable ($cd_ro_which)" "exit $cd_ro_rc (want 1, the C tool's flat failure code), bytes changed=$([ "$(sha "$T/f")" = "$cd_ro_sha" ] && echo no || echo YES), inode changed=$([ "$(stat -f '%i' "$T/f")" = "$cd_ro_ino" ] && echo no || echo YES), stderr: $(cat "$T/err")"
done

# An ABSENT FILE is refused by the same check, and for the same reason it is
# now 1 rather than 2: that is what the C tool's open() failure exited with.
# The message is the C tool's own perror("open") text, which mw_require_writable
# reproduces -- so a caller cannot tell the two apart, which is the point.
run change_dylib nosuchfile -strip-lc uuid -change A B
[ "$rc" -eq 1 ] && grep -qxF 'open: No such file or directory' "$T/err" \
    && ok "change_dylib: an absent FILE exits 1 with the C tool's own open() message" \
    || bad "change_dylib absent FILE" "exit $rc (want 1), stderr: $(cat "$T/err")"

# THE CLOSING "Updated FILE (N bytes)" LINE, ON BOTH PATHS, AND ONLY WHEN THE
# BYTES CHANGED. The C tool printed it from mr_apply_file, which no longer
# writes FILE, so the wrapper prints it after installing the temp. Both paths
# matter and for different reasons: a single-family run gets it where macho9
# used to print it, and a multi-family `macho9 edit` never printed it at all
# (nothing asserted the line at the time, which is how it went missing).
for cd_up_args in "-strip-lc uuid" "-strip-lc uuid -change /usr/lib/libSystem.B.dylib /x/y.dylib"; do
    fresh
    case $cd_up_args in *-change*) cd_up_which="multi-family" ;; *) cd_up_which="single-family" ;; esac
    # shellcheck disable=SC2086
    run change_dylib f $cd_up_args
    cd_up_rc=$rc
    cd_up_size=$(wc -c < "$T/f" | tr -d ' ')
    [ "$cd_up_rc" -eq 0 ] && has_line "$T/out" "Updated f ($cd_up_size bytes)" \
        && ok "change_dylib: a $cd_up_which run that changed the file ends with the C tool's Updated line" \
        || bad "change_dylib Updated ($cd_up_which)" "exit $cd_up_rc, stdout: $(cat "$T/out")"
done
# NOT PRINTED when nothing changed: the C tool wrote nothing and said nothing
# in that case, and this is what keeps the wrapper from announcing an install
# mw_finish decided against.
fresh
run change_dylib f -change /nope/absent.dylib /also/absent.dylib
[ "$rc" -eq 0 ] && ! grep -q '^Updated ' "$T/out" \
    && ok "change_dylib: a run that changed nothing prints no Updated line" \
    || bad "change_dylib Updated (no-op)" "exit $rc, stdout: $(cat "$T/out")"

# A HARD-LINKED FILE IS REFUSED (1) BY EVERY WRAPPER ON THE INSTALL PATH, which
# for these three is new: their C tools wrote through their own descriptor, so
# every name for the inode saw the change, while installing by mv would leave
# the siblings on the old content. add_version_min's own case is asserted
# above; these are the three whose verbs converted together. Each must refuse
# before running anything, leave BOTH names byte-identical, and leave no temp.
rm -rf "$T/hl"; mkdir "$T/hl"
hl_case() {   # hl_case TOOL ARG...
    hl_tool=$1; shift
    cp "$FIXTURE" "$T/hl/f"; ln "$T/hl/f" "$T/hl/f2"
    hl_sha=$(sha "$T/hl/f")
    hl_rc=0
    ( cd "$T/hl" && "$BIN/$hl_tool" f "$@" ) >"$T/hl.out" 2>"$T/hl.err" || hl_rc=$?
    [ "$hl_rc" -eq 1 ] && grep -q 'hard link' "$T/hl.err" \
        && [ "$(sha "$T/hl/f")" = "$hl_sha" ] && [ "$(sha "$T/hl/f2")" = "$hl_sha" ] \
        && ok "$hl_tool: a hard-linked FILE is refused (1), both names untouched" \
        || bad "$hl_tool hard link" "exit $hl_rc: $(cat "$T/hl.err")"
    ls -a "$T/hl" | grep -q 'macho9-compat' \
        && bad "$hl_tool hard link" "a temp file was left beside FILE" \
        || ok "$hl_tool: ... and no temp was left beside it"
    rm -f "$T/hl/f" "$T/hl/f2"
}
hl_case change_dylib -strip-lc uuid
hl_case change_dylib -strip-lc uuid -change /usr/lib/libSystem.B.dylib /x/y.dylib
hl_case fix_macho -change /usr/lib/libSystem.B.dylib /x/y.dylib
hl_case rename_segment __DATA __DATA_HL
rm -rf "$T/hl"

# A RUN macho9 REFUSES LEAVES NO TEMP BESIDE FILE EITHER. The temp is made by
# the wrapper and written by macho9; a refusal means macho9 never wrote it, and
# the wrapper's EXIT trap is what keeps the name from surviving. Measured in a
# directory of its own so "nothing new appeared" is exact, and with whole-
# listing equality rather than a grep, for the reason the stray-file assertion
# above gives.
rm -rf "$T/refused"; mkdir "$T/refused"
printf 'not a Mach-O at all, not even close\n' >"$T/refused/f"
refused_before=$(ls -a "$T/refused")
refused_rc=0
( cd "$T/refused" && "$BIN/change_dylib" f -strip-lc uuid ) >"$T/ref.out" 2>"$T/ref.err" \
    || refused_rc=$?
[ "$refused_rc" -ne 0 ] && [ "$(ls -a "$T/refused")" = "$refused_before" ] \
    && ok "change_dylib: a run macho9 refuses leaves no temp beside FILE" \
    || bad "change_dylib refused strays" "exit $refused_rc; the directory holds [$(ls -a "$T/refused" | tr '\n' ' ')]"
rm -rf "$T/refused"

# THE CAPACITY CAPS. Both cap sites in cli/macho9.c say the wrapper has to
# enforce them itself and print the ORIGIN wording, because macho9 names its
# own flags (-append where change_dylib names -add). This is the assertion
# that the message a caller sees is still change_dylib's.
fresh
i=0; add33=''
while [ $i -lt 33 ]; do add33="$add33 -add P"; i=$((i + 1)); done
# shellcheck disable=SC2086
run change_dylib f $add33
[ "$rc" -eq 1 ] && grep -qxF 'too many -add (max 32)' "$T/err" \
    && ok "change_dylib: the -add cap refuses in change_dylib's own words" \
    || bad "change_dylib -add cap" "exit $rc, stderr: $(cat "$T/err")"
[ "$(sha "$T/f")" = "$(sha "$FIXTURE")" ] \
    && ok "change_dylib: the cap refuses before touching the file" \
    || bad "change_dylib -add cap" "the file was modified"

fresh
i=0; strip17=''
while [ $i -lt 17 ]; do strip17="$strip17 -strip-lc uuid"; i=$((i + 1)); done
# shellcheck disable=SC2086
run change_dylib f $strip17
[ "$rc" -eq 1 ] && grep -qxF 'too many -strip-lc (max 16)' "$T/err" \
    && ok "change_dylib: the -strip-lc cap refuses in change_dylib's own words" \
    || bad "change_dylib -strip-lc cap" "exit $rc, stderr: $(cat "$T/err")"

# An unknown flag, and an unknown -strip-lc KIND: the C tool's exact lines.
fresh
run change_dylib f -bogus x
[ "$rc" -eq 1 ] && grep -qxF 'bad arg: -bogus' "$T/err" \
    && ok "change_dylib: an unknown flag refuses in change_dylib's own words" \
    || bad "change_dylib unknown flag" "exit $rc, stderr: $(cat "$T/err")"
fresh
run change_dylib f -strip-lc no-such-kind
[ "$rc" -eq 1 ] && grep -qxF 'unknown -strip-lc kind: no-such-kind' "$T/err" \
    && ok "change_dylib: an unknown KIND refuses in change_dylib's own words" \
    || bad "change_dylib unknown KIND" "exit $rc, stderr: $(cat "$T/err")"

# `change_dylib FILE -grow` is `argc < 4`, so it is a USAGE error even though
# the usage text presents -grow as a standalone flag -- and the usage line
# names argv[0], exactly as the C tool's did.
fresh
run change_dylib f -grow
cd_usage="Usage: $BIN/change_dylib input [-grow] [-change old new] [-delete path] [-reexport path] [-add path] [-insert path] [-strip-lc name] [-change-rpath old new] [-delete-rpath path] [-add-rpath path] ..."
[ "$rc" -eq 1 ] && firstline_is "$T/err" "$cd_usage" \
    && ok "change_dylib: -grow alone is a usage error naming argv[0]" \
    || bad "change_dylib -grow alone" "exit $rc, stderr: $(head -1 "$T/err")"

# ---- patch_macho --------------------------------------------------------
#
# EXIT CODES ARE MAPPED. `macho9 declassify` returns EX_REFUSED (1) where it
# examined the input and declined, and EX_FAIL (2) for an operational
# failure; patch_macho returned a flat 1 for everything. A caller that
# tested `!= 0` is unaffected either way, but tests/leaf-tool-crashes.sh
# tests for exactly 1. An absent IN is the operational-failure case --
# declassify cannot even open it, so it exits EX_FAIL (2), not EX_REFUSED --
# and is the one here where the mapping actually changes a number.
run patch_macho nosuchfile out
[ "$rc" -eq 1 ] \
    && ok "patch_macho: an absent IN maps macho9's EX_FAIL back to a flat 1" \
    || bad "patch_macho absent IN" "exit $rc, want 1"

fresh
printf 'not a mach-o at all\n' > "$T/nm"
run patch_macho nm out
[ "$rc" -eq 1 ] \
    && ok "patch_macho: a non-Mach-O input exits 1, not 2" \
    || bad "patch_macho non-Mach-O" "exit $rc, want 1"

# THE PASS-THROUGH's stdout. macho9 names the file it wrote even when it only
# copied it; patch_macho never did. The wrapper drops that one line -- and
# only that one, and only on this path.
fresh
run patch_macho f o
[ "$rc" -eq 0 ] && grep -q '^Already patched' "$T/out" && ! grep -q '^Wrote ' "$T/out" \
    && ok "patch_macho: the pass-through prints no 'Wrote ...' line" \
    || bad "patch_macho pass-through" "exit $rc, stdout: $(cat "$T/out")"
cmp -s "$T/f" "$T/o" \
    && ok "patch_macho: the pass-through output is the input, byte for byte" \
    || bad "patch_macho pass-through" "output differs from input"

# THE FOURTH OBSERVABLE: OUT's MODE. patch_macho created OUT with
# open(argv[2], O_WRONLY|O_CREAT|O_TRUNC, 0755); `macho9 declassify` writes OUT
# through wa_write_new, which gives it the INPUT's mode and always a new inode.
# The wrapper installs macho9's output onto OUT with `mv` -- atomic, like every
# other wrapper on the install path -- after chmod'ing it to the mode the C tool
# would have left: `0755 & ~umask` for an OUT that did not exist, and OUT's own
# current mode for one that did (open() changes neither). Every expected mode
# here was measured against the pre-wrapper binary.
#
# WHAT THE ATOMIC INSTALL TRADES AWAY, and it is asserted below rather than
# described: OUT's INODE. `cat TEMP > OUT` kept it (and with it OUT's hard links
# and xattrs); `mv` cannot, so an OUT with other hard links is REFUSED instead of
# silently split -- the one new behaviour, shared with all five other wrappers.
#
# `stat -f` with an explicit format is a machine-readable request, not
# human-readable output being parsed -- same category as this file's
# `od -An -tx1`, and it is BSD stat, which every macOS has.
mode_of() { stat -f '%Lp' "$1"; }
ino_of()  { stat -f '%i' "$1"; }

# 1. a FRESH OUT takes 0755 masked by the umask, not a bare 0755 and not IN's
#    mode (which is what macho9 alone would give it).
fresh
chmod 640 "$T/f"
rm -f "$T/o"
( cd "$T" && umask 077 && "$BIN/patch_macho" f o ) >/dev/null 2>&1
[ "$(mode_of "$T/o")" = 700 ] \
    && ok "patch_macho: a fresh OUT gets 0755 masked by the umask (0700 under 077)" \
    || bad "patch_macho fresh mode" "mode $(mode_of "$T/o"), want 700"
fresh
chmod 640 "$T/f"
rm -f "$T/o"
( cd "$T" && umask 022 && "$BIN/patch_macho" f o ) >/dev/null 2>&1
[ "$(mode_of "$T/o")" = 755 ] \
    && ok "patch_macho: and 0755 under umask 022, not the input's own mode" \
    || bad "patch_macho fresh mode" "mode $(mode_of "$T/o"), want 755"

# 2. an EXISTING OUT keeps its own mode -- open() did not change one, so the
#    wrapper chmods the temp to it before installing. Its INODE is new: that is
#    the `mv`, and it is what makes OUT wholly old or wholly new rather than
#    half-written.
fresh
: > "$T/o"; chmod 600 "$T/o"; before_ino=$(ino_of "$T/o")
run patch_macho f o
[ "$rc" -eq 0 ] && [ "$(mode_of "$T/o")" = 600 ] \
    && ok "patch_macho: an existing OUT keeps its mode" \
    || bad "patch_macho existing OUT" "exit $rc, mode $(mode_of "$T/o"), want 600"
[ "$(ino_of "$T/o")" != "$before_ino" ] \
    && ok "patch_macho: ... and is installed atomically, so its inode is new" \
    || bad "patch_macho existing OUT" "inode unchanged -- the install was not a rename"

# 3. IN == OUT still converts IN, which the C tool allowed and `macho9
#    declassify` now refuses outright: the wrapper is what provides it, running
#    macho9 into a temp beside OUT so macho9 itself never sees OUT == IN. This
#    fixture is already converted, so the pass-through's bytes are IN's own and
#    mw_finish installs nothing at all -- mode AND inode survive, exactly as
#    they did when the C tool wrote through the path.
fresh
chmod 640 "$T/f"; before_ino=$(ino_of "$T/f"); before_sha=$(sha "$T/f")
run patch_macho f f
[ "$rc" -eq 0 ] && [ "$(mode_of "$T/f")" = 640 ] && [ "$(sha "$T/f")" = "$before_sha" ] \
    && ok "patch_macho: IN == OUT still converts IN, keeping its mode" \
    || bad "patch_macho IN == OUT" "exit $rc, mode $(mode_of "$T/f"), bytes changed=$([ "$(sha "$T/f")" = "$before_sha" ] && echo no || echo YES)"
[ "$(ino_of "$T/f")" = "$before_ino" ] \
    && ok "patch_macho: ... and an unchanged pass-through installs nothing, so the inode stands" \
    || bad "patch_macho IN == OUT" "the inode changed even though the bytes did not"

# 3b. A HARD-LINKED OUT IS REFUSED (1), both names untouched -- the wrapper's
#     own refusal, before macho9 runs. The C tool wrote through OUT's path and
#     every link saw the new content; `mv` would leave the siblings on the old
#     content, so this is refused rather than silently split. Same refusal every
#     other wrapper on the install path makes, from the same mw_prepare.
rm -rf "$T/pmhl"; mkdir "$T/pmhl"
cp "$FIXTURE" "$T/pmhl/o"; ln "$T/pmhl/o" "$T/pmhl/o2"
cp "$FIXTURE" "$T/pmhl/in"
pmhl_sha=$(sha "$T/pmhl/o")
pmhl_rc=0
( cd "$T/pmhl" && "$BIN/patch_macho" in o ) >"$T/pmhl.out" 2>"$T/pmhl.err" || pmhl_rc=$?
[ "$pmhl_rc" -eq 1 ] && grep -q 'hard link' "$T/pmhl.err" \
    && [ "$(sha "$T/pmhl/o")" = "$pmhl_sha" ] && [ "$(sha "$T/pmhl/o2")" = "$pmhl_sha" ] \
    && ok "patch_macho: a hard-linked OUT is refused (1), both names untouched" \
    || bad "patch_macho hard-linked OUT" "exit $pmhl_rc: $(cat "$T/pmhl.err")"
ls -a "$T/pmhl" | grep -q 'macho9-compat' \
    && bad "patch_macho hard-linked OUT" "a temp file was left beside OUT" \
    || ok "patch_macho: ... and no temp was left beside it"
rm -rf "$T/pmhl"

# 4. an existing OUT that is not writable FAILS, even where the directory is --
#    the C tool's open(O_WRONLY) failed on it, and mw_prepare's pre-check
#    answers for it now, in the words every wrapper's pre-check uses (the C
#    tool's own perror said "create output: Permission denied"; only the label
#    differs). Exit 1 either way, which is all a caller ever saw.
fresh
: > "$T/o"; chmod 444 "$T/o"
run patch_macho f o
[ "$rc" -eq 1 ] && grep -q 'Permission denied' "$T/err" \
    && ok "patch_macho: an unwritable existing OUT fails, as open(O_WRONLY) did" \
    || bad "patch_macho unwritable OUT" "exit $rc (want 1), stderr: $(cat "$T/err")"
[ "$(wc -c < "$T/o" | tr -d ' ')" = 0 ] \
    && ok "patch_macho: ... and the unwritable OUT was not touched" \
    || bad "patch_macho unwritable OUT" "OUT was written anyway"
chmod 644 "$T/o"; rm -f "$T/o"

# 5. a fresh OUT that cannot be created, because its directory is not writable.
#    The temp macho9 writes lives beside OUT, so macho9's own mkstemp is what
#    fails and what reports -- ONE line, and the wrapper maps its EX_FAIL to
#    patch_macho's flat 1. Asserted as "exactly one line" (the C tool printed
#    one perror too), which is what fails if a shell diagnostic ever leaks out
#    beside it, and re-run under ksh because every wrapper must behave the same
#    under both shells.
fresh
rm -rf "$T/ro"; mkdir "$T/ro"; chmod 555 "$T/ro"
for pm_sh in /bin/sh /bin/ksh; do
    [ -x "$pm_sh" ] || { skip "patch_macho: uncreatable OUT under $pm_sh" "no such shell"; continue; }
    ( cd "$T" && "$pm_sh" "$BIN/patch_macho" f ro/out ) >"$T/out" 2>"$T/err"
    rc=$?
    # The teaching message is two lines; the tool's own diagnostic is the rest.
    sed '1,2d' "$T/err" > "$T/err.rest"
    [ "$rc" -eq 1 ] && [ "$(wc -l < "$T/err.rest" | tr -d ' ')" = 1 ] \
        && grep -qxF 'mkstemp: Permission denied' "$T/err.rest" \
        && ok "patch_macho: an uncreatable OUT reports once, and exits 1 ($pm_sh)" \
        || bad "patch_macho uncreatable OUT ($pm_sh)" "exit $rc, stderr after the teaching message: $(cat "$T/err.rest")"
    [ ! -e "$T/ro/out" ] \
        && ok "patch_macho: ... and created nothing ($pm_sh)" \
        || bad "patch_macho uncreatable OUT ($pm_sh)" "OUT exists after a failed run"
done
chmod 755 "$T/ro"; rm -rf "$T/ro"

# 5b. AN OUT THAT IS A DIRECTORY is refused, in the C tool's own perror words.
#     Neither layer below would refuse it: macho9 writes a temp BESIDE OUT and
#     never looks at OUT, and `mv` given a directory destination moves the temp
#     INTO it and succeeds -- exit 0, with `adir/.adir.macho9-compat.PID`
#     created and nothing the caller asked for. Measured before the guard
#     existed, which is why this assertion is here.
fresh
rm -rf "$T/adir"; mkdir "$T/adir"
adir_before=$(ls -a "$T/adir")
run patch_macho f adir
[ "$rc" -eq 1 ] && grep -qxF 'create output: Is a directory' "$T/err" \
    && [ "$(ls -a "$T/adir")" = "$adir_before" ] \
    && ok "patch_macho: an OUT that is a directory is refused (1), as open() did" \
    || bad "patch_macho directory OUT" "exit $rc, stderr: $(cat "$T/err")"
rm -rf "$T/adir"

# 5c. AN OUT WHOSE NAME BEGINS WITH A DASH is still a file name, as it was for
#     the C tool's open(). `macho9 declassify` refuses such an OUT now
#     (m9_bad_out, since `-flag`-looking positionals are the mistake its own
#     grammar change invites), and the wrapper is unaffected because the OUT it
#     hands macho9 is the temp -- whose name starts with a dot. Pinned so that
#     refusal cannot migrate down here, where it would break a caller the C tool
#     served.
fresh
rm -f "$T/-dashout"
run patch_macho f -dashout
[ "$rc" -eq 0 ] && cmp -s "$T/f" "$T/-dashout" \
    && ok "patch_macho: an OUT beginning with a dash is a file name, as open() had it" \
    || bad "patch_macho dashed OUT" "exit $rc, stderr: $(cat "$T/err")"
rm -f "$T/-dashout"

# 6. THE INSTALL LEAVES NOTHING BEHIND, on the path that succeeds: the temp
#    beside OUT is mv'd or removed, never left. Measured in a directory of its
#    own, by whole-listing equality, for the reason the change_dylib stray-file
#    assertion above gives.
rm -rf "$T/pmdir"; mkdir "$T/pmdir"
cp "$FIXTURE" "$T/pmdir/in"
pmdir_before=$(ls -a "$T/pmdir")
pmdir_rc=0
( cd "$T/pmdir" && "$BIN/patch_macho" in out ) >"$T/pmdir.out" 2>"$T/pmdir.err" || pmdir_rc=$?
pmdir_want=$(printf '%s\nout\n' "$pmdir_before" | LC_ALL=C sort)
[ "$pmdir_rc" -eq 0 ] && [ "$(ls -a "$T/pmdir" | LC_ALL=C sort)" = "$pmdir_want" ] \
    && ok "patch_macho: a successful run creates OUT and nothing else" \
    || bad "patch_macho strays" "exit $pmdir_rc; the directory holds [$(ls -a "$T/pmdir" | tr '\n' ' ')]"
rm -rf "$T/pmdir"

# ---- add_version_min ----------------------------------------------------
#
# Both front-ends call mv_add_version_min, so stdout comes out of the same
# printf -- with ONE difference the wrapper makes on purpose: `macho9 minos`
# ends by naming the file it wrote, and the C tool, which rewrote FILE in
# place, never did. So the wrapper's stdout must be macho9's minus that final
# "Wrote ..." line, and the bytes it installs over FILE must be macho9's OUT.
# Asserted by running both and comparing, not by pinning a transcript.
fresh
strip_vm "$T/f"
avm_in=$(sha "$T/f")
run add_version_min f
avmrc=$rc
cp "$T/out" "$T/avm.out"
avmsha=$(sha "$T/f")
fresh
strip_vm "$T/f"
( cd "$T" && "$BIN/macho9" minos f m9out 10.9 ) >"$T/m9.out" 2>/dev/null
sed '$d' "$T/m9.out" > "$T/m9.trimmed"
[ "$avmrc" -eq 0 ] && cmp -s "$T/avm.out" "$T/m9.trimmed" && [ "$avmsha" = "$(sha "$T/m9out")" ] \
    && ok "add_version_min: identical to macho9 minos, stdout and bytes" \
    || bad "add_version_min" "exit $avmrc; stdout or bytes differ from macho9 minos'"
[ "$avmsha" != "$avm_in" ] \
    && ok "add_version_min: ... and it really changed the file it was given" \
    || bad "add_version_min" "the fixture came out unchanged, so nothing above was proved"
[ "$(sed -n '$p' "$T/m9.out" | cut -c1-6)" = 'Wrote ' ] \
    && ok "add_version_min: the line it suppresses is macho9's own 'Wrote ...'" \
    || bad "add_version_min" "macho9 minos did not end with a Wrote line: $(cat "$T/m9.out")"

run add_version_min
[ "$rc" -eq 1 ] && firstline_is "$T/err" "Usage: $BIN/add_version_min binary" \
    && ok "add_version_min: no argument is a usage error naming argv[0]" \
    || bad "add_version_min usage" "exit $rc, stderr: $(head -1 "$T/err")"

# The wrappers keep editing FILE "in place" -- by writing a temp beside the
# real target and mv-ing it over. A symlinked FILE updates its target and
# stays a symlink; a hard-linked FILE is refused; a refusal leaves no temp
# behind; mode and xattrs survive.
cp "$FIXTURE" "$T/w_real"; strip_vm "$T/w_real"
ln -s w_real "$T/w_link"
( cd "$T" && "$BIN/add_version_min" w_link ) >/dev/null 2>"$T/w.err" \
    && ok "wrapper: a symlinked FILE is edited" || bad "wrapper symlink" "$(cat "$T/w.err")"
[ -L "$T/w_link" ] && "$BIN/macho9" info "$T/w_real" | grep -q LC_VERSION_MIN_MACOSX \
    && ok "wrapper: ... through the link, which is still a link" || bad "wrapper symlink" "link replaced or target unchanged"

# mw_finish DISCARDS a temp whose bytes already match the target rather than
# mv-ing an identical copy over it -- the C tool wrote nothing when nothing
# changed, and a rename would hand the file a fresh inode (and leave every
# other name for the old one behind). A second run on the file the run above
# just converted is exactly that case, and the INODE is what distinguishes
# "discarded" from "installed an identical copy"; the bytes cannot.
w_ino=$(stat -f %i "$T/w_real")
( cd "$T" && "$BIN/add_version_min" w_link ) >"$T/w2.out" 2>/dev/null
[ "$(stat -f %i "$T/w_real")" = "$w_ino" ] \
    && ok "wrapper: a run that changes nothing discards its temp, keeping the inode" \
    || bad "wrapper no-op run" "the target got a new inode"
grep -q "already present" "$T/w2.out" \
    && ok "wrapper: ... and prints the C tool's 'already present' line" \
    || bad "wrapper no-op run" "stdout: $(cat "$T/w2.out")"

cp "$FIXTURE" "$T/w_h1"; strip_vm "$T/w_h1"; ln "$T/w_h1" "$T/w_h2"
h_before=$(shasum -a 256 < "$T/w_h1")
rc=0; "$BIN/add_version_min" "$T/w_h1" >/dev/null 2>"$T/wh.err" || rc=$?
[ "$rc" -eq 1 ] && [ "$(shasum -a 256 < "$T/w_h1")" = "$h_before" ] \
    && ok "wrapper: a hard-linked FILE is refused (1), untouched" || bad "wrapper hard link" "rc $rc"
grep -q "hard link" "$T/wh.err" && ok "wrapper: ... and says why" || bad "wrapper hard link" "$(cat "$T/wh.err")"
ls -a "$T" | grep -q 'macho9-compat' && bad "wrapper" "a temp file was left behind" \
    || ok "wrapper: no temp file left behind"

# A DIRECTORY IS NOT A HARD-LINK PROBLEM. Every directory's link count is
# greater than one (`.`, its parent's entry, one per subdirectory), so a
# link-count check that did not ask whether it was looking at a regular file
# would refuse one as "has N hard links" and offer a remedy -- break the link
# -- that means nothing. mw_prepare checks regular files only, so a directory
# falls through to macho9 and gets a true answer instead.
mkdir -p "$T/w_dir/sub1" "$T/w_dir/sub2"
rc=0; ( cd "$T" && "$BIN/add_version_min" w_dir ) >/dev/null 2>"$T/wd.err" || rc=$?
grep -q "hard link" "$T/wd.err" \
    && bad "wrapper directory" "diagnosed as a hard-link problem: $(cat "$T/wd.err")" \
    || ok "wrapper: a directory is not diagnosed as a hard-link problem"
[ "$rc" -ne 0 ] \
    && ok "wrapper: ... it is still refused (exit $rc), by macho9's own open" \
    || bad "wrapper directory" "exit 0 on a directory"
ls -a "$T" | grep -q 'macho9-compat' && bad "wrapper directory" "a temp file was left behind" \
    || ok "wrapper: ... and left no temp beside it"

cp "$FIXTURE" "$T/w_meta"; strip_vm "$T/w_meta"; chmod 0751 "$T/w_meta"
xattr -w com.apple.quarantine "0081;00000000;test;" "$T/w_meta"
"$BIN/add_version_min" "$T/w_meta" >/dev/null 2>&1
[ "$(stat -f %Lp "$T/w_meta")" = 751 ] && xattr -p com.apple.quarantine "$T/w_meta" >/dev/null 2>&1 \
    && ok "wrapper: mode and quarantine survive" || bad "wrapper metadata" "mode $(stat -f %Lp "$T/w_meta")"

# ---- rename_segment -----------------------------------------------------
#
# Three of cmd_segment's divergences, plus the thin-only one this task found.
fresh
run rename_segment f __DATA __DATA_R1
[ "$rc" -eq 0 ] && grep -qxF 'f: renamed 1 segment(s) __DATA -> __DATA_R1' "$T/out" \
    && ok "rename_segment: prints its own one-line message, not mr_apply_file's chatter" \
    || bad "rename_segment message" "exit $rc, stdout: $(cat "$T/out")"
[ "$(wc -l < "$T/out" | tr -d ' ')" = 1 ] \
    && ok "rename_segment: that one line is ALL of stdout" \
    || bad "rename_segment message" "$(wc -l < "$T/out") lines: $(cat "$T/out")"

# EXIT 2 WHEN NOTHING MATCHED -- the divergence a controller ruling requires
# reproducing, and the reason this wrapper counts the matches first.
fresh
before=$(sha "$T/f")
run rename_segment f __NOPE __ALSONOPE
[ "$rc" -eq 2 ] && [ ! -s "$T/out" ] && [ "$(sha "$T/f")" = "$before" ] \
    && ok "rename_segment: nothing matched exits 2, silently, without writing" \
    || bad "rename_segment no match" "exit $rc (want 2), stdout: $(cat "$T/out")"
# `macho9 segment` DID write its output here -- a 0 exit means OUT is the
# answer even when the answer is a copy -- so this is the one path where the
# wrapper deliberately skips mw_finish and lets the EXIT trap remove the temp.
ls -a "$T" | grep -q 'macho9-compat' \
    && bad "rename_segment no match" "the unused temp survived" \
    || ok "rename_segment: ... and the output macho9 did write is not left behind"

# A rename to the SAME name still MATCHED, so it is exit 0 with a count of 1 --
# not exit 2. This is what rules out implementing "nothing matched" as
# "the bytes did not change".
fresh
run rename_segment f __DATA __DATA
[ "$rc" -eq 0 ] && grep -qxF 'f: renamed 1 segment(s) __DATA -> __DATA' "$T/out" \
    && ok "rename_segment: renaming a segment to its own name is a match, not 'nothing to do'" \
    || bad "rename_segment same name" "exit $rc, stdout: $(cat "$T/out")"

# THE MATCH COUNT MUST COME FROM THE MATCHER, not from a printed name. These
# two shapes are why: mseg_rename_lc matches with strncmp over the 16-byte
# segname field, which is neither NUL-terminated nor free of whitespace, so a
# wrapper that recovered the count by reading names back out of `macho9 info`
# got both wrong -- it exited 2 and left the file alone where the C tool
# renamed and exited 0. Both were measured against the pre-wrapper binary
# before this wrapper was changed to take the count from
# `macho9 segment: renamed=<N>`.
#
# The odd segnames are made with `macho9 segment` itself, which is how they are
# reachable in the first place; both are legal in a char[16] field. That verb
# writes an OUT rather than the file it is given, so each of these
# fixture-preparation runs installs its own result, the same way the wrappers
# under test do.
fresh
( cd "$T" && "$BIN/macho9" segment f f.seg __DATA 1234567890123456 \
    && mv -f f.seg f ) >/dev/null 2>&1
before=$(sha "$T/f")
run rename_segment f 12345678901234567 __X
[ "$rc" -eq 0 ] && grep -qxF 'f: renamed 1 segment(s) 12345678901234567 -> __X' "$T/out" \
    && [ "$(sha "$T/f")" != "$before" ] \
    && ok "rename_segment: an OLD longer than 16 bytes still matches on its first 16" \
    || bad "rename_segment 17-byte OLD" "exit $rc, stdout: $(cat "$T/out")"

fresh
( cd "$T" && "$BIN/macho9" segment f f.seg __DATA 'A B' && mv -f f.seg f ) >/dev/null 2>&1
before=$(sha "$T/f")
run rename_segment f 'A B' __Y
[ "$rc" -eq 0 ] && grep -qxF 'f: renamed 1 segment(s) A B -> __Y' "$T/out" \
    && [ "$(sha "$T/f")" != "$before" ] \
    && ok "rename_segment: a segname containing whitespace still matches" \
    || bad "rename_segment whitespace segname" "exit $rc, stdout: $(cat "$T/out")"

# The count itself, and that it is the count and not a constant: rename a
# segment name the image carries TWICE (which is what this tool produces --
# see src/segname.h on __DATA_CONST -> __DATA leaving two __DATAs).
fresh
( cd "$T" && "$BIN/macho9" segment f f.seg __TEXT __DUP && mv -f f.seg f ) >/dev/null 2>&1
( cd "$T" && "$BIN/macho9" segment f f.seg __DATA __DUP && mv -f f.seg f ) >/dev/null 2>&1
run rename_segment f __DUP __ONE
[ "$rc" -eq 0 ] && grep -qxF 'f: renamed 2 segment(s) __DUP -> __ONE' "$T/out" \
    && ok "rename_segment: reports the real match count, not 1" \
    || bad "rename_segment count" "exit $rc, stdout: $(cat "$T/out")"

# ...and the signal that count comes from is one this build advertises.
"$BIN/macho9" --capabilities 2>/dev/null | grep -q '^verb segment .*reports=renamed' \
    && ok "capabilities: this build advertises segment reports=renamed" \
    || bad "capabilities" "segment does not advertise reports=renamed, which the wrapper needs"

# THIN ONLY. rename_segment ran mi_open, which refuses a fat container;
# `macho9 segment` goes through mr_apply_file, which handles one. Without the
# wrapper's gate this would rename inside a fat file the C tool refused --
# and most of /System/Library/Frameworks is fat.
FAT=''
for f in /usr/lib/libSystem.B.dylib /usr/lib/libc++.1.dylib /bin/ls; do
    [ -r "$f" ] || continue
    case $(od -An -tx1 -N4 "$f" 2>/dev/null | tr -d ' ') in
        cafebabe|bebafeca) FAT=$f; break ;;
    esac
done
if [ -n "$FAT" ]; then
    cp "$FAT" "$T/fat"; chmod u+w "$T/fat"
    before=$(sha "$T/fat")
    run rename_segment fat __DATA __DATA_R9
    [ "$rc" -eq 1 ] && has_line "$T/err" 'fat: not a readable 64-bit Mach-O' \
        && [ "$(sha "$T/fat")" = "$before" ] \
        && ok "rename_segment: a fat container is refused, as it always was" \
        || bad "rename_segment fat" "exit $rc, stderr: $(head -1 "$T/err")"
else
    skip "rename_segment: fat container" "no fat Mach-O found on this host"
fi

# mg_plausible, from the caller's side. mr_apply_file's last gate can reject an
# image for something the rewrite did not do -- it re-decides a property of the
# INPUT -- and rename_segment never had such a gate at all. src/rewrite.c
# skips it for a rename-only operation set (tests/cli_test.sh asserts that
# directly at the verb, and asserts the gate still runs for everything else);
# this is the same property seen through the wrapper, which is where a caller
# sees it.
#
# The input is tests/mkimplausible.c's committed fixture, built here. It used
# to be a scan of /usr/lib for a dylib the gate refused, with a SKIP when none
# turned up -- which passes on 10.9 and covers nothing on the cross runner,
# leaving the one behavioural change this task made to macho9 with no coverage
# where it is built. (Those /usr/lib refusals were not the heuristic getting
# real dylibs wrong: mg_plausible read a dylib's image base of 0 as
# mi_text_base's "no segment maps the header" sentinel and bailed before the
# heuristic ran. mi_image_base fixed that and all 26 now pass -- so the scan
# would find nothing today either. The fixture is refused on its merits.)
"$CC" -O2 -Wall -Wextra -I "$ROOT/src" -o "$T/mkimplausible" "$HERE/mkimplausible.c"
"$T/mkimplausible" "$T/imp"

# The fixture is refused for an ordinary operation, so the pass below is narrow.
( cd "$T" && "$BIN/macho9" lc imp imp.lc -delete uuid ) >/dev/null 2>"$T/imperr"
[ $? -ne 0 ] && grep -q 'no known function' "$T/imperr" \
    && ok "rename_segment: the fixture really is one the gate rejects for other operations" \
    || bad "rename_segment mg_plausible" "lc -delete uuid was not refused: $(cat "$T/imperr")"

cp "$T/imp" "$T/v"
before=$(sha "$T/v")
run rename_segment v __DATA __DATA_R9
[ "$rc" -eq 0 ] && grep -qxF 'v: renamed 1 segment(s) __DATA -> __DATA_R9' "$T/out" \
    && [ "$(sha "$T/v")" != "$before" ] \
    && ok "rename_segment: renames a binary mg_plausible rejects for other operations" \
    || bad "rename_segment mg_plausible" "exit $rc: $(cat "$T/err")"

# The NEW-name length check and the arity check happen before any I/O, in
# rename_segment's own words -- both come from compat/translate.sh.
fresh
run rename_segment f __DATA 12345678901234567
[ "$rc" -eq 1 ] && firstline_is "$T/err" 'new segment name longer than 16 bytes' \
    && ok "rename_segment: a 17-byte NEW name is refused before any I/O" \
    || bad "rename_segment long name" "exit $rc, stderr: $(head -1 "$T/err")"
fresh
run rename_segment f __DATA
[ "$rc" -eq 1 ] && firstline_is "$T/err" "Usage: $BIN/rename_segment binary OLDNAME NEWNAME" \
    && ok "rename_segment: wrong arity is a usage error naming argv[0]" \
    || bad "rename_segment arity" "exit $rc, stderr: $(head -1 "$T/err")"

# An absent file fails before anything else, as the C tool's early O_RDWR did.
run rename_segment nosuchfile __DATA __X
[ "$rc" -eq 1 ] \
    && ok "rename_segment: an absent file exits 1" \
    || bad "rename_segment absent" "exit $rc, want 1"

# ---- retag_swift_classes ------------------------------------------------
#
# The variadic one. Its two messages and its had_error exit are rebuilt by the
# wrapper, because a single-file verb has nothing to say about a total and
# prints its per-file line even for a count of zero.
fresh
run retag_swift_classes f
[ "$rc" -eq 0 ] && grep -qxF 'total: 0 class record(s) retagged' "$T/out" \
    && ok "retag_swift_classes: prints the total line the single-file verb has no notion of" \
    || bad "retag_swift_classes total" "exit $rc, stdout: $(cat "$T/out")"
grep -q ': retagged 0 class record(s)' "$T/out" \
    && bad "retag_swift_classes" "printed a per-file line for a zero count, which the C tool did not" \
    || ok "retag_swift_classes: no per-file line for a zero count, as before"

# A NON-Mach-O argument was a silent skip: no message, no error flag, and the
# loop kept going. macho9 refuses it with EX_REFUSED and says so, so the
# wrapper has to swallow both. Three rows of tests/compat-matrix.tsv are this
# case.
fresh
printf 'not a mach-o at all\n' > "$T/nm"
run retag_swift_classes f nm f
[ "$rc" -eq 0 ] && grep -qxF 'total: 0 class record(s) retagged' "$T/out" \
    && ok "retag_swift_classes: a non-Mach-O argument is skipped, and the loop continues" \
    || bad "retag_swift_classes skip" "exit $rc, stdout: $(cat "$T/out")"
grep -q 'not a readable 64-bit Mach-O' "$T/err" \
    && bad "retag_swift_classes skip" "macho9's refusal for the skipped file leaked to stderr" \
    || ok "retag_swift_classes: the skip is silent, as it always was"

# A REAL failure (an absent path) sets had_error, prints the underlying
# diagnostic, and still prints the total. tests/leaf-tool-crashes.sh asserts
# the exit code of this exact shape.
fresh
run retag_swift_classes f nosuchfile
[ "$rc" -eq 1 ] && grep -qxF 'total: 0 class record(s) retagged' "$T/out" \
    && ok "retag_swift_classes: an absent path exits 1 and still prints the total" \
    || bad "retag_swift_classes error" "exit $rc, stdout: $(cat "$T/out")"
grep -q 'No such file or directory' "$T/err" \
    && ok "retag_swift_classes: the underlying diagnostic still reaches stderr" \
    || bad "retag_swift_classes error" "stderr: $(cat "$T/err")"

run retag_swift_classes
[ "$rc" -eq 1 ] && firstline_is "$T/err" "Usage: $BIN/retag_swift_classes binary [binary ...]" \
    && ok "retag_swift_classes: no argument is a usage error naming argv[0]" \
    || bad "retag_swift_classes usage" "exit $rc, stderr: $(head -1 "$T/err")"

# EVERY assertion above ran on `f` (tests/fixture.macho), which has ZERO Swift
# class records -- so "total: 0" is the only total ever asserted, the
# per-file line is only ever asserted ABSENT, and no assertion above ever
# observed an INSTALL happen at all. A wrapper whose mw_finish discarded
# every temp instead of installing it -- printing every line above
# correctly, having modified not one binary -- would pass every one of them
# unchanged. mkswift_fixture (tests/mkswift.c, shared with cli_test.sh) gives
# this suite a binary with real Swift class records, closing that gap.

# A nonzero-count binary really gets retagged: bytes change, the per-file
# line is printed with the real count, and it sums into total.
mkswift_fixture "$T/rsc1"
rsc1_before=$(sha "$T/rsc1")
run retag_swift_classes rsc1
[ "$rc" -eq 0 ] && grep -qxF 'rsc1: retagged 2 class record(s)' "$T/out" \
    && grep -qxF 'total: 2 class record(s) retagged' "$T/out" \
    && ok "retag_swift_classes: a nonzero-count binary prints its own line and the right total" \
    || bad "retag_swift_classes nonzero" "exit $rc, stdout: $(cat "$T/out")"
[ "$(sha "$T/rsc1")" != "$rsc1_before" ] \
    && ok "retag_swift_classes: ... and its bytes really changed (this was not a discarded no-op)" \
    || bad "retag_swift_classes nonzero" "rsc1's bytes did not change"

# A mixed run good/hard-linked/good: the hard-linked argument is refused (the
# new divergence this task's install step introduces -- the C tool wrote
# through the open fd regardless of hard links; this wrapper installs via mv,
# which cannot update every name for an inode at once), the loop keeps going,
# exit is 1, stdout is the two good binaries' lines plus the REDUCED total,
# and the hard-linked target -- and its link, same inode -- are untouched.
mkswift_fixture "$T/rsc_g1"
mkswift_fixture "$T/rsc_h1"; ln "$T/rsc_h1" "$T/rsc_h2"
mkswift_fixture "$T/rsc_g2"
rsc_h1_before=$(sha "$T/rsc_h1"); rsc_h1_ino=$(stat -f %i "$T/rsc_h1")
run retag_swift_classes rsc_g1 rsc_h1 rsc_g2
[ "$rc" -eq 1 ] && grep -qxF 'rsc_g1: retagged 2 class record(s)' "$T/out" \
    && grep -qxF 'rsc_g2: retagged 2 class record(s)' "$T/out" \
    && grep -qxF 'total: 4 class record(s) retagged' "$T/out" \
    && ! grep -q 'rsc_h1' "$T/out" \
    && ok "retag_swift_classes: good/hardlinked/good -- exit 1, the two good lines, and the reduced total" \
    || bad "retag_swift_classes hardlink mix" "exit $rc, stdout: $(cat "$T/out")"
grep -q 'hard link' "$T/err" \
    && ok "retag_swift_classes: ... and says why the hard-linked one was skipped" \
    || bad "retag_swift_classes hardlink mix" "no hard-link explanation on stderr: $(cat "$T/err")"
[ "$(sha "$T/rsc_h1")" = "$rsc_h1_before" ] && [ "$(stat -f %i "$T/rsc_h1")" = "$rsc_h1_ino" ] \
    && [ "$(sha "$T/rsc_h2")" = "$rsc_h1_before" ] \
    && ok "retag_swift_classes: ... the hard-linked target AND its link are untouched" \
    || bad "retag_swift_classes hardlink mix" "rsc_h1 or rsc_h2 changed"

# A mixed run good/unwritable/good: same shape, a different wrapper-level
# refusal (mw_require_writable, same words change_dylib's own guard uses).
mkswift_fixture "$T/rsc_g3"
mkswift_fixture "$T/rsc_u"; chmod 444 "$T/rsc_u"
mkswift_fixture "$T/rsc_g4"
rsc_u_before=$(sha "$T/rsc_u")
run retag_swift_classes rsc_g3 rsc_u rsc_g4
chmod 644 "$T/rsc_u"
[ "$rc" -eq 1 ] && grep -qxF 'rsc_g3: retagged 2 class record(s)' "$T/out" \
    && grep -qxF 'rsc_g4: retagged 2 class record(s)' "$T/out" \
    && grep -qxF 'total: 4 class record(s) retagged' "$T/out" \
    && ! grep -q 'rsc_u' "$T/out" \
    && ok "retag_swift_classes: good/unwritable/good -- exit 1, the two good lines, and the reduced total" \
    || bad "retag_swift_classes unwritable mix" "exit $rc, stdout: $(cat "$T/out")"
[ "$(sha "$T/rsc_u")" = "$rsc_u_before" ] \
    && ok "retag_swift_classes: ... the unwritable one is untouched" \
    || bad "retag_swift_classes unwritable mix" "rsc_u changed"

# No `.*.macho9-compat.$$` temp survives either mid-loop refusal above.
ls -a "$T" | grep -q 'macho9-compat' && bad "retag_swift_classes" "a temp file was left behind" \
    || ok "retag_swift_classes: no temp file left behind after a mid-loop refusal"

# A 0-count binary AMONG nonzero ones: mw_finish discards its temp rather
# than installing an identical copy, so its INODE (not just its bytes, which
# cannot tell the two apart) is unchanged -- while the good binaries around
# it still count. `f` (tests/fixture.macho) has zero Swift class records.
fresh
mkswift_fixture "$T/rsc_g5"
mkswift_fixture "$T/rsc_g6"
f_ino=$(stat -f %i "$T/f"); f_before=$(sha "$T/f")
run retag_swift_classes rsc_g5 f rsc_g6
[ "$rc" -eq 0 ] && grep -qxF 'total: 4 class record(s) retagged' "$T/out" \
    && ! grep -q '^f: retagged' "$T/out" \
    && ok "retag_swift_classes: a 0-count binary among nonzero ones prints no line of its own, and the total excludes it" \
    || bad "retag_swift_classes 0-count mix" "exit $rc, stdout: $(cat "$T/out")"
[ "$(stat -f %i "$T/f")" = "$f_ino" ] && [ "$(sha "$T/f")" = "$f_before" ] \
    && ok "retag_swift_classes: ... and its INODE is unchanged (discarded, not reinstalled)" \
    || bad "retag_swift_classes 0-count mix" "f's inode or bytes changed on a 0-count run"

# A symlinked argument stays a symlink; its target is what actually changes.
mkswift_fixture "$T/rsc_real"
ln -s rsc_real "$T/rsc_link"
rsc_real_before=$(sha "$T/rsc_real")
run retag_swift_classes rsc_link
[ "$rc" -eq 0 ] && grep -qxF 'rsc_link: retagged 2 class record(s)' "$T/out" \
    && ok "retag_swift_classes: a symlinked argument is retagged through the link" \
    || bad "retag_swift_classes symlink" "exit $rc, stdout: $(cat "$T/out")"
[ -L "$T/rsc_link" ] \
    && ok "retag_swift_classes: ... which is still a symlink afterward" \
    || bad "retag_swift_classes symlink" "rsc_link is no longer a symlink"
[ "$(sha "$T/rsc_real")" != "$rsc_real_before" ] \
    && ok "retag_swift_classes: ... and its target is what actually got the new bytes" \
    || bad "retag_swift_classes symlink" "rsc_real's bytes did not change"

# MEASURED against the mutation this suite exists to catch: with
# macho9-compat.sh's mw_finish changed to discard every temp unconditionally
# (install NOTHING, as if nothing ever differed), stdout is untouched --
# rsc1 still prints "rsc1: retagged 2 class record(s)" and "total: 2 ..." --
# so the two assertions above that check ONLY stdout or an untouched-file's
# bytes would still pass. What actually fails: "... its bytes really changed"
# (rsc1) and "... its target is what actually got the new bytes" (rsc_real),
# because those are the two that check a byte or an inode that was supposed
# to MOVE, not stay put. Not left staged here as a live test, because that
# would mean shipping a second, deliberately-broken copy of mw_finish just to
# exercise it.

# ---- fix_macho ----------------------------------------------------------
#
# The last tool to become a wrapper, and the only one whose wrapper does NOT
# close its divergences: the repo owner ruled five of them improvements to
# ADOPT. compat/fix_macho.sh's header states all five with their reasons. This
# block asserts each of the three flags it accepts, a fat container (its
# headline capability, and the one thing change_dylib could not do), and the
# two adopted changes that used to be REFUSALS -- a longer replacement path
# and a chained -rename_seg. Both of those were measured against the
# pre-wrapper C binary and recorded in tests/compat-matrix.tsv as differences;
# they are now the expected behaviour, and these are the assertions that say
# so out loud. The fifth adopted change -- -change no longer rewriting a
# dylib's own LC_ID_DYLIB -- has its own assertion further down.

# -change, the flag with the most reach. Byte-identical to the same operation
# through macho9 itself, which is the same shape the change_dylib block above
# asserts and for the same reason: one emitted command is one mr_apply_file
# pass over the same file with the same ops.
fresh
run fix_macho f -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib'
fmrc=$rc
cp "$T/out" "$T/fm.out"
fmsha=$(sha "$T/f")
fresh
( cd "$T" && "$BIN/macho9" dylib f f.m9out -replace /usr/lib/libSystem.B.dylib \
    '@loader_path/../S.dylib' ) >"$T/m9.out" 2>/dev/null
# The same one-line reshape the change_dylib block above explains.
sed 's|^Wrote f\.m9out (|Updated f (|' "$T/m9.out" >"$T/m9.want"
[ "$fmrc" -eq 0 ] && cmp -s "$T/fm.out" "$T/m9.want" && [ "$fmsha" = "$(sha "$T/f.m9out")" ] \
    && ok "fix_macho: -change is byte-identical to macho9 dylib -replace, stdout included" \
    || bad "fix_macho -change" "exit $fmrc; stdout or bytes differ from macho9 dylib's"

# -rename_seg, which fix_macho's own usage line never mentioned even though
# its parser always accepted it. One `macho9 segment` pass per pair.
fresh
before=$(sha "$T/f")
run fix_macho f -rename_seg __DATA __DATA_F1
[ "$rc" -eq 0 ] && [ "$(sha "$T/f")" != "$before" ] \
    && ( cd "$T" && "$BIN/macho9" info f ) 2>/dev/null | grep -q '__DATA_F1' \
    && ok "fix_macho: -rename_seg renames the segment" \
    || bad "fix_macho -rename_seg" "exit $rc: $(cat "$T/err")"

# -strip_build_version. tests/fixture.macho is a real 10.9 binary and carries
# no LC_BUILD_VERSION (the load command postdates it by four years), so this
# asserts the OTHER half, which is the half a caller depends on: the emitted
# command is the right one, the operation that matched nothing SAYS SO on
# stderr -- this plan's Task 1 report, which is what replaced fix_macho's
# "No changes needed: F" -- and the exit code is still 0.
#
# THAT LAST PART IS A GATE, not a detail. Task 2 added `--fatal-warnings`,
# which turns that report into a refusal. This wrapper must never pass it:
# fix_macho exited 0 when an operation matched nothing, and that is compat
# surface. An exit of 2 here means the flag leaked into the translation.
fresh
before=$(sha "$T/f")
run fix_macho f -strip_build_version
[ "$rc" -eq 0 ] && [ "$(sha "$T/f")" = "$before" ] \
    && ok "fix_macho: -strip_build_version with nothing to strip exits 0, having written nothing" \
    || bad "fix_macho -strip_build_version" "exit $rc (want 0), file changed=$([ "$(sha "$T/f")" = "$before" ] && echo no || echo YES)"
has_line "$T/err" 'macho9: no load command of kind build-version to delete' \
    && ok "fix_macho: an operation that matched nothing says so on stderr" \
    || bad "fix_macho unmatched report" "stderr: $(cat "$T/err")"
has_line "$T/err" '    macho9 lc f f.new -delete build-version' \
    && ok "fix_macho: -strip_build_version translates to lc -delete build-version" \
    || bad "fix_macho -strip_build_version translation" "stderr: $(cat "$T/err")"

# THE FIFTH DIVERGENCE: -change AIMED AT THIS DYLIB'S OWN INSTALL NAME.
# compat/fix_macho.c's match block opened on `mo_is_ordinal_lc(lc->cmd) ||
# lc->cmd == LC_ID_DYLIB` and then ran the changes[] comparison loop with NO
# LC_ID_DYLIB exclusion -- so `-change <this dylib's own install name> NEW`
# rewrote the dylib's identity, even though the file's own comment claimed
# "nothing in changes is ever meant to match it". src/rewrite.c enforces that
# comment as code now (`if (lc->cmd != LC_ID_DYLIB) { /* never rewrite this
# dylib's own identity */`), matching what install_name_tool does: -id, never
# -change, is the flag that ever touches LC_ID_DYLIB. The repo owner ruled
# this the fifth divergence to ADOPT; compat/fix_macho.sh's header states it,
# with its reasons.
#
# tests/fixture.macho is an EXECUTABLE and carries no LC_ID_DYLIB at all, so
# this needs its own fixture: a tiny dylib, built here the same way
# tests/change_dylib_test.sh builds its dylib fixtures (`$CC -dynamiclib
# -install_name ...`), with its own install name AND a real dependency, so
# one run asserts both halves at once -- the guard holds on the identity,
# and a -change aimed at a real dependency in the SAME invocation still
# lands, so this pins the guard rather than "fix_macho does nothing to
# dylibs".
fm_id="@loader_path/libfmid.dylib"
"$CC" -dynamiclib -O2 -mmacosx-version-min=10.9 -install_name "$fm_id" \
    -x c - -o "$T/libfmid.dylib" <<'EOF'
int fmid_dummy(void) { return 0; }
EOF
run fix_macho libfmid.dylib -change "$fm_id" '@loader_path/OTHER.dylib' \
    -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib'
if [ "$rc" -eq 0 ] \
    && has_line "$T/err" "macho9: $fm_id matched nothing" \
    && LC_ALL=C grep -q -- "$fm_id" "$T/libfmid.dylib" \
    && ! LC_ALL=C grep -q -- '@loader_path/OTHER.dylib' "$T/libfmid.dylib" \
    && LC_ALL=C grep -q -- '@loader_path/../S.dylib' "$T/libfmid.dylib"; then
    ok "fix_macho: -change at a dylib's own install name leaves LC_ID_DYLIB unchanged, reported unmatched, while a real dependency's -change in the same run still lands"
else
    bad "fix_macho -change own id" "exit $rc; stderr: $(cat "$T/err")"
fi

# ADOPTED CHANGE 1: A REPLACEMENT PATH LONGER THAN THE EXISTING COMMAND.
# compat/fix_macho.c wrote the new path INTO the existing LC_LOAD_DYLIB and
# refused when it did not fit ("new path '...' too long (320 > 32)", exit 1,
# file untouched -- a measured row of tests/compat-matrix.tsv). `macho9 dylib
# -replace` resizes the command into header pad the image already has, so this
# now succeeds. No --allow-grow is emitted; this uses existing pad only.
fresh
fm_long="@loader_path/"
i=0
while [ $i -lt 30 ]; do fm_long="${fm_long}longlongl"; i=$((i + 1)); done
fm_long="${fm_long}.dylib"
run fix_macho f -change /usr/lib/libSystem.B.dylib "$fm_long"
# A raw byte search over the rewritten file, NOT `otool -L`: tests/README.md's
# second lesson. grep's own "Binary file matches" chatter is irrelevant under
# -q, which reports only through its exit status.
if [ "$rc" -eq 0 ] && LC_ALL=C grep -q -- "$fm_long" "$T/f"; then
    ok "fix_macho: a longer replacement path is now rewritten into header pad, not refused"
else
    bad "fix_macho long path" "exit $rc (want 0); the 289-byte replacement did not land: $(cat "$T/err")"
fi

# ADOPTED CHANGE 2: A CHAINED -rename_seg NOW CHAINS. fix_macho applied every
# pair in ONE pass and gave each segment its FIRST match, so `-rename_seg
# __DATA __X -rename_seg __X __Y` ended at __X and the second pair never
# fired. Two `macho9 segment` passes chain, so it ends at __Y. Asserted on
# BOTH names: __Y present is the new behaviour, __X absent is what rules out
# the old one still happening.
fresh
run fix_macho f -rename_seg __DATA __X -rename_seg __X __Y
fm_names=$( ( cd "$T" && "$BIN/macho9" info f ) 2>/dev/null )
if [ "$rc" -eq 0 ] \
    && printf '%s\n' "$fm_names" | grep -q 'segname=__Y' \
    && ! printf '%s\n' "$fm_names" | grep -q 'segname=__X'; then
    ok "fix_macho: a chained -rename_seg now produces the SECOND name, not the first"
else
    bad "fix_macho chained rename" "exit $rc; segnames: $(printf '%s\n' "$fm_names" | sed -n 's/.*\(segname=__[XY]\).*/\1/p' | tr '\n' ' ')"
fi

# A FAT CONTAINER. This is fix_macho's headline capability -- it is the reason
# the tool existed alongside change_dylib, which understood only thin files
# until the shared rewriter gave both the same fat loop.
#
# The container is BUILT HERE, in shell, from tests/fixture.macho rather than
# found by scanning the host. tests/README.md records why: a test that scanned
# for a suitable binary shipped zero coverage on the cross runner. The bytes
# are the on-disk fat convention (big-endian fat_header/fat_arch), written by
# construction, not by detection -- the same choice tests/change_dylib_test.sh
# made when it built makefat instead of calling lipo.
fm_be32() {
    printf '%b' "$(printf '\\0%o\\0%o\\0%o\\0%o' \
        $((($1 >> 24) & 255)) $((($1 >> 16) & 255)) $((($1 >> 8) & 255)) $(($1 & 255)))"
}
# fm_mkfat OUT SLICE0 CPUTYPE0 [SLICE1 CPUTYPE1] -- slices at 4096-aligned
# offsets, in the order given.
fm_mkfat() {
    fm_out=$1 fm_s0=$2 fm_ct0=$3 fm_s1=${4:-} fm_ct1=${5:-}
    fm_z0=$(wc -c < "$fm_s0" | tr -d ' ')
    fm_n=1; [ -n "$fm_s1" ] && fm_n=2
    fm_o0=4096
    {
        printf '%b' '\0312\0376\0272\0276'     # FAT_MAGIC, big-endian on disk
        fm_be32 "$fm_n"
        fm_be32 "$fm_ct0"; fm_be32 3; fm_be32 "$fm_o0"; fm_be32 "$fm_z0"; fm_be32 12
        if [ "$fm_n" -eq 2 ]; then
            fm_z1=$(wc -c < "$fm_s1" | tr -d ' ')
            fm_o1=$(( (fm_o0 + fm_z0 + 4095) / 4096 * 4096 ))
            fm_be32 "$fm_ct1"; fm_be32 3; fm_be32 "$fm_o1"; fm_be32 "$fm_z1"; fm_be32 12
        fi
        dd if=/dev/zero bs=1 count=$(( fm_o0 - 8 - 20 * fm_n )) 2>/dev/null
        cat "$fm_s0"
        if [ "$fm_n" -eq 2 ]; then
            dd if=/dev/zero bs=1 count=$(( fm_o1 - fm_o0 - fm_z0 )) 2>/dev/null
            cat "$fm_s1"
        fi
    } > "$fm_out"
}
# 0x01000007 is CPU_TYPE_X86_64, which is what tests/fixture.macho really is.
fm_mkfat "$T/fat1" "$FIXTURE" 16777223
case $(od -An -tx1 -N4 "$T/fat1" | tr -d ' ') in
    cafebabe) ok "fix_macho: the hand-built fat container really is one" ;;
    *) bad "fix_macho fat fixture" "magic is $(od -An -tx1 -N4 "$T/fat1" | tr -d ' '), not cafebabe" ;;
esac
cp "$T/fat1" "$T/fatf"
run fix_macho fatf -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib'
if [ "$rc" -eq 0 ] && LC_ALL=C grep -q -- '@loader_path/../S.dylib' "$T/fatf" \
    && ! grep -q 'matched nothing' "$T/err"; then
    ok "fix_macho: rewrites inside a fat container, which is why this tool existed"
else
    bad "fix_macho fat" "exit $rc: $(cat "$T/err")"
fi

# A SLICE THAT IS NOT A 64-BIT MACH-O IS LEFT ALONE, and the rest of the file
# is still rewritten. This is NOT one of the five adopted changes: fix_macho
# printed "  Skipping arch N" and carried on, and mr_process_fat's MR_SKIP path
# does the same thing with a different message. Measured, not assumed -- the
# plan's table describes the fat divergence as "refuses the whole file", which
# is true only of MR_ERROR (a slice that IS a 64-bit Mach-O whose edit failed),
# not of a slice that simply is not one. Asserting the SKIP is what keeps that
# distinction from being quietly widened later.
#
# 0x00000007 is CPU_TYPE_I386; the slice's bytes are filler, not a Mach-O.
dd if=/dev/zero bs=1 count=4096 2>/dev/null | tr '\000' 'Z' > "$T/junkslice"
fm_mkfat "$T/fat2" "$FIXTURE" 16777223 "$T/junkslice" 7
cp "$T/fat2" "$T/fatg"
run fix_macho fatg -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib'
if [ "$rc" -eq 0 ] && grep -q 'not a 64-bit Mach-O; leaving this slice unchanged' "$T/out" \
    && LC_ALL=C grep -q -- '@loader_path/../S.dylib' "$T/fatg"; then
    ok "fix_macho: a non-64-bit slice is left unchanged and the other slice is still rewritten"
else
    bad "fix_macho fat skip" "exit $rc; stdout: $(cat "$T/out")"
fi

# THE CAPACITY CAPS, in fix_macho's own words. Both moved into
# compat/translate.sh when compat/fix_macho.c retired, and the -rename_seg one
# has no macho9 counterpart at all -- each pair is its own `macho9 segment`
# invocation, so nothing downstream would ever count them. This is the
# assertion that the message a caller sees is still fix_macho's.
fresh
i=0; fm_chg33=''
while [ $i -lt 33 ]; do fm_chg33="$fm_chg33 -change A B"; i=$((i + 1)); done
# shellcheck disable=SC2086
run fix_macho f $fm_chg33
[ "$rc" -eq 1 ] && grep -qxF 'too many -change (max 32)' "$T/err" \
    && [ "$(sha "$T/f")" = "$(sha "$FIXTURE")" ] \
    && ok "fix_macho: the -change cap refuses in fix_macho's own words, before touching the file" \
    || bad "fix_macho -change cap" "exit $rc, stderr: $(cat "$T/err")"

fresh
i=0; fm_seg17=''
while [ $i -lt 17 ]; do fm_seg17="$fm_seg17 -rename_seg __A __B"; i=$((i + 1)); done
# shellcheck disable=SC2086
run fix_macho f $fm_seg17
[ "$rc" -eq 1 ] && grep -qxF 'too many -rename_seg (max 16)' "$T/err" \
    && [ "$(sha "$T/f")" = "$(sha "$FIXTURE")" ] \
    && ok "fix_macho: the -rename_seg cap refuses, and nothing downstream would have" \
    || bad "fix_macho -rename_seg cap" "exit $rc, stderr: $(cat "$T/err")"

# Usage and refusals, in fix_macho's own words, naming argv[0] where it did.
run fix_macho f
[ "$rc" -eq 1 ] && firstline_is "$T/err" "Usage: $BIN/fix_macho <file> [-change old new] [-strip_build_version]" \
    && ok "fix_macho: too few arguments is a usage error naming argv[0]" \
    || bad "fix_macho usage" "exit $rc, stderr: $(head -1 "$T/err")"
fresh
run fix_macho f -nope
[ "$rc" -eq 1 ] && grep -qxF 'Unknown option: -nope' "$T/err" \
    && ok "fix_macho: an unknown flag refuses in fix_macho's own words" \
    || bad "fix_macho unknown flag" "exit $rc, stderr: $(cat "$T/err")"
fresh
run fix_macho f -rename_seg __DATA 12345678901234567
[ "$rc" -eq 1 ] && grep -qxF 'new segment name longer than 16 bytes: 12345678901234567' "$T/err" \
    && ok "fix_macho: a 17-byte NEW segment name is refused before any I/O" \
    || bad "fix_macho long segname" "exit $rc, stderr: $(head -1 "$T/err")"

# An absent file, and an unwritable one: fix_macho opened O_RDWR before it
# looked at anything, so both failed immediately with perror("open"). An
# invocation that emits one of mr_apply_file's verbs gets that from its own
# O_RDWR; one that emits `macho9 edit` does not, because me_run reads the
# image O_RDONLY and only finds out it cannot write at the END of the run --
# which is why the wrapper checks for itself, and why both cases below are
# MULTI-command.
#
# BOTH CASES DISCRIMINATE NOW, and the unwritable one more sharply than
# before. Remove the wrapper's check and the absent file reports macho9's
# "macho9 edit: nosuchfile: cannot open or read" instead of fix_macho's own
# words; the unwritable one SUCCEEDS -- measured -- because
# wa_write_atomic mkstemps beside the file and renames over it, which needs
# the DIRECTORY to be writable and not the file, so a mode-444 binary is
# replaced (new inode, mode 444 carried over) and the run exits 0 where
# fix_macho's O_RDWR refused. The check below is the only thing standing
# between a caller and that silent rewrite. Each was mutation-tested.
run fix_macho nosuchfile -strip_build_version -change A B
[ "$rc" -eq 1 ] && has_line "$T/err" 'open: No such file or directory' \
    && ok "fix_macho: an absent file fails immediately, in fix_macho's own words" \
    || bad "fix_macho absent" "exit $rc, stderr: $(cat "$T/err")"
fresh
chmod 444 "$T/f"
before=$(sha "$T/f")
run fix_macho f -strip_build_version -change A B
fm_ro_rc=$rc
chmod 644 "$T/f"
[ "$fm_ro_rc" -eq 1 ] && has_line "$T/err" 'open: Permission denied' \
    && [ "$(sha "$T/f")" = "$before" ] \
    && ok "fix_macho: an unwritable file fails before the multi-command rewrite runs" \
    || bad "fix_macho unwritable" "exit $fm_ro_rc, stderr: $(cat "$T/err")"

# ---- hostile argv shapes -----------------------------------------------
#
# A path with a SPACE, a path with a LEADING DASH, and an EMPTY string. All
# three are shapes the wrappers were fixed for -- translate.sh's mt_quote does
# the quoting, for the verb form and for the edit script's statements alike
# (src/script.c's ms_split reads a statement's words by a shell's rules, which
# is why one quoting serves both) -- and none of them was covered, so the
# fixes could have regressed silently. The reviewer verified all three against
# the pre-wrapper binaries; these keep them verified.

# A SPACE in the file name, on the mixed-family path -- the one that emits
# `macho9 edit FILE -`, so the path is quoted into an edit command line rather
# than a verb's. Asserted by comparing against the same operations on an
# ordinarily-named copy.
fresh
cp "$FIXTURE" "$T/has space"
( cd "$T" && "$BIN/change_dylib" "has space" -strip-lc uuid \
    -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib' ) >/dev/null 2>"$T/err"
rc=$?
( cd "$T" && "$BIN/change_dylib" f -strip-lc uuid \
    -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib' ) >/dev/null 2>&1
[ "$rc" -eq 0 ] && cmp -s "$T/has space" "$T/f" \
    && ok "change_dylib: a file name with a space rewrites identically" \
    || bad "change_dylib spaced path" "exit $rc: $(cat "$T/err")"
rm -f "$T/has space"

# A LEADING DASH. Every one of these tools took argv[1] as a path
# unconditionally, so `-dashy` is a file name, not an option.
fresh
cp "$FIXTURE" "$T/-dashy"
before=$(sha "$T/-dashy")
( cd "$T" && "$BIN/change_dylib" -dashy -strip-lc uuid ) >/dev/null 2>"$T/err"
rc=$?
[ "$rc" -eq 0 ] && [ "$(sha "$T/-dashy")" != "$before" ] \
    && ok "change_dylib: a file name starting with a dash is a file name" \
    || bad "change_dylib leading dash" "exit $rc: $(cat "$T/err")"
# ...and on the mixed-family path, where the name reaches `macho9 edit` as its
# FILE positional. That is its own guard: `edit` is the one verb with flags to
# scan past, and cli/macho9.c's parser takes a single-dash token as a file name
# for exactly this reason -- rejecting it made this case fail the moment the
# wrappers started emitting `edit`. Compared against the SAME operations on an
# ordinarily-named copy, so both sides are rewritten here rather than relying
# on whatever $T/f happens to hold.
fresh
cp "$FIXTURE" "$T/-dashy"
( cd "$T" && "$BIN/change_dylib" -dashy -strip-lc uuid \
    -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib' ) >/dev/null 2>"$T/err"
rc=$?
( cd "$T" && "$BIN/change_dylib" f -strip-lc uuid \
    -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib' ) >/dev/null 2>&1
[ "$rc" -eq 0 ] && cmp -s "$T/-dashy" "$T/f" \
    && ok "change_dylib: and on the multi-command path, where it reaches macho9 edit as a positional" \
    || bad "change_dylib leading dash, mixed" "exit $rc: $(cat "$T/err")"
rm -f "$T/-dashy"

# The same leading-dash shape for fix_macho, on its single-command path --
# one -change, so one `macho9 dylib` line. What is at stake: FILE reaches
# cmd_dylib_or_rpath as argv[2], read positionally, never scanned for a
# leading dash the way an option would be -- so `$1` passing through
# mt_translate unexamined is the guard this pins, same file-not-option
# question as change_dylib's case above. -change, not -strip_build_version,
# because tests/fixture.macho carries no LC_BUILD_VERSION to strip (see
# below) and a no-op would not tell the dash apart from a typo.
fresh
cp "$FIXTURE" "$T/-dashy"
before=$(sha "$T/-dashy")
run fix_macho -dashy -change /usr/lib/libSystem.B.dylib '@loader_path/../S.dylib'
[ "$rc" -eq 0 ] && [ "$(sha "$T/-dashy")" != "$before" ] \
    && ok "fix_macho: a file name starting with a dash is a file name" \
    || bad "fix_macho leading dash" "exit $rc: $(cat "$T/err")"
rm -f "$T/-dashy"

# An EMPTY NEW segment name: legal (a segname may be all NULs) and matched by
# tests/compat-matrix.tsv's `rename_segment f __DATA ''` row, whose C-side
# output was "f: renamed 1 segment(s) __DATA -> " with the trailing space.
fresh
run rename_segment f __DATA ''
[ "$rc" -eq 0 ] && grep -qxF 'f: renamed 1 segment(s) __DATA -> ' "$T/out" \
    && ok "rename_segment: an empty NEW name is accepted, and printed as empty" \
    || bad "rename_segment empty NEW" "exit $rc, stdout: [$(cat "$T/out")]"

# An EMPTY file name reaches open() as "" and fails there, on both sides.
run change_dylib '' -strip-lc uuid
[ "$rc" -ne 0 ] \
    && ok "change_dylib: an empty file name fails rather than acting on something else" \
    || bad "change_dylib empty path" "exit 0"

# A SPACE in a retag_swift_classes argument, which is variadic -- so the space
# must not split one file into two.
fresh
cp "$FIXTURE" "$T/two words"
( cd "$T" && "$BIN/retag_swift_classes" "two words" f ) >"$T/out" 2>"$T/err"
rc=$?
[ "$rc" -eq 0 ] && grep -qxF 'total: 0 class record(s) retagged' "$T/out" \
    && [ "$(wc -l < "$T/out" | tr -d ' ')" = 1 ] \
    && ok "retag_swift_classes: a spaced argument stays one file" \
    || bad "retag_swift_classes spaced path" "exit $rc, stdout: $(cat "$T/out")"
rm -f "$T/two words"

# ---- the emitted grammar is one this build actually has -----------------
#
# Same check tests/translate_test.sh makes of the translator, made here of the
# wrappers: every verb a wrapper can reach must be one this macho9 advertises.
# Hardcoding that agreement is how the ops=/kinds= lists in cli/macho9.c
# drifted from their own parsers once already.
"$BIN/macho9" --capabilities > "$T/caps" 2>/dev/null
for v in declassify minos segment retag-swift lc dylib rpath; do
    grep -q "^verb $v" "$T/caps" \
        && ok "capabilities: this build advertises $v" \
        || bad "capabilities" "$v is not advertised, but a wrapper emits it"
done

echo "wrapper_test: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0
