#!/bin/sh
# tests/translate_test.sh -- one test per translation, asserting the EXACT
# text compat/translate.sh emits: a verb command line, or -- when one old
# invocation is worth more than one macho9 command -- the whole
# `macho9 edit FILE OUT -` here-document, terminator included.
#
#   sh tests/translate_test.sh <bindir>
#
# Every line compat/translate.sh prints is a CLAIM that an old-grammar
# invocation and a macho9 one mean the same thing. docs/PROPOSAL.md rejected
# the old spellings as macho9 synonyms on purpose, "because they would
# advertise an interchangeability that does not exist, on exactly the binaries
# where it does not hold" -- so each claim gets a test, and the test pins the
# whole command line, not just that something was printed. A translation that
# emitted `dylib -append` where `-insert` was meant would still "work" and
# would still be wrong (an appended LC_LOAD_DYLIB gets the highest library
# ordinal; an inserted one gets ordinal 1).
#
# WHAT THIS DOES NOT DO: it never runs macho9 on a file. Whether the emitted
# commands PRODUCE the same bytes as the old tool is a different question, and
# tests/compat-sweep.sh answers it over 1200 combinations against real
# binaries. This test is about the text.
#
# The bindir is used for exactly one thing: `macho9 --capabilities`. The spec's
# "Migration" section says that probe exists so the wrapper and the binary need
# not move in lockstep, and the last check below is what actually uses it --
# every verb, op and KIND this translator can emit has to be one this build
# advertises. Hardcoding that agreement instead of checking it is how the
# ops=/kinds= lists in cli/macho9.c drifted from their own parsers once already.
set -u

BIN="${1:?usage: translate_test.sh <bindir>}"
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
TR="$ROOT/compat/translate.sh"
[ -r "$TR" ] || { echo "translate_test: $TR missing" >&2; exit 1; }
[ -x "$BIN/machotool" ] || { echo "translate_test: $BIN/machotool missing" >&2; exit 1; }

T=$(mktemp -d "${TMPDIR:-/tmp}/macho-translate-test.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT INT TERM

pass=0; fail=0

# ok <name> <expected-stdout> -- TOOL ARG...
#
# Runs the translator as a separate process, exactly as a caller would, and
# compares its whole stdout against the expected text (empty means "no lines").
# Exit status must be 0.
ok() {
    name=$1; want=$2; shift 3   # the third argument is the literal --
    got=$( /bin/sh "$TR" "$@" 2>"$T/err" ); rc=$?
    if [ "$rc" -ne 0 ]; then
        printf 'FAIL %s: exit %d, expected 0\n  stderr: %s\n' "$name" "$rc" "$(cat "$T/err")" >&2
        fail=$((fail + 1)); return 0
    fi
    if [ "$got" != "$want" ]; then
        printf 'FAIL %s\n  want: %s\n  got:  %s\n' "$name" "$want" "$got" >&2
        fail=$((fail + 1)); return 0
    fi
    pass=$((pass + 1))
}

# refuses <name> <expected exit> <expected first stderr line> -- TOOL ARG...
#
# The old tool refused this argv, so the translation must refuse it too, with
# the ORIGIN tool's own message and nothing at all on stdout -- never a
# plausible-looking command that would do something adjacent.
refuses() {
    name=$1; wantrc=$2; wantmsg=$3; shift 4
    got=$( MT_PROG0="$1" /bin/sh "$TR" "$@" 2>"$T/err" ); rc=$?
    msg=$(head -1 "$T/err")
    bad=''
    [ "$rc" = "$wantrc" ] || bad="exit $rc (want $wantrc)"
    [ -z "$got" ] || bad="$bad; stdout not empty: $got"
    [ "$msg" = "$wantmsg" ] || bad="$bad; stderr first line: $msg"
    if [ -n "$bad" ]; then
        printf 'FAIL %s: %s\n' "$name" "$bad" >&2
        fail=$((fail + 1)); return 0
    fi
    pass=$((pass + 1))
}

# ---- change_dylib: one assertion per flag -------------------------------
#
# All ten flags its parser accepts, each alone. `-grow` cannot appear alone
# (see the usage-error section below), so it is asserted with the smallest
# operation that lets it through.
ok cd-change    'macho9 dylib f f.new -replace OLD NEW
mv -f f.new f'        -- change_dylib f -change OLD NEW
ok cd-delete    'macho9 dylib f f.new -delete P
mv -f f.new f'               -- change_dylib f -delete P
ok cd-reexport  'macho9 dylib f f.new -reexport P
mv -f f.new f'             -- change_dylib f -reexport P
ok cd-add       'macho9 dylib f f.new -append P
mv -f f.new f'               -- change_dylib f -add P
ok cd-insert    'macho9 dylib f f.new -insert P
mv -f f.new f'               -- change_dylib f -insert P
ok cd-rchange   'macho9 rpath f f.new -replace OLD NEW
mv -f f.new f'        -- change_dylib f -change-rpath OLD NEW
ok cd-rdelete   'macho9 rpath f f.new -delete P
mv -f f.new f'               -- change_dylib f -delete-rpath P
ok cd-radd      'macho9 rpath f f.new -append P
mv -f f.new f'               -- change_dylib f -add-rpath P
ok cd-strip     'macho9 lc f f.new -delete uuid
mv -f f.new f'               -- change_dylib f -strip-lc uuid
ok cd-grow      'macho9 dylib f f.new --allow-grow -append P
mv -f f.new f'  -- change_dylib f -grow -add P

# -add is NOT -insert and -insert is NOT -add: an appended LC_LOAD_DYLIB gets
# the highest library ordinal, an inserted one gets ordinal 1. Asserting the
# pair together is what would catch a translation that silently downgraded one
# to the other.
ok cd-add-vs-insert 'macho9 dylib f f.new -append A -insert B
mv -f f.new f' -- change_dylib f -add A -insert B

# Every -strip-lc KIND, since the vocabulary is a table and a table can lose a
# row. These are change_dylib's own five, from src/lc_kinds.c.
ok cd-kind-uuid     'macho9 lc f f.new -delete uuid
mv -f f.new f'           -- change_dylib f -strip-lc uuid
ok cd-kind-codesig  'macho9 lc f f.new -delete codesig
mv -f f.new f'        -- change_dylib f -strip-lc codesig
ok cd-kind-srcver   'macho9 lc f f.new -delete source-version
mv -f f.new f' -- change_dylib f -strip-lc source-version
ok cd-kind-buildver 'macho9 lc f f.new -delete build-version
mv -f f.new f'  -- change_dylib f -strip-lc build-version
ok cd-kind-drs      'macho9 lc f f.new -delete code-sign-drs
mv -f f.new f'  -- change_dylib f -strip-lc code-sign-drs

# Repeats accumulate into ONE command per family, in the order typed -- not one
# command per operation. Order within an array is meaningful to the rewriter.
ok cd-repeat 'macho9 dylib f f.new -replace A B -replace C D
mv -f f.new f' -- change_dylib f -change A B -change C D
ok cd-strip-repeat 'macho9 lc f f.new -delete uuid -delete codesig
mv -f f.new f' -- change_dylib f -strip-lc uuid -strip-lc codesig

# ---- change_dylib: mixing families becomes ONE edit script ---------------
#
# One family is one verb, whose flags macho9 batches. More than one family has
# no single verb to be, so it is one `macho9 edit FILE OUT -` with the
# operations as statements on stdin -- one read, one pass per statement, one
# write, which is the shape the C tool had and a sequence of verbs did not.
# OUT is the positional right after FILE, as it is for every other verb here --
# it was a `--output` flag while `edit` still wrote FILE -- and SCRIPT is `-`,
# the here-document on stdin; the teaching form's trailing `mv` is the same
# install step every other verb's form ends with.
#
# The order is load-command, then dylib, then rpath: deleting load commands
# hands header pad back, and the other two consume it. Getting this backwards
# is how a mixed invocation that used to fit stops fitting.
ok cd-mixed-2 "macho9 edit f f.new - <<'MACHO9_EDIT'
load-command delete uuid
dylib replace A B
MACHO9_EDIT
mv -f f.new f" -- change_dylib f -strip-lc uuid -change A B

ok cd-mixed-3 "macho9 edit f f.new - <<'MACHO9_EDIT'
load-command delete uuid
dylib append D
rpath append R
MACHO9_EDIT
mv -f f.new f" -- change_dylib f -add-rpath R -add D -strip-lc uuid

# The ORDER OF THE FLAGS does not change the order of the statements between
# families -- only the order within each family's own block.
ok cd-mixed-order "macho9 edit f f.new - <<'MACHO9_EDIT'
load-command delete codesig
load-command delete uuid
dylib replace A B
MACHO9_EDIT
mv -f f.new f" -- change_dylib f -change A B -strip-lc codesig -strip-lc uuid

# -grow becomes the `allow-grow` DIRECTIVE, which must precede every
# operation (src/script.c refuses one that does not). Like the --allow-grow it
# replaces, it reaches dylib and rpath and not the load-command deletes:
# src/edit.c sets ops.allow_grow only for the statements that can outgrow the
# pad, and deleting load commands can only shrink the table.
ok cd-grow-mixed "macho9 edit f f.new - <<'MACHO9_EDIT'
allow-grow
load-command delete uuid
dylib replace A B
rpath append R
MACHO9_EDIT
mv -f f.new f" -- change_dylib f -grow -strip-lc uuid -change A B -add-rpath R

# EVERY INSERT IS EMITTED IN REVERSE FLAG ORDER, because each one goes to the
# FRONT of the table: as a batch `-insert A -insert B` leaves A at ordinal 1
# and B at 2, and a sequence reproduces that only by inserting B first. This
# is the assertion that catches the emission getting it the natural way round.
# tests/wrapper_test.sh asserts the resulting ordinals on a real binary.
ok cd-insert-reverse "macho9 edit f f.new - <<'MACHO9_EDIT'
load-command delete uuid
dylib insert B
dylib insert A
MACHO9_EDIT
mv -f f.new f" -- change_dylib f -insert A -insert B -strip-lc uuid

# install.sh's production line -- the single most important translation in
# this task, quoted from the plan's Task 0 evidence.
ok cd-production "macho9 edit /tmp/c /tmp/c.new - <<'MACHO9_EDIT'
load-command delete uuid
load-command delete codesig
dylib replace /usr/lib/libSystem.B.dylib @loader_path/../S.dylib
dylib replace /usr/lib/libicucore.A.dylib @loader_path/../I.dylib
dylib replace /usr/lib/libc++.1.dylib @loader_path/../c++.1.dylib
MACHO9_EDIT
mv -f /tmp/c.new /tmp/c" \
    -- change_dylib /tmp/c -strip-lc uuid -strip-lc codesig \
        -change /usr/lib/libSystem.B.dylib @loader_path/../S.dylib \
        -change /usr/lib/libicucore.A.dylib @loader_path/../I.dylib \
        -change /usr/lib/libc++.1.dylib @loader_path/../c++.1.dylib

# tests/characterize.sh's line, this repo's own CI equivalence gate.
ok cd-characterize "macho9 edit out out.new - <<'MACHO9_EDIT'
load-command delete uuid
load-command delete codesig
dylib replace /usr/lib/libSystem.B.dylib @loader_path/../S.dylib
MACHO9_EDIT
mv -f out.new out" \
    -- change_dylib out -strip-lc uuid -strip-lc codesig \
        -change /usr/lib/libSystem.B.dylib @loader_path/../S.dylib

# `-grow` with no operation at all: change_dylib accepts it (once argc is big
# enough) and changes nothing, so the translation is deliberately EMPTY rather
# than some adjacent command. Empty stdout with exit 0 is the contract for
# "the old invocation was a no-op".
ok cd-grow-only '' -- change_dylib f -grow -grow

# ---- fix_macho ----------------------------------------------------------
ok fm-change   'macho9 dylib f f.new -replace OLD NEW
mv -f f.new f'         -- fix_macho f -change OLD NEW
ok fm-stripbv  'macho9 lc f f.new -delete build-version
mv -f f.new f'       -- fix_macho f -strip_build_version
# -rename_seg is accepted by fix_macho's parser and appears NOWHERE in its
# usage text. Enumerating from the parser is what found it.
ok fm-rename   'macho9 segment f f.new __DATA __D2
mv -f f.new f'            -- fix_macho f -rename_seg __DATA __D2
# `macho9 segment` takes ONE pair, so two renames are two commands on their
# own -- no second family needed -- and become one edit script by the same
# rule as everything else. Each rename is still its own pass, in argv order.
ok fm-rename-2 "macho9 edit f f.new - <<'MACHO9_EDIT'
segment rename __A __B
segment rename __C __D
MACHO9_EDIT
mv -f f.new f" -- fix_macho f -rename_seg __A __B -rename_seg __C __D
# All three families, in load-command / dylib / segment order. No allow-grow
# anywhere: fix_macho has no -grow and never enlarges a header.
ok fm-all "macho9 edit f f.new - <<'MACHO9_EDIT'
load-command delete build-version
dylib replace A B
segment rename __A __B
MACHO9_EDIT
mv -f f.new f" -- fix_macho f -change A B -strip_build_version -rename_seg __A __B
# The flag is a boolean, so repeating it is still one -delete build-version.
ok fm-stripbv-twice 'macho9 lc f f.new -delete build-version
mv -f f.new f' \
    -- fix_macho f -strip_build_version -strip_build_version

# CHAINED -rename_seg TRANSLATES, and these three assertions USED TO BE
# `refuses ... 2 ...`. compat/translate.sh refused the shape while
# compat/fix_macho.c still shipped, because a wrapper had to preserve that
# tool's answer and the two differ: fix_macho gave each segment its FIRST
# matching pair and never revisited it, so `-rename_seg A B -rename_seg B C`
# ended at B, while two macho9 segment passes chain and end at C. The repo
# owner has since ruled that difference an improvement to ADOPT -- "doing what
# was asked" -- and the C tool is gone, so there is no longer a second answer
# to preserve. These now pin the translation, in the same place they used to
# pin the refusal; compat/translate.sh's -rename_seg arm records the reversal.
ok fm-chain "macho9 edit f f.new - <<'MACHO9_EDIT'
segment rename __DATA __X
segment rename __X __Y
MACHO9_EDIT
mv -f f.new f" -- fix_macho f -rename_seg __DATA __X -rename_seg __X __Y
# A chain of three emits three passes, in argv order -- every link, not just
# the first (which is where the refusal used to trip).
ok fm-chain-3 "macho9 edit f f.new - <<'MACHO9_EDIT'
segment rename __DATA __P
segment rename __P __Q
segment rename __Q __R
MACHO9_EDIT
mv -f f.new f" \
    -- fix_macho f -rename_seg __DATA __P -rename_seg __P __Q -rename_seg __Q __R
# The empty string is a legal NEW -- a segname may be all NULs -- and it stays
# an argument rather than vanishing: mt_quote emits it as '' so the emitted
# line still has four words after the verb. That is what the old chain check's
# own regression case was really about (an empty NEW that field-splitting
# silently dropped), and it is still worth pinning now that the check is gone.
# mt_quote's '' is also exactly what src/script.c's ms_split reads back as an
# empty field, so the statement still has four words.
ok fm-chain-empty "macho9 edit f f.new - <<'MACHO9_EDIT'
segment rename __DATA ''
segment rename '' __Y
MACHO9_EDIT
mv -f f.new f" \
    -- fix_macho f -rename_seg __DATA '' -rename_seg '' __Y
# The three shapes that were never affected by that refusal, and are not
# affected by its removal either. Each was measured against the real fix_macho
# on tests/fixture.macho and agreed byte-for-byte with its translation.
#
# NOTE none of these three is refused by mt_chain_check, which watches -change
# and not -rename_seg: a rename statement and a `macho9 segment` pass are the
# same single operation, so sequencing them is the adopted behaviour rather
# than a shape with no equivalent.
ok fm-same-old "macho9 edit f f.new - <<'MACHO9_EDIT'
segment rename __DATA __A
segment rename __DATA __B
MACHO9_EDIT
mv -f f.new f" -- fix_macho f -rename_seg __DATA __A -rename_seg __DATA __B
ok fm-new-eq-earlier-old "macho9 edit f f.new - <<'MACHO9_EDIT'
segment rename __DATA __B
segment rename __TEXT __DATA
MACHO9_EDIT
mv -f f.new f" -- fix_macho f -rename_seg __DATA __B -rename_seg __TEXT __DATA
ok fm-independent "macho9 edit f f.new - <<'MACHO9_EDIT'
segment rename __DATA __A
segment rename __TEXT __B
MACHO9_EDIT
mv -f f.new f" -- fix_macho f -rename_seg __DATA __A -rename_seg __TEXT __B

# ---- the four fixed-arity tools -----------------------------------------
# macho9 never writes its input, so the teaching form names an output of its
# own and ends with the install step -- two lines, and both of them pinned:
# what a reader is shown has to be the complete equivalent of the old in-place
# edit, not the half of it that rewrites nothing.
ok avm     'macho9 minos f f.new 10.9
mv -f f.new f'                               -- add_version_min f
# MT_OUT is how a wrapper names the temp it is going to install: the command
# writes exactly that, and the `mv` disappears because the wrapper does the
# installing itself.
mt_out_got=$( MT_OUT=/tmp/t.tmp /bin/sh "$TR" add_version_min f )
if [ "$mt_out_got" = 'macho9 minos f /tmp/t.tmp 10.9' ]; then
    pass=$((pass + 1))
else
    printf 'FAIL avm-mt-out: got %s\n' "$mt_out_got" >&2; fail=$((fail + 1))
fi
# patch_macho is the one tool whose grammar always named its own output, so the
# teaching form is the command the user typed -- there is nothing to install.
ok pm      'macho9 declassify in out'        -- patch_macho in out
# ... EXCEPT when IN and OUT are the same file, which patch_macho allowed and
# `macho9 declassify` now refuses. The teaching form has to be a pasteable
# equivalent of that in-place conversion, so it names an output of its own and
# installs it, exactly as the five in-place tools' forms do.
ok pm-same 'macho9 declassify f f.new
mv -f f.new f'                               -- patch_macho f f
# MT_OUT is how the wrapper names the temp it installs onto the user's OUT --
# for both shapes, since the wrapper installs onto OUT either way.
mt_out_got=$( MT_OUT=/tmp/t.tmp /bin/sh "$TR" patch_macho in out )
if [ "$mt_out_got" = 'macho9 declassify in /tmp/t.tmp' ]; then
    pass=$((pass + 1))
else
    printf 'FAIL pm-mt-out: got %s\n' "$mt_out_got" >&2; fail=$((fail + 1))
fi
mt_out_got=$( MT_OUT=/tmp/t.tmp /bin/sh "$TR" patch_macho f f )
if [ "$mt_out_got" = 'macho9 declassify f /tmp/t.tmp' ]; then
    pass=$((pass + 1))
else
    printf 'FAIL pm-mt-out-same: got %s\n' "$mt_out_got" >&2; fail=$((fail + 1))
fi
ok rs      'macho9 segment f f.new __DATA __DATA2
mv -f f.new f' -- rename_segment f __DATA __DATA2
ok rs-16   'macho9 segment f f.new __DATA 1234567890123456
mv -f f.new f' -- rename_segment f __DATA 1234567890123456
# retag_swift_classes is variadic over FILES; macho9 retag-swift takes one, so
# the translation is a loop, one line per file, in argv order. Each file, like
# add_version_min's, gets its own output and install step.
ok rsc-1 'macho9 retag-swift a a.new
mv -f a.new a'                                                    -- retag_swift_classes a
ok rsc-3 'macho9 retag-swift a a.new
mv -f a.new a
macho9 retag-swift b b.new
mv -f b.new b
macho9 retag-swift c c.new
mv -f c.new c' -- retag_swift_classes a b c
# MT_OUT names a single output for the whole call, so it only makes sense set
# when retranslating ONE file at a time -- retag_swift_classes.sh's own loop.
rsc_out_got=$( MT_OUT=/tmp/t.tmp /bin/sh "$TR" retag_swift_classes a )
if [ "$rsc_out_got" = 'macho9 retag-swift a /tmp/t.tmp' ]; then
    pass=$((pass + 1))
else
    printf 'FAIL rsc-mt-out: got %s\n' "$rsc_out_got" >&2; fail=$((fail + 1))
fi

# ---- quoting ------------------------------------------------------------
#
# Each emitted line has to be eval-safe, because that is how a wrapper runs it.
# Quote only what needs quoting, so the common case stays readable.
ok q-space "macho9 segment 'a b' 'a b.new' __DATA __D2
mv -f 'a b.new' 'a b'"      -- rename_segment 'a b' __DATA __D2
ok q-quote "macho9 declassify 'it'\\''s' out"      -- patch_macho "it's" out
ok q-empty "macho9 segment f f.new __DATA ''
mv -f f.new f"            -- rename_segment f __DATA ''
# ... and an eval of that line really does reconstruct the argument. The FIRST
# line only: the translation now ends with an `mv` install line, and it is the
# verb command's own quoting this is about. Positionals after the verb are
# FILE, OUT, OLD, NEW -- so NEW is $5, not $4.
line=$( /bin/sh "$TR" rename_segment 'a b' __DATA "__d'x" | sed -n 1p )
set --; eval "set -- $(printf '%s' "$line" | sed 's/^macho9 //')"
if [ "$1" = segment ] && [ "$2" = 'a b' ] && [ "$5" = "__d'x" ]; then
    pass=$((pass + 1))
else
    printf 'FAIL q-eval: eval of %s produced [%s][%s][%s][%s][%s]\n' "$line" \
        "${1:-}" "${2:-}" "${3:-}" "${4:-}" "${5:-}" >&2
    fail=$((fail + 1))
fi

# ---- MACHO9 names the program word --------------------------------------
got=$( MACHO9=/opt/bin/macho9 /bin/sh "$TR" add_version_min f )
if [ "$got" = '/opt/bin/macho9 minos f f.new 10.9
mv -f f.new f' ]; then
    pass=$((pass + 1))
else
    printf 'FAIL macho9-env: got %s\n' "$got" >&2; fail=$((fail + 1))
fi

# ---- refusals: the old tool's own message, verbatim ---------------------
#
# Every one of these is a case the OLD tool refused. The translation must
# refuse identically, print nothing on stdout, and use the ORIGIN wording --
# for the capacity caps that is a controller ruling, because cli/macho9.c
# deliberately prints different text there so the new grammar never leaks the
# old flag spellings.
CD_USAGE='Usage: change_dylib input [-grow] [-change old new] [-delete path] [-reexport path] [-add path] [-insert path] [-strip-lc name] [-change-rpath old new] [-delete-rpath path] [-add-rpath path] ...'

# `argc < 4`: change_dylib's usage text presents -grow as a standalone option,
# but its parser never gets to see it. Parser wins.
refuses cd-usage-grow  1 "$CD_USAGE" -- change_dylib f -grow
refuses cd-usage-bare  1 "$CD_USAGE" -- change_dylib f
refuses cd-usage-none  1 "$CD_USAGE" -- change_dylib

# The `i + N < argc` guards name the FLAG, not the missing operand.
refuses cd-short-change 1 'bad arg: -change'       -- change_dylib f -change OLD
refuses cd-short-add    1 'bad arg: -add'          -- change_dylib f -grow -add
refuses cd-short-strip  1 'bad arg: -strip-lc'     -- change_dylib f -grow -strip-lc
refuses cd-short-rchange 1 'bad arg: -change-rpath' -- change_dylib f -change-rpath OLD
refuses cd-bad-arg      1 'bad arg: -nope'         -- change_dylib f -nope x
refuses cd-bad-kind     1 'unknown -strip-lc kind: nope' -- change_dylib f -strip-lc nope

# THE ONE REFUSAL THIS FILE MAKES OF ITS OWN, on the edit-script path only.
# A batch gives every -change the ORIGINAL paths to compare against, so
# `-change a b -change b c` rewrites the a's to b and the original b's to c
# and never revisits what the first pair produced; a sequence of statements
# rewrites those too and lands on c. No emission order fixes it, so the
# translation says so instead of emitting something adjacent.
refuses cd-chain 1 '-change a b and -change b c chain: run them as separate invocations' \
    -- change_dylib f -change a b -change b c -strip-lc uuid
# A SWAP is the same defect: b becomes a, and then every a (including the ones
# the first statement just made) becomes b.
refuses cd-chain-swap 1 '-change a b and -change b a chain: run them as separate invocations' \
    -- change_dylib f -change a b -change b a -strip-lc uuid
# The rpath family gets the same check, naming ITS flag -- the user typed
# -change-rpath, and a message naming -change would name a flag they did not.
refuses cd-chain-rpath 1 '-change-rpath a b and -change-rpath b c chain: run them as separate invocations' \
    -- change_dylib f -change-rpath a b -change-rpath b c -strip-lc uuid
# fix_macho's -change, on the same path, for the same reason.
refuses fm-chain-change 1 '-change a b and -change b c chain: run them as separate invocations' \
    -- fix_macho f -change a b -change b c -strip_build_version
# THE SAME CHAIN WITH ONE FAMILY IS NOT REFUSED, and this is the assertion
# that keeps the refusal from spreading to invocations that do not need it:
# the verb's own batch is the C tool's semantics, so there is nothing to
# reproduce and nothing to refuse.
ok cd-chain-one-family 'macho9 dylib f f.new -replace a b -replace b c
mv -f f.new f' \
    -- change_dylib f -change a b -change b c
ok fm-chain-one-family 'macho9 dylib f f.new -replace a b -replace b c
mv -f f.new f' \
    -- fix_macho f -change a b -change b c
# And a -change whose NEW is its OWN old is not a chain: no OTHER statement
# rewrites what it produced, so a statement and a batch agree. Asserted ON the
# edit-script path, since that is the only place the check runs at all.
ok cd-self-replace "macho9 edit f f.new - <<'MACHO9_EDIT'
load-command delete uuid
dylib replace a a
dylib replace c d
MACHO9_EDIT
mv -f f.new f" -- change_dylib f -change a a -change c d -strip-lc uuid

refuses fm-usage        1 'Usage: fix_macho <file> [-change old new] [-strip_build_version]' -- fix_macho f
refuses fm-unknown      1 'Unknown option: -nope'  -- fix_macho f -nope
refuses fm-short-change 1 'Unknown option: -change' -- fix_macho f -change OLD
refuses fm-long-seg     1 'new segment name longer than 16 bytes: 12345678901234567' \
    -- fix_macho f -rename_seg __DATA 12345678901234567

refuses avm-usage-0 1 'Usage: add_version_min binary' -- add_version_min
refuses avm-usage-2 1 'Usage: add_version_min binary' -- add_version_min a b
refuses pm-usage-1  1 'Usage: patch_macho input output' -- patch_macho a
refuses pm-usage-3  1 'Usage: patch_macho input output' -- patch_macho a b c
refuses rs-usage-2  1 'Usage: rename_segment binary OLDNAME NEWNAME' -- rename_segment a b
refuses rs-usage-4  1 'Usage: rename_segment binary OLDNAME NEWNAME' -- rename_segment a b c d
refuses rs-long     1 'new segment name longer than 16 bytes' -- rename_segment f __DATA 12345678901234567
refuses rsc-usage-0 1 'Usage: retag_swift_classes binary [binary ...]' -- retag_swift_classes

# A tool this file has never heard of gets a clear "no equivalent" and a
# nonzero exit -- never a guess.
refuses unknown-tool 2 \
    'translate.sh: no equivalent -- unknown tool otool (expected one of: change_dylib add_version_min patch_macho rename_segment retag_swift_classes fix_macho)' \
    -- otool -L f

# ---- capacity caps ------------------------------------------------------
#
# Enforced HERE, printing change_dylib's own text, because routing through
# macho9 would print macho9's (which names -append where change_dylib names
# -add). Both cap sites in cli/macho9.c carry a comment saying exactly that.
mkcap() { i=0; out=''; while [ $i -lt $2 ]; do out="$out $1"; i=$((i + 1)); done; printf '%s' "$out"; }

ok cap-strip-16-fits "macho9 lc f f.new$(mkcap '-delete uuid' 16)
mv -f f.new f" -- change_dylib f $(mkcap '-strip-lc uuid' 16)
refuses cap-strip-17 1 'too many -strip-lc (max 16)' -- change_dylib f $(mkcap '-strip-lc uuid' 17)
refuses cap-add-33   1 'too many -add (max 32)'      -- change_dylib f $(mkcap '-add P' 33)
refuses cap-ins-33   1 'too many -insert (max 32)'   -- change_dylib f $(mkcap '-insert P' 33)
refuses cap-chg-33   1 'too many -change (max 32)'   -- change_dylib f $(mkcap '-change A B' 33)
refuses cap-radd-33  1 'too many -add-rpath (max 32)' -- change_dylib f $(mkcap '-add-rpath P' 33)
# -change, -delete and -reexport SHARE one 32-entry array, so 16 of one plus 17
# of another overflows although neither reaches 32 alone. The message names the
# flag that overflowed, which here is -delete.
refuses cap-shared 1 'too many -delete (max 32)' \
    -- change_dylib f $(mkcap '-change A B' 16) $(mkcap '-delete P' 17)

# fix_macho's two caps, which came here when compat/fix_macho.c retired. Its
# FM_ROOM printed change_dylib's exact wording with fix_macho's flag names in
# it, so these are that same text. The -rename_seg cap has no macho9
# counterpart at all -- macho9 sees one rename at a time either way, its
# `segment` verb taking one pair and an edit script's `segment rename`
# statement being one pair, so nothing downstream would ever count them --
# which makes this file the only thing keeping that refusal alive.
ok fm-cap-change-32-fits "macho9 dylib f f.new$(mkcap '-replace A B' 32)
mv -f f.new f" \
    -- fix_macho f $(mkcap '-change A B' 32)
refuses fm-cap-change-33 1 'too many -change (max 32)' -- fix_macho f $(mkcap '-change A B' 33)
refuses fm-cap-rename-17 1 'too many -rename_seg (max 16)' \
    -- fix_macho f $(mkcap '-rename_seg __A __B' 17)
# At capacity must still be accepted -- a check one too eager would silently
# halve what a caller can ask for. 16 pairs is 16 emitted `segment rename`
# statements (counted, not the line total, since the edit script adds its own
# opening and terminating lines around them).
fm_16=$( /bin/sh "$TR" fix_macho f $(mkcap '-rename_seg __A __B' 16) 2>"$T/err" )
fm_16_n=$(printf '%s\n' "$fm_16" | grep -c '^segment rename __A __B$')
if [ "$fm_16_n" = 16 ]; then
    pass=$((pass + 1))
else
    printf 'FAIL fm-cap-rename-16-fits: %s statements, want 16 (stderr: %s)\n' \
        "$fm_16_n" "$(cat "$T/err")" >&2
    fail=$((fail + 1))
fi

# ---- the emitted grammar is one this build actually has -----------------
#
# `macho9 --capabilities` is the machine-readable probe docs/PROPOSAL.md's
# "Migration" section says exists so the wrapper and the binary need not move
# in lockstep. Use it rather than assuming: every verb this translator can
# emit must be advertised, every -OP it can emit must be in that verb's ops=,
# and every KIND must be in lc's kinds=.
"$BIN/machotool" --capabilities > "$T/caps" 2>/dev/null
capcheck() {   # capcheck <verb> <attr-prefix> <value>...
    v=$1; attr=$2; shift 2
    line=$(grep "^verb $v" "$T/caps")
    if [ -z "$line" ]; then
        printf 'FAIL caps-%s: this build does not advertise the verb\n' "$v" >&2
        fail=$((fail + 1)); return 0
    fi
    for want in "$@"; do
        vals=$(printf '%s\n' "$line" | tr ' ' '\n' | sed -n "s/^$attr=//p" | tr ',' '\n')
        if printf '%s\n' "$vals" | grep -qx "$want"; then
            pass=$((pass + 1))
        else
            printf 'FAIL caps-%s: %s=%s not advertised (line: %s)\n' "$v" "$attr" "$want" "$line" >&2
            fail=$((fail + 1))
        fi
    done
}
for v in declassify minos segment retag-swift lc dylib rpath edit; do
    if grep -q "^verb $v" "$T/caps"; then
        pass=$((pass + 1))
    else
        printf 'FAIL caps-verb-%s: not advertised by this build\n' "$v" >&2
        fail=$((fail + 1))
    fi
done
capcheck dylib ops replace delete append insert reexport
capcheck rpath ops replace delete append
capcheck lc    ops delete
capcheck lc    kinds uuid codesig source-version build-version code-sign-drs
capcheck minos versions 10.9

# Every STATEMENT this translator can put in an edit script, with its arity --
# the same agreement the verb checks above make, for the other half of what is
# emitted. A statement macho9 does not know is a script that fails to parse,
# which is a worse failure than a verb that does not exist: it happens after
# the wrapper has already told the caller what it was about to run.
stmtcheck() {   # stmtcheck <kind> <op> <nargs>
    if grep -qxF "statement $1 $2 $3" "$T/caps"; then
        pass=$((pass + 1))
    else
        printf 'FAIL caps-statement-%s-%s: this build does not advertise it taking %s argument(s)\n' \
            "$1" "$2" "$3" >&2
        fail=$((fail + 1))
    fi
}
stmtcheck load-command delete 1
stmtcheck dylib replace 2
stmtcheck dylib delete 1
stmtcheck dylib reexport 1
stmtcheck dylib append 1
stmtcheck dylib insert 1
stmtcheck rpath replace 2
stmtcheck rpath delete 1
stmtcheck rpath append 1
stmtcheck segment rename 2

# ---- POSIX sh, not bash -------------------------------------------------
#
# These wrappers run on 10.9, whose /bin/sh is bash 3.2 in sh mode -- which
# still accepts plenty that a stricter POSIX shell does not. Re-run one
# translation under ksh, which is present on 10.9 too and does not share
# bash's extensions, so a bashism that slips in fails here rather than on
# somebody's machine.
if [ -x /bin/ksh ]; then
    got=$( /bin/ksh "$TR" change_dylib f -strip-lc uuid -change A B -add-rpath R 2>&1 )
    want="macho9 edit f f.new - <<'MACHO9_EDIT'
load-command delete uuid
dylib replace A B
rpath append R
MACHO9_EDIT
mv -f f.new f"
    if [ "$got" = "$want" ]; then
        pass=$((pass + 1))
    else
        printf 'FAIL ksh: translate.sh does not run the same under ksh\n  got: %s\n' "$got" >&2
        fail=$((fail + 1))
    fi
else
    # SAY SO. A silently-absent check reads as a passing one: the total just
    # drops by one and nothing tells you which shell went unexercised.
    echo "translate_test: SKIP the second-shell cross-check -- /bin/ksh is not present on this host"
fi

echo "translate_test: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0
