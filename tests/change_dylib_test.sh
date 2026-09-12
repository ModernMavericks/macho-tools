#!/bin/sh
# Hermetic test for change_dylib's library-ordinal renumbering (-insert, -delete).
#
# Two-level-namespace binaries record, for every undefined symbol, WHICH dylib it
# comes from — as a 1-based index into the LC_LOAD_DYLIB commands in load order
# (nlist n_desc, and the SET_DYLIB_ORDINAL opcodes in the LC_DYLD_INFO bind
# streams). Inserting or deleting a load command shifts those indices, so the
# tool must renumber them or the binary silently binds symbols to the wrong
# library. This builds real dylibs, rewrites a real executable, and RUNS it —
# a wrong ordinal shows up as a dyld "Symbol not found" or a wrong answer.
#
#   sh tests/change_dylib_test.sh               standalone (needs only clang + otool)
#   sh tests/change_dylib_test.sh <bindir>       via ctest: uses the change_dylib
#                                                 and fix_macho wrappers CMake
#                                                 already staged next to machotool
set -e
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$SCRIPT_DIR"
ROOT_DIR="$SCRIPT_DIR/.."
SRC_DIR="$ROOT_DIR/src"
COMPAT_DIR="$ROOT_DIR/compat"
CC="${CC:-clang}"

# The FIXTURES must be 10.9-targeted, not host-targeted. A modern linker emits
# LC_DYLD_CHAINED_FIXUPS by default, and change_dylib refuses those on purpose --
# 10.9's dyld cannot read them, which is why patch_macho exists. Without this the
# suite passes on 10.9 and fails on a modern runner, having silently changed what
# it tests. -mmacosx-version-min=10.9 gets the classic LC_DYLD_INFO_ONLY form on
# either host, so the test asks the same question everywhere.
#
# Note this applies only to the fixtures. change_dylib/fix_macho themselves
# (below) are /bin/sh wrappers around machotool, which is a host tool, built for
# (or already built on) the host.
FIXTURE_FLAGS="-mmacosx-version-min=10.9"
T="${TMPDIR:-/tmp}/change_dylib_test.$$"
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT INT TERM

# Take the binaries CMake already built when a build dir is passed -- matching
# chained-fixups.sh/characterize.sh/cli_test.sh/leaf-tool-crashes.sh, which
# all receive $<TARGET_FILE_DIR:...> this way and run the binary CMake built,
# not one they compile themselves. Compile from source ONLY as the standalone
# fallback (`sh tests/change_dylib_test.sh`, no arguments, clang + otool only).
#
# The standalone build used to hand-enumerate macho9core's source list right
# here -- a SECOND place deciding what the library contains, independent of
# CMakeLists.txt's own `add_library(machotoolcore ...)` list (still a hand
# enumeration itself, CMakeLists.txt:57), which had already needed
# hand-updating five times (uleb, image, ordinals, fat, trie, lc_kinds,
# atomic_write, linkedit, grow) as the toolkit grew. Two places independently
# deciding one thing is this repo's signature bug class (two deletion
# predicates, two fat parsers, two export-LC scans, two LC-kind tables, all
# shipped); this was that same class living in a test script, where it could
# let this exact path -- the one a standalone `sh change_dylib_test.sh` run
# actually exercises -- silently drift out of sync while ctest itself stayed
# green, because ctest (below) never took this branch at all. Globbing
# src/*.c kills that historical drift (a new library file forgotten here
# stops being possible), but it is NOT the same list CMakeLists.txt compiles
# -- it is a superset by construction, since CMakeLists.txt's own list is
# still hand-enumerated. A future src/something_else.c deliberately kept OUT
# of machotoolcore would still be silently pulled into this standalone build.
# A real fix (glob machotoolcore's own sources in CMakeLists.txt too, or have
# this script read that list back out of CMake) is a follow-up, not this
# round -- the glob here only trades the demonstrated failure mode (a file
# added to machotoolcore and forgotten here) for a theoretical one (a file
# deliberately excluded from machotoolcore that this glob doesn't know to
# exclude), which has never happened in this repo's history.
#
# A bindir argument, once given, MUST be honored or the run must fail loudly
# -- never silently fall back to a from-source build. A wrong or stale bindir
# (a typo, a build that didn't finish, a renamed preset) falling back here
# would make ctest pass green while never once exercising the shipped
# binary -- exactly the "green while broken" failure class Step 2 exists to
# close. So: no argument at all means standalone (compile from source, the
# documented, intentional fallback); a NON-EMPTY argument is a hard
# requirement, exactly like the other four suites' `BIN="${1:?usage...}"`.
#
# STANDALONE, AFTER TASK 2: change_dylib is no longer a C program to compile.
# It is compat/change_dylib.sh, a wrapper that needs machotool and the two
# files it sources sitting next to it -- so the standalone branch builds
# machotool and then assembles that layout in $T, under the installed names,
# exactly as CMakeLists.txt stages it next to machotool in a build tree.
#
# fix_macho joined it: compat/fix_macho.c is gone, and there is nothing left
# in compat/ to compile at all. machotool is the only binary this branch
# builds now, which is the whole retirement plan's headline seen from inside
# a test.
if [ $# -eq 0 ]; then
    echo "change_dylib_test: no bindir given -- compiling standalone from source"
    mkdir -p "$T/bin"
    "$CC" -O2 -I "$SRC_DIR" -o "$T/bin/machotool" "$ROOT_DIR/cli/machotool.c" "$SRC_DIR"/*.c
    "$CC" -O2 -o "$T/bin/makefat" "$SCRIPT_DIR/makefat.c"
    "$CC" -O2 -o "$T/bin/fatcheck" "$SCRIPT_DIR/fatcheck.c"
    cp "$COMPAT_DIR/change_dylib.sh" "$T/bin/change_dylib"
    cp "$COMPAT_DIR/fix_macho.sh" "$T/bin/fix_macho"
    cp "$COMPAT_DIR/machotool-compat.sh" "$T/bin/machotool-compat.sh"
    cp "$COMPAT_DIR/translate.sh" "$T/bin/machotool-translate.sh"
    chmod +x "$T/bin/change_dylib" "$T/bin/fix_macho"
    BIN="$T/bin"
    CHANGE_DYLIB="$T/bin/change_dylib"
    FIX_MACHO="$T/bin/fix_macho"
    MACHOTOOL="$T/bin/machotool"
else
    BIN="$1"
    if [ ! -x "$BIN/change_dylib" ] || [ ! -x "$BIN/fix_macho" ]; then
        echo "change_dylib_test: bindir '$BIN' given but change_dylib/fix_macho not found (or not executable) there -- refusing to silently fall back to a from-source build" >&2
        exit 1
    fi
    if [ ! -x "$BIN/makefat" ] || [ ! -x "$BIN/fatcheck" ]; then
        echo "change_dylib_test: bindir '$BIN' given but makefat/fatcheck not found (or not executable) there -- refusing to silently fall back to a from-source build" >&2
        exit 1
    fi
    echo "change_dylib_test: using the CMake-built binaries in $BIN"
    CHANGE_DYLIB="$BIN/change_dylib"
    FIX_MACHO="$BIN/fix_macho"
    MACHOTOOL="$BIN/machotool"
fi
fails=0
ok()   { echo "PASS $1"; }
bad()  { echo "FAIL $1: $2"; fails=$((fails+1)); }
# Not a failure: the assertion could not be exercised on this host (e.g. its
# linker didn't produce the load command being tested). Printed loudly and
# distinctly from PASS/FAIL, per-assertion, rather than silently omitted --
# a silent skip is how coverage rots. Does not touch $fails.
skip() { echo "SKIP $1: $2"; }

# A handful of cases below capture a helper's OUTPUT (a rewritten binary's
# stdout, or ordinal_of's printed ordinal) with `2>&1` merged in, then compare
# that captured value for equality against an expected string. That is a
# libgmalloc harness bug: this whole suite is sometimes run under
# DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib (see case 18's own
# libgmalloc run, and INGREDIENTS.md/tests/README.md on why this codebase
# leans on it), which prints an unrelated banner to STDERR the first time any
# process it's injected into touches the heap. Merging that banner into a
# captured value being equality-checked turns a perfectly healthy run into a
# spurious failure -- not a memory bug, a harness bug, and specifically one
# that makes the suite unusable under the exact tool this codebase uses to
# find real memory bugs. Fixed at each of the 7 call sites below by sending
# stderr to its own scratch file instead of merging it into the captured
# value; the file's content is still folded into any bad() message, so a
# helper that genuinely fails still fails loudly, it just no longer corrupts
# the comparison itself.

# ordinal_of FILE SYMBOL: prints the 1-based library ordinal an undefined
# nlist symbol's n_desc records, or exits nonzero with a message on stderr.
# Case 8 needs this because neither `otool -L` nor a runtime re-run is enough
# to check an ordinal VALUE was written correctly (see that case's comment
# for what was tried and ruled out). It reads the same GET_LIBRARY_ORDINAL
# macro change_dylib itself uses, over our own minimal LC_SYMTAB walk -- not
# a text format any Apple tool controls the shape of across OS versions, so
# it asks the same question on a 10.9 host and a 2020s one.
cat > "$T/ordinal_of.c" <<'EOF'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s file symbol\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); return 2; }
    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf || read(fd, buf, (size_t)st.st_size) != st.st_size) {
        fprintf(stderr, "read failed\n"); return 2;
    }
    close(fd);
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    if (hdr->magic != MH_MAGIC_64) { fprintf(stderr, "not a 64-bit Mach-O\n"); return 2; }
    uint8_t *lcp = buf + sizeof(struct mach_header_64);
    struct symtab_command *st_cmd = NULL;
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_SYMTAB) st_cmd = (struct symtab_command *)lcp;
        lcp += lc->cmdsize;
    }
    if (!st_cmd) { fprintf(stderr, "no LC_SYMTAB\n"); return 2; }
    struct nlist_64 *syms = (struct nlist_64 *)(buf + st_cmd->symoff);
    const char *strtab = (const char *)(buf + st_cmd->stroff);
    for (uint32_t i = 0; i < st_cmd->nsyms; i++) {
        struct nlist_64 *n = &syms[i];
        if (n->n_type & N_STAB) continue;
        uint8_t type = n->n_type & N_TYPE;
        if (type != N_UNDF && type != N_PBUD) continue;
        const char *name = strtab + n->n_un.n_strx;
        if (strcmp(name, argv[2]) == 0) {
            printf("%d\n", GET_LIBRARY_ORDINAL(n->n_desc));
            return 0;
        }
    }
    fprintf(stderr, "symbol not found: %s\n", argv[2]);
    return 1;
}
EOF
"$CC" -O2 -o "$T/ordinal_of" "$T/ordinal_of.c"

# has_bytes FILE NEEDLE: exit 0 if NEEDLE's bytes appear anywhere in FILE,
# 1 if not, 2 on error. A plain byte-search (memmem), not grep/otool/nm --
# built once here so every case below that just needs "is this path string
# present/absent in the rewritten file" (case 8b's fix_macho -change dylib
# path, and the long-path case further down) shares one implementation
# instead of each hand-rolling its own oracle.
cat > "$T/has_bytes.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s file needle\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st; fstat(fd, &st);
    char *buf = malloc((size_t)st.st_size);
    if (!buf || read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) {
        fprintf(stderr, "read failed\n"); return 2;
    }
    close(fd);
    size_t nlen = strlen(argv[2]);
    return memmem(buf, (size_t)st.st_size, argv[2], nlen) != NULL ? 0 : 1;
}
EOF
"$CC" -O2 -o "$T/has_bytes" "$T/has_bytes.c"

# makefat/fatcheck: build and inspect a fat (universal) Mach-O without
# depending on system lipo. See tests/makefat.c and tests/fatcheck.c -- CMake
# builds both beside machotool, and $BIN is that directory.

# --- fixtures: three dylibs, and a main that calls into two of them ----------
cat > "$T/a.c" <<'EOF'
int a_sym(void) { return 11; }
EOF
cat > "$T/b.c" <<'EOF'
int b_sym(void) { return 22; }
EOF
cat > "$T/spare.c" <<'EOF'
int spare_sym(void) { return 99; }
EOF
cat > "$T/main.c" <<'EOF'
#include <stdio.h>
int a_sym(void); int b_sym(void);
int main(void) { int v = a_sym() + b_sym(); printf("%d\n", v); return v == 33 ? 0 : 1; }
EOF
for l in a b spare; do
    "$CC" -dynamiclib -O2 $FIXTURE_FLAGS -install_name "@loader_path/lib$l.dylib" \
        "$T/$l.c" -o "$T/lib$l.dylib"
done

# ordinals as linked: 1=liba, 2=libb  (link order sets load-command order)
build_main() { "$CC" -O2 $FIXTURE_FLAGS "$T/main.c" "$T/liba.dylib" "$T/libb.dylib" -o "$1"; }

# --- baseline ----------------------------------------------------------------
build_main "$T/main"
out=$(cd "$T" && ./main) && [ "$out" = "33" ] \
    && ok "baseline runs (33)" || bad "baseline" "got '$out'"

# --- 1. -insert puts the new dylib FIRST and renumbers ------------------------
# Without renumbering, a_sym's ordinal 1 now names libspare -> dyld aborts.
build_main "$T/main_ins"
"$CHANGE_DYLIB" "$T/main_ins" -grow -insert "@loader_path/libspare.dylib" >/dev/null || bad "tool run" "change_dylib failed"
first=$(otool -L "$T/main_ins" | sed -n '2p' | awk '{print $1}')
case "$first" in
    *libspare.dylib) ok "-insert: libspare is the first dependency" ;;
    *) bad "-insert order" "first dep is '$first'" ;;
esac
if out=$(cd "$T" && ./main_ins 2>"$T/main_ins.err") && [ "$out" = "33" ]; then
    ok "-insert: renumbered, binary still resolves a_sym/b_sym (33)"
else
    bad "-insert renumber" "got '$out'$( [ -s "$T/main_ins.err" ] && echo "; stderr: $(cat "$T/main_ins.err")")"
fi

# --- 2. -delete of an EARLIER dylib renumbers the survivors -------------------
# libspare is linked first but unreferenced; deleting it shifts liba 2->1,
# libb 3->2. Without renumbering, a_sym would be looked up in libb.
"$CC" -O2 $FIXTURE_FLAGS "$T/main.c" "$T/libspare.dylib" "$T/liba.dylib" "$T/libb.dylib" -o "$T/main_del"
out=$(cd "$T" && ./main_del) && [ "$out" = "33" ] \
    || bad "delete fixture" "fixture itself broken: '$out'"
"$CHANGE_DYLIB" "$T/main_del" -delete "@loader_path/libspare.dylib" >/dev/null || bad "tool run" "change_dylib failed"
otool -L "$T/main_del" | grep -q libspare \
    && bad "-delete" "libspare still present" \
    || ok "-delete: libspare removed"
if out=$(cd "$T" && ./main_del 2>"$T/main_del.err") && [ "$out" = "33" ]; then
    ok "-delete: renumbered, survivors still resolve (33)"
else
    bad "-delete renumber" "got '$out'$( [ -s "$T/main_del.err" ] && echo "; stderr: $(cat "$T/main_del.err")")"
fi

# --- 3. deleting a dylib that symbols still bind to must be refused ----------
build_main "$T/main_bad"
if "$CHANGE_DYLIB" "$T/main_bad" -delete "@loader_path/liba.dylib" >/dev/null 2>&1; then
    bad "-delete in-use" "tool accepted deleting a dylib that still has bound symbols"
else
    ok "-delete: refuses to orphan symbols bound to the deleted dylib"
fi

# --- 4. -insert composes with -change ----------------------------------------
build_main "$T/main_both"
"$CHANGE_DYLIB" "$T/main_both" -grow -insert "@loader_path/libspare.dylib" \
    -change "@loader_path/libb.dylib" "@loader_path/libb2.dylib" >/dev/null || bad "tool run" "change_dylib failed"
cp "$T/libb.dylib" "$T/libb2.dylib"
otool -L "$T/main_both" | grep -q libb2 \
    && ok "-insert + -change compose" || bad "compose" "libb2 not present"
if out=$(cd "$T" && ./main_both 2>"$T/main_both.err") && [ "$out" = "33" ]; then
    ok "-insert + -change: still resolves (33)"
else
    bad "compose run" "got '$out'$( [ -s "$T/main_both.err" ] && echo "; stderr: $(cat "$T/main_both.err")")"
fi

# --- 5. two dylibs exporting the SAME symbol ---------------------------------
# The shape MF actually ships: libS.dylib wraps symbols libSystem also exports,
# so a stale ordinal can name a library that does resolve the symbol — just the
# wrong implementation. In practice dyld usually catches a stale ordinal first,
# because dyld_stub_binder lives in the last-linked dylib and its index goes out
# of range ("library ordinal too big"). Pinned here either way: after the
# rewrite this must still call libdup1.
cat > "$T/dup1.c" <<'EOF'
int dup_sym(void) { return 1; }
EOF
cat > "$T/dup2.c" <<'EOF'
int dup_sym(void) { return 2; }
EOF
cat > "$T/dupmain.c" <<'EOF'
#include <stdio.h>
int dup_sym(void);
int main(void) { int v = dup_sym(); printf("%d\n", v); return v == 1 ? 0 : 1; }
EOF
for l in dup1 dup2; do
    "$CC" -dynamiclib -O2 $FIXTURE_FLAGS -install_name "@loader_path/lib$l.dylib" \
        "$T/$l.c" -o "$T/lib$l.dylib"
done
# ordinals: 1=libspare, 2=libdup1 (the one we bind to), 3=libdup2
"$CC" -O2 $FIXTURE_FLAGS "$T/dupmain.c" "$T/libspare.dylib" "$T/libdup1.dylib" "$T/libdup2.dylib" \
    -o "$T/main_dup"
"$CHANGE_DYLIB" "$T/main_dup" -delete "@loader_path/libspare.dylib" >/dev/null \
    || bad "tool run" "change_dylib failed"
out=$(cd "$T" && ./main_dup 2>"$T/main_dup.err") || true
if [ "$out" = "1" ]; then
    ok "-delete: still calls libdup1 (no silent rebind to libdup2)"
else
    bad "-delete silent rebind" "got '$out' — bound to the wrong dylib$( [ -s "$T/main_dup.err" ] && echo "; stderr: $(cat "$T/main_dup.err")")"
fi

# --- 7. a path both -change'd and -delete'd in one invocation ----------------
# Regression test for a Task 3 review finding: build_lcs used to decide
# deletion by "the FIRST `changes[]` entry matching this path has new_path ==
# NULL", while the ordinal map (mo_map_build, via ord_is_deleted) decided by
# "ANY entry matching this path has new_path == NULL". Naming libspare in both
# a -change and a -delete made the two disagree -- the load command survived
# (renamed) while the map marked it gone -- and every ordinal after it in the
# binary silently shifted by one. The tool exited 0 and wrote a binary dyld
# refused to load ("Symbol not found: dyld_stub_binder").
#
# Chosen behaviour: -delete wins, unconditionally, regardless of where it
# falls relative to a conflicting -change or -reexport. Refusing outright
# would also close the disagreement, but "delete wins" is what falls out of
# unifying on ord_is_deleted's ANY-match semantics (the fix build_lcs and
# mo_map_build now share), needs no new argument-parsing validation, and
# matches this tool's existing stance that -delete is the more definitive of
# the two operations. The point of the test is not which policy was chosen --
# it's that build_lcs and the ordinal map now agree, so the result is a
# binary that actually runs.
# ordinals as linked: 1=libspare, 2=liba, 3=libb (same shape as case 2, so
# the delete side of this also renumbers real survivors, not just no-ops).
"$CC" -O2 $FIXTURE_FLAGS "$T/main.c" "$T/libspare.dylib" "$T/liba.dylib" "$T/libb.dylib" \
    -o "$T/main_conflict"
cp "$T/libspare.dylib" "$T/libspare_renamed.dylib"
"$CHANGE_DYLIB" "$T/main_conflict" \
    -change "@loader_path/libspare.dylib" "@loader_path/libspare_renamed.dylib" \
    -delete "@loader_path/libspare.dylib" >/dev/null 2>&1
rc=$?
deps=$(otool -L "$T/main_conflict")
if [ $rc -eq 0 ] && ! echo "$deps" | grep -q libspare; then
    if out=$(cd "$T" && ./main_conflict 2>"$T/main_conflict.err") && [ "$out" = "33" ]; then
        ok "-change and -delete of the same path: delete wins, still runs (33)"
    else
        bad "-change+-delete conflict" "tool accepted it but the binary is broken: '$out'$( [ -s "$T/main_conflict.err" ] && echo "; stderr: $(cat "$T/main_conflict.err")")"
    fi
elif [ $rc -eq 0 ]; then
    bad "-change+-delete conflict" "tool exited 0 but kept libspare: $deps"
else
    bad "-change+-delete conflict" "tool refused (exit $rc); chosen policy is delete-wins, not refuse"
fi

# --- 8. -delete of the dylib ITSELF named by an LC_LOAD_UPWARD_DYLIB --------
# Regression test for the companion review finding: mo_is_ordinal_lc (which
# the ordinal map is built from) counts LC_LOAD_UPWARD_DYLIB, but build_lcs's
# dylib-matching block used to omit it from the set it can match/delete/rename.
# So -delete naming an upward dylib used to map it to 0 in the map while its
# load command survived untouched in the table -- same class of disagreement
# as case 7 (map says gone, table says present), reached through a different
# load-command kind. Confirmed against the pre-fix binary: it printed only one
# "Delete" line (for libupd_spare; the upward one never matched), left
# LC_LOAD_UPWARD_DYLIB in the table, and silently rebound dyld_stub_binder to
# the survived-by-accident dylib instead of libSystem -- an in-range ordinal
# naming the WRONG library, worse than a load-time refusal.
#
# LC_LOAD_UPWARD_DYLIB only appears on a dylib-to-dylib edge -- ld64 silently
# drops the flag for an executable (confirmed separately) -- and only once the
# referenced dylib already exists on disk for ld to open. This builds that for
# real (libupd_a upward-depends on libupd_b, a plain sibling dylib), a genuine
# linker-produced load command, not a fabricated one.
#
# The ordinal check reads the ordinal VALUE directly (via ordinal_of, above)
# rather than parsing a debug tool's text output or inferring correctness from
# a re-run. Two things were tried and ruled out first:
#   - `nm -m`, grepping its "(from libSystem)" annotation for dyld_stub_binder:
#     passed on this 10.9 host, came back EMPTY on a modern cross-runner --
#     that annotation's shape isn't something this suite can rely on holding
#     across a decade of Xcode, and nothing else here depended on it.
#   - re-running a program that calls into libupd_a.dylib, on the theory that
#     a wrong ordinal must make dyld refuse to load (as it does for case 7,
#     where the wrong ordinal is on the EXECUTABLE's own dyld_stub_binder).
#     Built and ran this for real: a lazily-bound symbol (_getpid) with a
#     wrong-but-in-range ordinal on a DEPENDENCY dylib did NOT crash and did
#     NOT refuse to load on this host -- old two-level-namespace dyld falls
#     back to searching other loaded images for a lazy bind that isn't where
#     its ordinal says, so the broken case silently "worked" too. A test that
#     can't fail on its own bug fixture is worse than no test.
# ordinal_of sidesteps both: it reads GET_LIBRARY_ORDINAL(n_desc) straight out
# of LC_SYMTAB, so it reports what change_dylib actually wrote, not what some
# other tool's formatter or dyld's fallback search happens to paper over.
cat > "$T/upd_a.c" <<'EOF'
#include <unistd.h>
int upd_a_sym(void) { return getpid() > 0 ? 10 : -1; }
EOF
cat > "$T/upd_b.c" <<'EOF'
int upd_b_sym(void) { return 42; }
EOF
"$CC" -dynamiclib -O2 $FIXTURE_FLAGS -install_name "@loader_path/libupd_b.dylib" \
    "$T/upd_b.c" -o "$T/libupd_b.dylib"
"$CC" -dynamiclib -O2 $FIXTURE_FLAGS -install_name "@loader_path/libupd_a.dylib" \
    "$T/upd_a.c" "$T/libspare.dylib" -Xlinker -upward_library -Xlinker "$T/libupd_b.dylib" \
    -o "$T/libupd_a.dylib"
if ! otool -l "$T/libupd_a.dylib" | grep -q LC_LOAD_UPWARD_DYLIB; then
    bad "upward fixture" "linker did not produce LC_LOAD_UPWARD_DYLIB; skipping case 8"
else
    # A pristine copy for case 8b (fix_macho -change) below, taken before
    # case 8's own change_dylib -delete run mutates libupd_a.dylib in place.
    cp "$T/libupd_a.dylib" "$T/libupd_a_for_fixmacho.dylib"
    # `|| true`: without it, a genuinely failing ordinal_of (nonzero exit) would
    # trip `set -e` on this bare assignment and abort the WHOLE script right
    # here -- no bad(), no FAIL line, the EXIT trap deletes ordinal_before.err
    # before anyone reads it, and the only visible symptom is the script's own
    # exit code. Loud in exit status, silent in diagnostics. `|| true` lets
    # execution reach the equality check below, which already turns a failed
    # (empty/wrong) $before into a bad() call with the stderr file's content
    # folded in -- so a genuine failure now actually fails LOUDLY, with a
    # message, instead of just stopping.
    before=$("$T/ordinal_of" "$T/libupd_a.dylib" _getpid 2>"$T/ordinal_before.err") || true
    [ "$before" = "3" ] || bad "upward fixture" "fixture itself not as expected before any rewrite: _getpid ordinal is '$before', wanted 3$( [ -s "$T/ordinal_before.err" ] && echo "; stderr: $(cat "$T/ordinal_before.err")")"
    # ordinals as linked: 1=libspare, 2=libupd_b (upward), 3=libSystem, so
    # _getpid (a real libSystem call, not foldable by the optimizer) starts
    # at ordinal 3. Deleting 1 and 2 must leave only libSystem, now ordinal 1,
    # with _getpid's nlist entry renumbered to match -- not left stale at 3
    # (now out of range) and not left pointing at whatever load command
    # happens to occupy slot 1 in a table that disagreed with the map.
    #
    # This case used to run with MACHO_NO_VERIFY=1, on the belief that the
    # fixture's plain __TEXT layout did not satisfy mg_plausible's
    # LC_FUNCTION_STARTS heuristic (src/grow.h). It did fail regardless of any
    # rewrite, so the "not something the ordinal fix introduces" half was
    # right -- but the heuristic was never unsatisfied, it never RAN. This
    # fixture is a dylib, dylibs are linked at image base 0, and mg_plausible
    # read that 0 as mi_text_base's "no segment maps the header" sentinel and
    # bailed at its precondition. It refused every dylib on the machine.
    # mi_image_base tells the two apart now, so the gate runs here for real
    # and this case needs no escape hatch: that is the regression test.
    "$CHANGE_DYLIB" "$T/libupd_a.dylib" \
        -delete "@loader_path/libspare.dylib" \
        -delete "@loader_path/libupd_b.dylib" >/dev/null \
        || bad "tool run" "change_dylib failed on the upward-dylib fixture"
    deps=$(otool -L "$T/libupd_a.dylib")
    if echo "$deps" | grep -Eq 'libspare|libupd_b'; then
        bad "-delete upward" "libspare or libupd_b still present: $deps"
    else
        ok "-delete: an LC_LOAD_UPWARD_DYLIB is matched/deleted like any other dylib LC"
    fi
    # `|| true`: same reason as $before above -- otherwise a failing ordinal_of
    # here would trip `set -e` and abort the script before the `case` below
    # (which already treats a non-numeric $after, stderr folded in, as a
    # loud bad()) ever runs.
    after=$("$T/ordinal_of" "$T/libupd_a.dylib" _getpid 2>"$T/ordinal_after.err") || true
    case "$after" in
        [0-9]*)
            if [ "$after" = "1" ]; then
                ok "-delete: _getpid's ordinal renumbered to the surviving libSystem (1)"
            else
                bad "-delete upward renumber" "_getpid's ordinal is $after, expected 1"
            fi
            ;;
        *)
            bad "-delete upward renumber" "ordinal_of returned no number, not a wrong number: '$after'$( [ -s "$T/ordinal_after.err" ] && echo "; stderr: $(cat "$T/ordinal_after.err")")"
            ;;
    esac
fi

# --- 8b. fix_macho -change must rewrite an LC_LOAD_UPWARD_DYLIB too ----------
# Companion to case 8. It was written against compat/fix_macho.c's own
# independent dylib-LC set: that file hand-listed {LOAD, WEAK, ID, REEXPORT}
# for -change and silently omitted LC_LOAD_UPWARD_DYLIB, so `fix_macho
# -change` on this exact fixture left libupd_a.dylib's upward dependency
# untouched and printed "No changes needed" (exit 0) while change_dylib (case
# 8, above) rewrote the identical load command. Two answers to one question.
#
# WHAT CHANGED, AND WHY THIS CASE STAYS. fix_macho is a /bin/sh wrapper now
# (compat/fix_macho.sh), so there is only ONE answer left: both tools reach
# mo_is_ordinal_lc through mr_apply_file. The question the case asks is
# therefore no longer "do these two agree" but "does the surviving
# implementation still recognize an upward dylib" -- which is worth pinning
# either way, and is the reason this is an update rather than a deletion.
#
# The "No changes needed" oracle had to change with it: that line was
# fix_macho's own stdout and no longer exists anywhere. Its replacement is
# the unmatched report, `machotool: <path> matched nothing`, on
# STDERR, which says the same thing per operation instead of per run. Note
# `$out` merges both streams, so the check reads either way.
if [ -f "$T/libupd_a_for_fixmacho.dylib" ]; then
    # Same length as the old path (both 27 bytes). This USED to matter because
    # fix_macho refused a replacement that did not fit the existing command
    # ("new path ... too long"); `machotool dylib -replace` resizes into header
    # pad instead, which is the first of compat/fix_macho.sh's five adopted
    # divergences. Keeping the lengths equal anyway keeps this case testing
    # only what it means to -- whether -change recognizes an
    # LC_LOAD_UPWARD_DYLIB at all -- rather than quietly also testing the
    # resize.
    old_install_name="@loader_path/libupd_b.dylib"
    new_install_name="@loader_path/libupd_c.dylib"
    out=$("$FIX_MACHO" "$T/libupd_a_for_fixmacho.dylib" \
        -change "$old_install_name" "$new_install_name" 2>&1) || bad "fix_macho -change upward" "tool run failed: $out"
    # has_bytes (built above, shared with the long-path case further down),
    # not `otool -L | grep`: the same lesson-two oracle this wave's own
    # tests/chained-fixups.sh fix removed elsewhere in this diff. otool -L's
    # dependency-list FORMAT is exactly the kind of thing this project does
    # not control across OS releases; a raw byte-search over the rewritten
    # file's own bytes asks the same question regardless.
    if echo "$out" | grep -q "matched nothing"; then
        bad "fix_macho -change upward" "machotool reported the -change matched nothing -- LC_LOAD_UPWARD_DYLIB not rewritten"
    elif "$T/has_bytes" "$T/libupd_a_for_fixmacho.dylib" "$new_install_name"; then
        ok "fix_macho -change: an LC_LOAD_UPWARD_DYLIB is rewritten like any other dylib LC"
    else
        bad "fix_macho -change upward" "new path ($new_install_name) not found in the rewritten file"
    fi
    if "$T/has_bytes" "$T/libupd_a_for_fixmacho.dylib" "$old_install_name"; then
        bad "fix_macho -change upward" "old path ($old_install_name) still present in the rewritten file"
    fi
else
    bad "fix_macho -change upward" "pristine copy from case 8 missing; case 8 must have skipped"
fi

# --- 9. more operations than the option arrays hold must be refused ----------
# Each option accumulates into a fixed-size array. Without a bounds check the
# writes run off the end into whatever follows -- silently, because nothing
# reads back a length. Only -strip-lc checked, so the rest could overflow.
# One case per array, each one past its capacity.
cap_case() {
    desc=$1; shift
    build_main "$T/main_cap"
    if "$CHANGE_DYLIB" "$T/main_cap" "$@" >/dev/null 2>"$T/cap.err"; then
        bad "$desc" "accepted more operations than the array holds"
    elif grep -qi 'too many' "$T/cap.err"; then
        ok "$desc"
    else
        bad "$desc" "refused, but without a 'too many' diagnostic: $(head -1 "$T/cap.err")"
    fi
}
set -- ; i=0
while [ $i -lt 33 ]; do set -- "$@" -add "@loader_path/libspare.dylib"; i=$((i+1)); done
cap_case "-add beyond capacity is refused" "$@"
set -- ; i=0
while [ $i -lt 33 ]; do set -- "$@" -insert "@loader_path/libspare.dylib"; i=$((i+1)); done
cap_case "-insert beyond capacity is refused" "$@"
set -- ; i=0
while [ $i -lt 33 ]; do set -- "$@" -change "@loader_path/liba.dylib" "@loader_path/libz.dylib"; i=$((i+1)); done
cap_case "-change beyond capacity is refused" "$@"
set -- ; i=0
while [ $i -lt 33 ]; do set -- "$@" -add-rpath "/tmp/rp"; i=$((i+1)); done
cap_case "-add-rpath beyond capacity is refused" "$@"
set -- ; i=0
while [ $i -lt 33 ]; do set -- "$@" -delete-rpath "/tmp/rp"; i=$((i+1)); done
cap_case "-delete-rpath beyond capacity is refused" "$@"

# Exactly at capacity must still be accepted -- a check one too eager would
# silently halve what every caller can ask for.
build_main "$T/main_atcap"
set -- ; i=0
while [ $i -lt 32 ]; do set -- "$@" -add "@loader_path/libspare.dylib"; i=$((i+1)); done
if "$CHANGE_DYLIB" "$T/main_atcap" -grow "$@" >/dev/null 2>"$T/atcap.err"; then
    ok "-add exactly at capacity is accepted"
else
    bad "-add at capacity" "refused at the cap: $(head -1 "$T/atcap.err")"
fi

# --- 9b. fix_macho's option arrays had NO bounds check at all ----------------
# The same defect, in the other tool, unfixed until the compat-retirement
# plan's Task 2. docs/PROPOSAL.md records it being found and fixed in
# change_dylib -- "Repeated options wrote past their fixed-size arrays; 33
# -change flags smashed the stack -- fixed, PR #9" -- and that fix only ever
# covered change_dylib; fix_macho's changes[32] and renames[16] were still
# filled by a loop that never checked. A 33rd -change made the pre-fix binary
# die of SIGABRT (exit 134, stack-protector abort), measured on this host.
#
# WHAT CHANGED: there are no arrays any more. compat/fix_macho.c is retired
# and fix_macho is a /bin/sh wrapper, so the caps live in compat/translate.sh's
# mt_room, which counts and refuses before it emits anything. The -change cap
# would ALSO be caught downstream (machotool caps at MR_MAX_OPS too, in different
# words); the -rename_seg cap would NOT, because each pair becomes its own
# `machotool segment` invocation and machotool never sees more than one -- so for
# that half of this case the translation is the only thing enforcing anything,
# which is exactly why both halves stay.
#
# Asserted as "refuses, saying too many, having modified nothing", not as a
# particular exit code, per this suite's own rule about pinning the behaviour
# rather than which guard fired. That phrasing is why these two assertions did
# not have to change with the implementation under them: they always described
# the outcome, never the guard.
fm_cap_case() {
    desc=$1; shift
    build_main "$T/main_fmcap"
    before_fm=$(shasum -a 256 < "$T/main_fmcap" | cut -d' ' -f1)
    if "$FIX_MACHO" "$T/main_fmcap" "$@" >/dev/null 2>"$T/fmcap.err"; then
        bad "$desc" "accepted more operations than the array holds"
    elif grep -qi 'too many' "$T/fmcap.err"; then
        ok "$desc"
    else
        bad "$desc" "refused, but without a 'too many' diagnostic: $(head -1 "$T/fmcap.err")"
    fi
    [ "$(shasum -a 256 < "$T/main_fmcap" | cut -d' ' -f1)" = "$before_fm" ] \
        || bad "$desc" "the input was modified despite the refusal"
}
set -- ; i=0
while [ $i -lt 33 ]; do set -- "$@" -change "@loader_path/liba.dylib" "@loader_path/libz.dylib"; i=$((i+1)); done
fm_cap_case "fix_macho: -change beyond capacity is refused, not a stack smash" "$@"
set -- ; i=0
while [ $i -lt 17 ]; do set -- "$@" -rename_seg __DATA __DATA_R; i=$((i+1)); done
fm_cap_case "fix_macho: -rename_seg beyond capacity is refused, not a stack smash" "$@"

# Exactly at capacity must still be accepted, same reasoning as case 9's.
build_main "$T/main_fmatcap"
set -- ; i=0
while [ $i -lt 32 ]; do set -- "$@" -change "@loader_path/liba.dylib" "@loader_path/libz.dylib"; i=$((i+1)); done
if "$FIX_MACHO" "$T/main_fmatcap" "$@" >/dev/null 2>"$T/fmatcap.err"; then
    ok "fix_macho: -change exactly at capacity is accepted"
else
    bad "fix_macho -change at capacity" "refused at the cap: $(head -1 "$T/fmatcap.err")"
fi

# ...and the same for -rename_seg, which is NEW here. It was never asserted
# while the cap lived in compat/fix_macho.c, and it matters more now than the
# -change one does: nothing downstream counts renames (one `machotool segment`
# invocation per pair), so an off-by-one in compat/translate.sh's mt_room would
# silently halve what a caller can ask for with nothing else to catch it. 16
# pairs must run, which is 16 machotool invocations against the same file.
build_main "$T/main_fmatcap_seg"
set -- ; i=0
while [ $i -lt 16 ]; do set -- "$@" -rename_seg __DATA __DATA_R; i=$((i+1)); done
if "$FIX_MACHO" "$T/main_fmatcap_seg" "$@" >/dev/null 2>"$T/fmatcapseg.err"; then
    ok "fix_macho: -rename_seg exactly at capacity is accepted"
else
    bad "fix_macho -rename_seg at capacity" "refused at the cap: $(head -1 "$T/fmatcapseg.err")"
fi

# --- 10/11. fat binaries in the rewrite path ---------------------------------
# fix_macho walked fat and thin itself; change_dylib understood only thin until
# this same convergence gave both the shared rewriter's fat loop. Both cases
# build a genuine 2-slice fat binary: a real, linked x86_64 executable (the
# same $T/main built above) plus a slice this tool cannot and must not try to
# rewrite -- a syntactically valid but deliberately minimal 32-bit (MH_MAGIC,
# CPU_TYPE_I386) Mach-O, built by hand rather than `clang -arch i386`, which a
# modern toolchain may no longer support at all.
# The fat container itself is assembled by makefat (above), not system lipo,
# for the same host-portability reason -- what lipo will accept is not this
# suite's to pin. makefat's layout was cross-checked by hand during
# development against a real lipo-built fat file and matched byte for byte.
#
# Both cases read the result with fatcheck (above), never otool: fat_header/
# fat_arch's shape is ours to define and trust, not a text formatter's whose
# fields have drifted across Xcode versions before.
cat > "$T/mkslice32.c" <<'EOF'
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <mach-o/loader.h>
int main(int argc, char **argv) {
    uint8_t buf[4096];
    /* fill byte defaults to 0x5A; an optional argv[2] picks a DIFFERENT one
     * so two calls can produce distinguishable blobs -- needed by case 13
     * to tell "arch 0 kept its own bytes" from "arch 0 got arch 2's". */
    int fill = argc > 2 ? (int)strtol(argv[2], NULL, 0) : 0x5A;
    memset(buf, fill, sizeof buf);   /* distinctive, so a corrupting bug shows up */
    struct mach_header *h = (struct mach_header *)buf;
    h->magic = MH_MAGIC;
    h->cputype = CPU_TYPE_I386;
    h->cpusubtype = CPU_SUBTYPE_I386_ALL;
    h->filetype = MH_EXECUTE;
    h->ncmds = 0;
    h->sizeofcmds = 0;
    h->flags = 0;
    FILE *f = fopen(argv[1], "wb");
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
    return 0;
}
EOF
"$CC" -O2 -o "$T/mkslice32" "$T/mkslice32.c"
"$T/mkslice32" "$T/slice32.bin"
"$T/mkslice32" "$T/slice32b.bin" 0x7B

# --- 10. a plain -change on a fat input: both slices land correctly ---------
"$BIN/makefat" "$T/main_fat" "$T/main" 0x1000007 3 12 "$T/slice32.bin" 7 3 12
arch1_before=$("$BIN/fatcheck" archinfo "$T/main_fat" | sed -n '3p')
"$CHANGE_DYLIB" "$T/main_fat" -change "@loader_path/liba.dylib" "@loader_path/liba_fat.dylib" >/dev/null \
    || bad "fat tool run" "change_dylib failed on a fat input"

narch=$("$BIN/fatcheck" archinfo "$T/main_fat" | head -1)
[ "$narch" = "narch=2" ] && ok "fat: narch unchanged (2)" || bad "fat narch" "got '$narch'"

dylibs0=$("$BIN/fatcheck" dylibs "$T/main_fat" 0)
if echo "$dylibs0" | grep -q '^@loader_path/liba_fat\.dylib$'; then
    ok "fat: the x86_64 slice's dylib path was actually changed"
else
    bad "fat dylib change" "x86_64 slice does not name the new path: $dylibs0"
fi

"$BIN/fatcheck" dump "$T/main_fat" 1 "$T/fat_slice1_after.bin"
if cmp -s "$T/slice32.bin" "$T/fat_slice1_after.bin"; then
    ok "fat: the slice this tool cannot understand is preserved byte-for-byte"
else
    bad "fat slice preserved" "the 32-bit slice's bytes changed"
fi

arch1_after=$("$BIN/fatcheck" archinfo "$T/main_fat" | sed -n '3p')
[ "$arch1_before" = "$arch1_after" ] \
    && ok "fat: unmoved slice keeps its original offset and size ($arch1_after)" \
    || bad "fat offset preserved" "arch 1 was '$arch1_before', now '$arch1_after'"

# --- 11. fat + real growth: a later slice must shift, never overlap ---------
# Case 10 never needed mg_grow_header (the new path fit the existing pad), so
# it cannot exercise the "pack sequentially after a growth" branch of the
# reassembly. This forces a real grow (enough -add's that the pad genuinely
# overflows, same idiom as the capacity cases above) so the x86_64 slice's
# size actually changes, and checks the 32-bit slice both moves out of the
# way and still arrives byte-for-byte intact at its new offset.
"$BIN/makefat" "$T/main_fat_grow" "$T/main" 0x1000007 3 12 "$T/slice32.bin" 7 3 12
before_arch0=$("$BIN/fatcheck" archinfo "$T/main_fat_grow" | sed -n '2p')
before_arch0_size=$(echo "$before_arch0" | awk '{print $3}')

set -- ; i=0
while [ $i -lt 32 ]; do
    set -- "$@" -add "@loader_path/libpad_a_pretty_long_synthetic_name_used_only_to_force_real_header_growth_$i.dylib"
    i=$((i+1))
done
"$CHANGE_DYLIB" "$T/main_fat_grow" -grow "$@" >/dev/null 2>"$T/fatgrow.err" \
    || bad "fat grow tool run" "change_dylib failed: $(head -1 "$T/fatgrow.err")"

after_arch0=$("$BIN/fatcheck" archinfo "$T/main_fat_grow" | sed -n '2p')
after_arch1=$("$BIN/fatcheck" archinfo "$T/main_fat_grow" | sed -n '3p')
after_arch0_size=$(echo "$after_arch0" | awk '{print $3}')
after_arch0_off=$(echo "$after_arch0" | awk '{print $2}')
after_arch1_off=$(echo "$after_arch1" | awk '{print $2}')
after_arch0_end=$((after_arch0_off + after_arch0_size))

if [ "$after_arch0_size" -gt "$before_arch0_size" ]; then
    ok "fat+grow: the x86_64 slice actually grew ($before_arch0_size -> $after_arch0_size)"
else
    bad "fat+grow" "x86_64 slice did not grow: before=$before_arch0_size after=$after_arch0_size"
fi

if [ "$after_arch1_off" -ge "$after_arch0_end" ]; then
    ok "fat+grow: the 32-bit slice moved past the grown x86_64 slice, no overlap"
else
    bad "fat+grow overlap" "arch1 at $after_arch1_off overlaps arch0's end at $after_arch0_end"
fi

# "no overlap" alone is satisfied by ANY packing, aligned or not -- a
# misaligned fat slice is the classic "looks fine, won't load" failure the
# align field exists to prevent, so this needs its own, positional assertion.
# Mutation-tested: with the alignment computation in process_fat replaced by
# `want = cursor` (no rounding at all), this specific check is what fails --
# confirmed by hand during development; the overlap check above still passes.
after_arch1_align=$(echo "$after_arch1" | awk '{print $4}')
if [ $((after_arch1_off % (1 << after_arch1_align))) -eq 0 ]; then
    ok "fat+grow: the shifted 32-bit slice's new offset honors its alignment (2^$after_arch1_align)"
else
    bad "fat+grow alignment" "arch1 at $after_arch1_off is not aligned to 2^$after_arch1_align"
fi

"$BIN/fatcheck" dump "$T/main_fat_grow" 1 "$T/fat_slice1_after_grow.bin"
if cmp -s "$T/slice32.bin" "$T/fat_slice1_after_grow.bin"; then
    ok "fat+grow: the 32-bit slice's bytes are still exactly preserved at its new offset"
else
    bad "fat+grow slice preserved" "the 32-bit slice's bytes changed after the shift"
fi

# --- 12. CRITICAL regression: a descending arch-offset table must not -------
#         corrupt the input or overflow the reassembly buffer.
# Nothing in the fat format requires fat_arch[] to be in ascending file-offset
# order -- lipo/makefat merely happen to emit it that way. A table with
# arch[0] at a HIGHER file offset than arch[1] is legal fat, and both entries
# independently pass an offset+size-in-bounds check.
#
# Reviewed bug this pins: process_fat sized the reassembly buffer from
# `cursor` -- wherever the LAST slice in the loop landed -- instead of the
# MAXIMUM end across every slice. On a descending table the last slice
# processed is the smallest-offset one, so the buffer came out far too small
# for the memcpy of an earlier, higher-offset slice: a heap buffer overflow
# (confirmed with libgmalloc: SIGSEGV) that, WITHOUT a heap-corruption
# detector watching, exited 0 after silently truncating this suite's 74088-
# byte fixture down to 8192 bytes -- the real input gone, no error printed.
# Reproduced against the pre-fix binary by hand during development (see
# task-5-report.md) before writing this regression test.
#
# mkdescfat builds that exact shape: a real linked x86_64 slice at a fixed
# HIGH offset (0x10000) and the 32-bit slice at a fixed LOW offset (0x1000),
# both real (positive) slices, neither overlapping the header/table region.
cat > "$T/mkdescfat.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/fat.h>
static uint8_t *readfile(const char *path, size_t *outsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); exit(2); }
    struct stat st; fstat(fd, &st);
    uint8_t *buf = malloc((size_t)st.st_size);
    if (read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) { perror("read"); exit(2); }
    close(fd);
    *outsz = (size_t)st.st_size;
    return buf;
}
static uint32_t sw32(uint32_t v) {
    return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v & 0xff0000) >> 8) | ((v >> 24) & 0xff);
}
int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s out slice_hi slice_lo\n", argv[0]); return 2; }
    size_t sz_hi, sz_lo;
    uint8_t *b_hi = readfile(argv[2], &sz_hi);
    uint8_t *b_lo = readfile(argv[3], &sz_lo);
    uint32_t off_hi = 0x10000, off_lo = 0x1000;
    uint32_t total = off_hi + (uint32_t)sz_hi;
    uint8_t *out = calloc(1, total);
    struct fat_header *fh = (struct fat_header *)out;
    fh->magic = sw32(FAT_MAGIC);
    fh->nfat_arch = sw32(2);
    struct fat_arch *ar = (struct fat_arch *)(out + sizeof(struct fat_header));
    /* arch[0] is the HIGHER-offset slice -- descending, on purpose */
    ar[0].cputype = sw32(0x1000007); ar[0].cpusubtype = sw32(3);
    ar[0].offset = sw32(off_hi); ar[0].size = sw32((uint32_t)sz_hi); ar[0].align = sw32(12);
    ar[1].cputype = sw32(7); ar[1].cpusubtype = sw32(3);
    ar[1].offset = sw32(off_lo); ar[1].size = sw32((uint32_t)sz_lo); ar[1].align = sw32(12);
    memcpy(out + off_hi, b_hi, sz_hi);
    memcpy(out + off_lo, b_lo, sz_lo);
    int ofd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) { perror("open out"); return 2; }
    if (write(ofd, out, total) != (ssize_t)total) { perror("write"); return 2; }
    close(ofd);
    return 0;
}
EOF
"$CC" -O2 -o "$T/mkdescfat" "$T/mkdescfat.c"
"$T/mkdescfat" "$T/main_descfat" "$T/main" "$T/slice32.bin"
before_size=$(wc -c < "$T/main_descfat" | tr -d ' ')

if "$CHANGE_DYLIB" "$T/main_descfat" \
    -change "@loader_path/liba.dylib" "@loader_path/liba_desc.dylib" >/dev/null 2>"$T/descfat.err"; then
    ok "fat descending-offset: tool ran to completion without crashing"
else
    bad "fat descending-offset run" "change_dylib failed/crashed: $(head -1 "$T/descfat.err")"
fi

after_size=$(wc -c < "$T/main_descfat" | tr -d ' ')
if [ "$after_size" -ge "$before_size" ]; then
    ok "fat descending-offset: output not smaller than input ($before_size -> $after_size bytes)"
else
    bad "fat descending-offset size" "input was $before_size bytes, output is only $after_size -- TRUNCATED"
fi

dylibs_hi=$("$BIN/fatcheck" dylibs "$T/main_descfat" 0)
if echo "$dylibs_hi" | grep -q '^@loader_path/liba_desc\.dylib$'; then
    ok "fat descending-offset: the high-offset slice's dylib path was actually changed"
else
    bad "fat descending-offset dylib" "high-offset slice does not name the new path: $dylibs_hi"
fi

"$BIN/fatcheck" dump "$T/main_descfat" 1 "$T/descfat_lo_after.bin"
if cmp -s "$T/slice32.bin" "$T/descfat_lo_after.bin"; then
    ok "fat descending-offset: the low-offset slice is still preserved byte-for-byte"
else
    bad "fat descending-offset lo slice" "the low-offset slice's bytes changed or are missing"
fi

# --- 13. CRITICAL regression: two REWRITTEN slices landing at the SAME -----
#         output offset must refuse, not silently collide.
# Round 2 review: case 12 closed the memory-safety half of "the arch table
# isn't ascending" (the reassembly buffer could overflow) but left the
# correctness half open. An unshifted slice (e.g. arch[0], first in table
# order) keeps its ORIGINAL offset unconditionally; once some OTHER, earlier-
# in-table-order slice grows, every slice after it packs sequentially from a
# cursor that has no idea where that still-fixed slice sits. On a
# non-ascending table the sequential cursor can walk straight into the fixed
# slice's territory. Reproduced by hand against the pre-fix binary with this
# exact 3-slice fixture: arch 0 and arch 2 both landed at offset 20480,
# arch 2's memcpy silently overwrote arch 0's bytes, and the tool exited 0
# with the fat file "successfully" updated -- one architecture's code gone,
# no error, right file size, right narch.
#
# Engineered precisely rather than hunted for: slice1 is $T/main at offset
# 0x1000, forced (by the same 32-add idiom as cases 11/12) to grow by
# exactly one page, from 8600 to 12696 bytes on THIS host -- confirmed
# exactly this size in case 11 above. That makes the post-growth cursor for
# whatever comes after it (0x1000 + 12696 = 16792, rounded up to its
# 4096-byte alignment) land at EXACTLY 0x5000 (20480) -- so slice0 is placed
# there, fixed, from the start, guaranteeing a collision rather than hoping
# for one -- ON THIS HOST.
#
# Portability trap (this suite's sixth round of one): $T/main's exact
# compiled size is the host compiler's to decide, not this script's. On a
# cross runner it differs, which changes not WHETHER the fixture collides
# but WHICH of the two overlap guards catches it first: if the differently-
# sized slices already overlap as DECLARED, mfat_parse's read-side check
# refuses before the repack ever runs; only if they don't is this the
# write-side check in process_fat's own reassembly. Both are correct
# refusals of the exact same condition -- this asserts the observable
# BEHAVIOUR (refuses, names an overlap, leaves the input untouched), not
# which of the two call sites produced the message, so it passes either
# way. Confirmed the write-side path specifically only on THIS (10.9) host;
# it is not something this test can pin cross-host without controlling the
# compiler's output, which it does not.
cat > "$T/mk3fat.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/fat.h>
static uint8_t *readfile(const char *path, size_t *outsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); exit(2); }
    struct stat st; fstat(fd, &st);
    uint8_t *buf = malloc((size_t)st.st_size);
    if (read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) { perror("read"); exit(2); }
    close(fd);
    *outsz = (size_t)st.st_size;
    return buf;
}
static uint32_t sw32(uint32_t v) {
    return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v & 0xff0000) >> 8) | ((v >> 24) & 0xff);
}
int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s out slice0 slice1 slice2\n", argv[0]); return 2; }
    size_t sz0, sz1, sz2;
    uint8_t *b0 = readfile(argv[2], &sz0);
    uint8_t *b1 = readfile(argv[3], &sz1);
    uint8_t *b2 = readfile(argv[4], &sz2);
    /* slice1 must end (0x1000+sz1) at or before slice2's start, and slice2
     * must end at or before slice0's start -- non-overlapping ORIGINAL
     * layout, required by mfat_parse's own (new) input-side overlap check. */
    uint32_t off0 = 0x5000, off1 = 0x1000, off2 = 0x4000;
    uint32_t total = off0 + (uint32_t)sz0;
    if (off1 + sz1 > total) total = (uint32_t)(off1 + sz1);
    if (off2 + sz2 > total) total = (uint32_t)(off2 + sz2);
    uint8_t *out = calloc(1, total);
    struct fat_header *fh = (struct fat_header *)out;
    fh->magic = sw32(FAT_MAGIC);
    fh->nfat_arch = sw32(3);
    struct fat_arch *ar = (struct fat_arch *)(out + sizeof(struct fat_header));
    ar[0].cputype = sw32(7); ar[0].cpusubtype = sw32(3);          /* opaque, fixed-high */
    ar[0].offset = sw32(off0); ar[0].size = sw32((uint32_t)sz0); ar[0].align = sw32(12);
    ar[1].cputype = sw32(0x1000007); ar[1].cpusubtype = sw32(3);  /* real x86_64, grows */
    ar[1].offset = sw32(off1); ar[1].size = sw32((uint32_t)sz1); ar[1].align = sw32(12);
    ar[2].cputype = sw32(7); ar[2].cpusubtype = sw32(4);          /* opaque, relocates */
    ar[2].offset = sw32(off2); ar[2].size = sw32((uint32_t)sz2); ar[2].align = sw32(12);
    memcpy(out + off0, b0, sz0);
    memcpy(out + off1, b1, sz1);
    memcpy(out + off2, b2, sz2);
    int ofd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) { perror("open out"); return 2; }
    if (write(ofd, out, total) != (ssize_t)total) { perror("write"); return 2; }
    close(ofd);
    return 0;
}
EOF
"$CC" -O2 -o "$T/mk3fat" "$T/mk3fat.c"
"$T/mk3fat" "$T/main_fat3" "$T/slice32.bin" "$T/main" "$T/slice32b.bin"
before_md5=$(md5 -q "$T/main_fat3" 2>/dev/null || md5sum "$T/main_fat3" | awk '{print $1}')

set -- ; i=0
while [ $i -lt 32 ]; do
    set -- "$@" -add "@loader_path/libpad_a_pretty_long_synthetic_name_used_only_to_force_real_header_growth_$i.dylib"
    i=$((i+1))
done
rc=0
"$CHANGE_DYLIB" "$T/main_fat3" -grow "$@" >/dev/null 2>"$T/fat3.err" || rc=$?

if [ $rc -eq 0 ]; then
    # The algorithm never repacks smarter than "sequential from a cursor" --
    # per the fix, this exact layout can only ever be refused, never placed
    # correctly, so a SUCCESSFUL exit here means the collision guard did not
    # run at all, not that a cleverer layout was found.
    bad "fat collision" "tool exited 0 on a layout engineered to collide -- the overlap guard did not fire"
elif grep -qi 'overlap' "$T/fat3.err"; then
    # Deliberately a loose substring, not the write-side message's exact
    # wording ("overlapping offsets"): the read-side guard in mfat_parse
    # ("...or two slices overlapping each other") is an equally correct
    # refusal of the same condition, and which of the two fires is a
    # function of this fixture's host-compiled sizes, not of anything this
    # test controls. See the comment above for why.
    ok "fat collision: refused with a diagnostic naming the overlap (exit $rc)"
else
    bad "fat collision" "refused (exit $rc) but without an overlap diagnostic: $(head -1 "$T/fat3.err")"
fi

after_md5=$(md5 -q "$T/main_fat3" 2>/dev/null || md5sum "$T/main_fat3" | awk '{print $1}')
[ "$before_md5" = "$after_md5" ] \
    && ok "fat collision: input left completely untouched on refusal" \
    || bad "fat collision" "input was modified despite the refusal"

# --- 14. the install must replace the FILE, never the PATH --------------------
# Regression: the mkstemp+rename atomic write (landed alongside case 12/13's
# fat fixes) rename()d over the PATH the caller gave it. When that path is a
# SYMLINK -- exactly the shape of a macOS framework dylib,
# Foo.framework/Foo -> Versions/A/Foo -- rename() replaced the symlink
# itself with a plain file and left the real target (and anything else that
# follows the same symlink) unpatched, while the tool still printed
# "Updated" and exited 0. The same rename-over-path also breaks a file with
# multiple hard links: the sibling name keeps the stale content because
# rename() gives its own name a fresh inode.
#
# THE QUESTION IS THE SAME, THE ANSWERING CODE HAS MOVED. machotool does not write
# FILE at all now; compat/change_dylib.sh does, by installing a temp with mv
# (mw_resolve/mw_prepare/mw_finish, compat/machotool-compat.sh). So the symlink
# case is that install's to get right, and it still does; the hard-link case is
# one mv cannot get right, and 14b below is now the REFUSAL that replaced it.
# Structural reader, not otool text: otool -l's "path X (offset N)" wording
# and its -A2 line spacing both drift across Xcode versions -- this suite
# went red on the modern cross runner seven times during this project, every
# one a test assumption exactly like that. Per tests/README.md ("never parse
# nm/otool human-readable output as an oracle"), read the LC_RPATH load
# commands directly out of the Mach-O and compare the path bytes, so this
# behaves identically on a 2014 and a 2026 toolchain -- same pattern as
# has_lc.c below.
cat > "$T/has_rpath.c" <<'EOF'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
/* Exit 0 if `file` carries an LC_RPATH command whose path is exactly
 * `path`, 1 if it doesn't, 2 on a usage/read error. */
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s file path\n", argv[0]); return 2; }
    const char *want = argv[2];
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 2; }
    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf || read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) {
        fprintf(stderr, "read failed\n"); return 2;
    }
    close(fd);
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    if (hdr->magic != MH_MAGIC_64) { fprintf(stderr, "not a 64-bit Mach-O\n"); return 2; }
    uint8_t *lcp = buf + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_RPATH) {
            struct rpath_command *rc = (struct rpath_command *)lcp;
            const char *p = (const char *)lcp + rc->path.offset;
            if (strcmp(p, want) == 0) return 0;
        }
        lcp += lc->cmdsize;
    }
    return 1;
}
EOF
"$CC" -O2 -o "$T/has_rpath" "$T/has_rpath.c"
rpath_present() { "$T/has_rpath" "$1" "$2"; }

# 14a. symlink: change_dylib is pointed at the LINK; the LINK must still be
# a symlink to the same name afterward, and the REAL file it names must be
# the one that changed. An xattr on the real file (quarantine et al. are
# exactly this) must survive too.
build_main "$T/wa_real"
xattr -w com.machotool.test present "$T/wa_real" 2>/dev/null || true
ln -s wa_real "$T/wa_link"
before_ino=$(stat -f %i "$T/wa_real")
"$CHANGE_DYLIB" "$T/wa_link" -add-rpath /opt/machotool_wa_pad >/dev/null \
    || bad "install symlink" "change_dylib failed"
if [ -L "$T/wa_link" ] && [ "$(readlink "$T/wa_link")" = "wa_real" ]; then
    ok "install: symlink is still a symlink, to the same name"
else
    bad "install symlink" "wa_link is no longer a symlink to wa_real"
fi
after_ino=$(stat -f %i "$T/wa_real")
if rpath_present "$T/wa_real" "/opt/machotool_wa_pad"; then
    ok "install: the REAL target got the change (via the symlink)"
else
    bad "install symlink" "wa_real does not have the new rpath"
fi
[ "$before_ino" != "$after_ino" ] \
    && ok "install: symlink's real target rewritten via mkstemp+rename (fresh inode = atomicity kept)" \
    || bad "install symlink" "wa_real's inode did not change ($before_ino) -- fell back to in-place write instead of the atomic path"
xv=$(xattr -p com.machotool.test "$T/wa_real" 2>/dev/null || echo MISSING)
case "$xv" in
    present) ok "install: xattr on the real target survived" ;;
    MISSING) bad "install symlink" "xattr dropped from the real target" ;;
    *) bad "install symlink" "xattr corrupted: got '$xv'" ;;
esac

# 14b. hard link: two names, one inode. THIS IS NOW A REFUSAL, and the
# assertion is inverted from what it used to be. change_dylib's C tool wrote
# through its own descriptor, so every name for the inode saw the change, and
# wa_write_atomic reproduced that by falling back to an in-place write when
# st_nlink > 1 -- the one path in the old writer that could leave a file half
# written. machotool does not write FILE at all now: the wrapper writes a temp and
# mv's it, which would give this name a fresh inode and leave the sibling on
# the old content. So mw_prepare (compat/machotool-compat.sh) refuses a
# hard-linked FILE up front, with the C tool's flat failure code, rather than
# silently splitting the group -- the one new behaviour a caller of any of these
# wrappers can see, and a trade every one of them makes the same way.
build_main "$T/wa_hard1"
ln "$T/wa_hard1" "$T/wa_hard2"
wa_hard_sha=$(shasum -a 256 < "$T/wa_hard1")
wa_hard_rc=0
"$CHANGE_DYLIB" "$T/wa_hard1" -add-rpath /opt/machotool_wa_hardpad >/dev/null 2>"$T/wa_hard.err" \
    || wa_hard_rc=$?
[ "$wa_hard_rc" -eq 1 ] && grep -q 'hard link' "$T/wa_hard.err" \
    && ok "hard link: a hard-linked FILE is refused (1), saying why" \
    || bad "hard link" "exit $wa_hard_rc: $(cat "$T/wa_hard.err")"
if [ "$(shasum -a 256 < "$T/wa_hard1")" = "$wa_hard_sha" ] \
        && ! rpath_present "$T/wa_hard1" "/opt/machotool_wa_hardpad"; then
    ok "hard link: ... and neither name was touched"
else
    bad "hard link" "the refused run modified the file anyway"
fi
[ "$(stat -f %i "$T/wa_hard1")" = "$(stat -f %i "$T/wa_hard2")" ] \
    && ok "hard link: the group is still one inode, unsplit" \
    || bad "hard link" "wa_hard1 and wa_hard2 no longer share an inode"

# 14c. ordinary case: no symlink, no extra hard link -- must still take the
# atomic mkstemp+rename path (the whole reason wa_write_new writes a temp and
# the wrapper installs it with mv: a write failing partway must never leave a
# half-written binary in place).
build_main "$T/wa_plain"
before_ino=$(stat -f %i "$T/wa_plain")
"$CHANGE_DYLIB" "$T/wa_plain" -add-rpath /opt/machotool_wa_plain >/dev/null \
    || bad "install ordinary" "change_dylib failed"
after_ino=$(stat -f %i "$T/wa_plain")
if rpath_present "$T/wa_plain" "/opt/machotool_wa_plain" && [ "$before_ino" != "$after_ino" ]; then
    ok "install: ordinary case still goes through mkstemp+rename (new inode)"
else
    bad "install ordinary" "expected the change applied via a fresh inode (rpath present=$(rpath_present "$T/wa_plain" "/opt/machotool_wa_plain" && echo y || echo n), inode $before_ino -> $after_ino)"
fi

# --- 15. LC_LAZY_LOAD_DYLIB (legacy -lazy_library) must be an explicit ------
#         REFUSAL, never silent mis-renumbering.
#
# mo_is_ordinal_lc() (src/ordinals.c) treats LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB,
# LC_REEXPORT_DYLIB and LC_LOAD_UPWARD_DYLIB as ordinal-bearing -- the kinds
# this codebase's renumbering has actually been exercised against -- but
# NOT LC_LAZY_LOAD_DYLIB (cmd 0x20, the legacy -lazy_library form), even
# though dyld gives it a library ordinal exactly like LC_LOAD_DYLIB does.
# mg_classify (src/grow.h, used by -grow) already accepts it as inert
# under a base move, which is a different question -- ordinal renumbering,
# not rebasing -- so that acceptance says nothing about renumbering safety.
# Before the fix, mo_map_build simply skipped it while building the old-
# ordinal -> new-ordinal map: any symbol bound to it, or to a dylib load
# command listed AFTER it, silently got the wrong ordinal once -insert or
# -delete renumbered. No crash, no message -- a binary that loads the wrong
# library, or that dyld refuses at launch with no clue why. The fix refuses
# outright the moment mo_map_build sees the load command, saying so.
#
# HOST PORTABILITY: `-lazy_library` is a legacy ld flag; nothing guarantees
# a modern linker still emits LC_LAZY_LOAD_DYLIB for it (or accepts the flag
# at all). This does not assume it does -- it builds the fixture, then reads
# the fixture's OWN load commands with a tiny C reader (never otool/nm text)
# to confirm LC_LAZY_LOAD_DYLIB is actually present before asserting
# anything about change_dylib's behavior on it. If this host's linker didn't
# produce one, that's a fact about the host, not about change_dylib -- SKIP
# loudly rather than pass (or fail) on a fixture that doesn't test what it
# claims to.
cat > "$T/has_lc.c" <<'EOF'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s file cmd-hex\n", argv[0]); return 2; }
    uint32_t want = (uint32_t)strtoul(argv[2], NULL, 16);
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st; fstat(fd, &st);
    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf || read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) {
        fprintf(stderr, "read failed\n"); return 2;
    }
    close(fd);
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    if (hdr->magic != MH_MAGIC_64) { fprintf(stderr, "not a 64-bit Mach-O\n"); return 2; }
    uint8_t *lcp = buf + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == want) return 0;
        lcp += lc->cmdsize;
    }
    return 1;
}
EOF
"$CC" -O2 -o "$T/has_lc" "$T/has_lc.c"

cat > "$T/lazy_a.c" <<'EOF'
int lazy_a_sym(void) { return 77; }
EOF
cat > "$T/lazy_main.c" <<'EOF'
int lazy_a_sym(void);
int main(void) { return lazy_a_sym() == 77 ? 0 : 1; }
EOF
"$CC" -dynamiclib -O2 $FIXTURE_FLAGS -install_name "@loader_path/liblazy_a.dylib" \
    "$T/lazy_a.c" -o "$T/liblazy_a.dylib"
"$CC" -O2 $FIXTURE_FLAGS "$T/lazy_main.c" \
    -Xlinker -lazy_library -Xlinker "$T/liblazy_a.dylib" -o "$T/lazy_main" 2>"$T/lazy_link.err" || true

if [ ! -x "$T/lazy_main" ] || ! "$T/has_lc" "$T/lazy_main" 0x20; then
    skip "LC_LAZY_LOAD_DYLIB refusal" "this host's linker did not produce an LC_LAZY_LOAD_DYLIB from -lazy_library ($(head -1 "$T/lazy_link.err" 2>/dev/null || echo "no diagnostic"))"
else
    before_md5=$(md5 -q "$T/lazy_main" 2>/dev/null || md5sum "$T/lazy_main" | awk '{print $1}')
    rc=0
    "$CHANGE_DYLIB" "$T/lazy_main" -add-rpath /opt/should_never_apply >"$T/lazy_out.txt" 2>"$T/lazy_err.txt" || rc=$?
    after_md5=$(md5 -q "$T/lazy_main" 2>/dev/null || md5sum "$T/lazy_main" | awk '{print $1}')

    [ "$rc" -ne 0 ] \
        && ok "LC_LAZY_LOAD_DYLIB: change_dylib refuses (exit $rc)" \
        || bad "LC_LAZY_LOAD_DYLIB" "change_dylib exited 0 instead of refusing"
    grep -qi "LC_LAZY_LOAD_DYLIB" "$T/lazy_err.txt" \
        && ok "LC_LAZY_LOAD_DYLIB: refusal names the load command, not a generic error" \
        || bad "LC_LAZY_LOAD_DYLIB" "refused without naming LC_LAZY_LOAD_DYLIB: $(cat "$T/lazy_err.txt")"
    [ "$before_md5" = "$after_md5" ] \
        && ok "LC_LAZY_LOAD_DYLIB: input left completely untouched on refusal" \
        || bad "LC_LAZY_LOAD_DYLIB" "input was modified despite the refusal"
fi

# --- 16. src/fat.c's declared-slice overlap check, exercised through -------
#         fix_macho -- its ONLY protection against this.
#
# mfat_parse (src/fat.c:~55-61) walks every DECLARED fat_arch entry and
# refuses if any two overlap each other -- a read-side check, independent of
# what any caller does with the file afterward. Before this check existed
# this exact malformed input silently let change_dylib's reassembly
# corrupt one slice's bytes with another's (case 12/13 above cover THAT,
# the write-side consequence, for change_dylib specifically). compat/
# fix_macho.c had no write-side check of its own to fall back on -- mfat_parse's
# read-side refusal was 100% of what stood between it and indexing into
# overlapping/aliased slice data as if the two slices were independent.
# A prior review deleted this check and the entire suite (32/32 at the time)
# stayed green, because nothing exercised it -- this closes that hole
# directly.
#
# WHAT CHANGED: fix_macho is a /bin/sh wrapper now, so the fat walk under it
# is mr_process_fat (src/rewrite.c), which calls the SAME mfat_parse -- there
# is no longer a second fat reader anywhere in this repo, which is the point
# of the convergence. The case is therefore no longer "the one tool that
# depends on this check"; it is a caller-side exercise of the check itself,
# reached through the grammar that used to be the only way in. It is kept
# rather than folded into case 12/13 because nothing else drives mfat_parse
# from a compat grammar, and because a deleted assertion is how this check got
# silently removed once already.
#
# $FIX_MACHO is $BIN/fix_macho if a bindir was given, or the copy staged in
# $T/bin by the standalone fallback -- either way it is ready to use here,
# with no separate build step needed for this case.

cat > "$T/mk2fat_overlap.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <mach-o/fat.h>
static uint32_t sw32(uint32_t v) {
    return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v & 0xff0000) >> 8) | ((v >> 24) & 0xff);
}
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s out\n", argv[0]); return 2; }
    /* Two DECLARED slices whose byte ranges genuinely intersect:
     * slice0 = [0x1000, 0x3000), slice1 = [0x2000, 0x3000) -- overlap at
     * [0x2000, 0x3000). Neither runs past the file or into the header/arch
     * table, so this exercises ONLY the pairwise overlap check, nothing
     * else mfat_parse also refuses. */
    uint32_t total = 0x3000;
    uint8_t *out = calloc(1, total);
    struct fat_header *fh = (struct fat_header *)out;
    fh->magic = sw32(FAT_MAGIC);
    fh->nfat_arch = sw32(2);
    struct fat_arch *ar = (struct fat_arch *)(out + sizeof(struct fat_header));
    ar[0].cputype = sw32(7); ar[0].cpusubtype = sw32(3);
    ar[0].offset = sw32(0x1000); ar[0].size = sw32(0x2000); ar[0].align = sw32(12);
    ar[1].cputype = sw32(0x1000007); ar[1].cpusubtype = sw32(3);
    ar[1].offset = sw32(0x2000); ar[1].size = sw32(0x1000); ar[1].align = sw32(12);
    int ofd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) { perror("open out"); return 2; }
    if (write(ofd, out, total) != (ssize_t)total) { perror("write"); return 2; }
    close(ofd);
    return 0;
}
EOF
"$CC" -O2 -o "$T/mk2fat_overlap" "$T/mk2fat_overlap.c"
"$T/mk2fat_overlap" "$T/fat_two_declared_slices"
before_md5=$(md5 -q "$T/fat_two_declared_slices" 2>/dev/null || md5sum "$T/fat_two_declared_slices" | awk '{print $1}')

rc=0
"$FIX_MACHO" "$T/fat_two_declared_slices" -strip_build_version >"$T/overlap_fix.out" 2>&1 || rc=$?
after_md5=$(md5 -q "$T/fat_two_declared_slices" 2>/dev/null || md5sum "$T/fat_two_declared_slices" | awk '{print $1}')

[ "$rc" -ne 0 ] \
    && ok "fat declared-overlap: fix_macho refuses (exit $rc)" \
    || bad "fat declared-overlap" "fix_macho exited 0 on two declared slices that overlap each other"
grep -qi "overlapping" "$T/overlap_fix.out" \
    && ok "fat declared-overlap: refusal names the overlap" \
    || bad "fat declared-overlap" "refused without mentioning overlap: $(cat "$T/overlap_fix.out")"
[ "$before_md5" = "$after_md5" ] \
    && ok "fat declared-overlap: input left completely untouched on refusal" \
    || bad "fat declared-overlap" "input was modified despite the refusal"

# --- 17. heap overflow on a long -change replacement path -------------------
#
# process_one sizes its scratch buffer (new_lcs) as `first_sect_off +
# add_bytes + 64`, where add_bytes used to count only the bytes -add/-insert/
# -add-rpath contribute -- NOT -change/-change-rpath, even though build_lcs
# happily grows a MATCHED command to `base + strlen(new_path)` (rounded up),
# keeping whichever is larger of that or the original cmdsize. A long enough
# -change replacement made build_lcs write past the end of a buffer sized
# for a change that never happened: repro `change_dylib bin -change
# /usr/lib/libSystem.B.dylib <9000 chars>` -> SIGSEGV under libgmalloc.
# Pre-existing (present in 868e2a6, long before this branch), fixed here by
# including -change/-change-rpath's replacement lengths in add_bytes too --
# see the comment on that calculation in change_dylib.c.
#
# The overflow happens INSIDE build_lcs, before process_one's own "does it
# fit the header pad" check ever runs -- so it reproduced with or without
# -grow (confirmed by hand against the pre-fix binary, both ways, under
# libgmalloc: SIGSEGV either way). This suite doesn't run under libgmalloc
# itself (heap corruption without a detector watching can silently succeed
# instead of crashing -- the same reasoning as case 12's comment), so this
# asserts observable BEHAVIOR: the tool never crashes (a shell only reports
# a plain nonzero exit for a refusal, never the 128+signal shape a SIGSEGV
# produces) and, when the write does go through (-grow, so it fits), the
# resulting file actually contains the long path intact and nothing else
# looks truncated. Confirmed separately by hand, under
# DYLD_INSERT_LIBRARIES=libgmalloc.dylib: the pre-fix binary SIGSEGVs
# (exit 139) on this exact repro, with or without -grow; the fixed binary
# exits cleanly both ways.
build_main "$T/longchange_fixture"
LONG_PATH=$(printf 'Q%.0s' $(seq 1 9000))

# has_bytes was already built above (right after ordinal_of), shared with
# case 8b -- checked with a tiny C byte-search (memmem), not grep: this
# host's `grep` (ugrep) reports "out of memory" trying to fixed-string-match
# a 9000-byte pattern against a binary file -- a grep quirk, not a
# change_dylib one, but a good reminder that even a non-otool/nm text tool
# can ask a different question (or none at all) depending on what's on a
# given host's PATH.

# Without -grow: must refuse cleanly (header pad can't possibly hold a
# 9000-byte path), never crash.
rc=0
"$CHANGE_DYLIB" "$T/longchange_fixture" -change "@loader_path/liba.dylib" "$LONG_PATH" \
    >/dev/null 2>"$T/longchange_noG.err" || rc=$?
if [ "$rc" -gt 127 ]; then
    bad "long -change (no -grow)" "tool was killed by a signal (exit $rc) -- looks like the heap overflow"
elif [ "$rc" -eq 0 ]; then
    bad "long -change (no -grow)" "expected a clean refusal (header pad can't hold 9000 bytes) but exited 0"
else
    ok "long -change (no -grow): refused cleanly (exit $rc), no crash"
fi

# With -grow: must succeed, and the long path must land in the file intact.
rc=0
"$CHANGE_DYLIB" "$T/longchange_fixture" -grow -change "@loader_path/liba.dylib" "$LONG_PATH" \
    >/dev/null 2>"$T/longchange_G.err" || rc=$?
if [ "$rc" -gt 127 ]; then
    bad "long -change (-grow)" "tool was killed by a signal (exit $rc) -- the heap overflow"
elif [ "$rc" -ne 0 ]; then
    bad "long -change (-grow)" "expected success with -grow but exited $rc: $(cat "$T/longchange_G.err")"
else
    ok "long -change (-grow): completed without crashing (exit 0)"
fi
if "$T/has_bytes" "$T/longchange_fixture" "$LONG_PATH"; then
    ok "long -change (-grow): the full 9000-byte replacement path landed intact"
else
    bad "long -change (-grow)" "the long replacement path is not intact in the output file"
fi

# --- 18. heap overflow when TWO load commands share an install name and one
#     -change matches both ---------------------------------------------------
#
# Case 17 fixed add_bytes to account for -change/-change-rpath growth at all,
# but it still budgeted "one grown command per -change/-change-rpath
# ARGUMENT" -- and build_lcs's matching loop grows EVERY load command that
# matches, not just one. Two LC_LOAD_DYLIBs can legitimately carry the same
# install name (nothing in the format forbids it), so a single -change for
# that name needs budget for TWO grown commands, and the per-argument budget
# gave it one. Confirmed as a real heap buffer overflow at both 3000 and
# 9000-char replacement paths under DYLD_INSERT_LIBRARIES=libgmalloc.dylib
# against the pre-fix binary: exit 139 (SIGSEGV) both times; without
# libgmalloc, the corruption doesn't crash (same reasoning as case 17's own
# comment on why this suite otherwise avoids running under libgmalloc: heap
# corruption without a detector watching can silently succeed) -- which is
# exactly why the assertion below runs THIS ONE case under libgmalloc itself
# rather than relying on a by-hand confirmation. Fixed by change_growth_bytes,
# which walks the REAL load commands instead of the -change arguments -- see
# its own comment in change_dylib.c for the one (safe, over- not under-)
# approximation it still makes.
#
# The fixture needs two commands sharing a name, which a normal link never
# produces -- ld itself resolves a second dylib against the first one it
# already loaded under the same install name, so only one LC_LOAD_DYLIB ever
# gets emitted (confirmed by hand: linking two distinct .dylib files built
# with an identical -install_name still yields exactly one LC_LOAD_DYLIB).
# This instead uses -insert to add a SECOND "@loader_path/liba.dylib"
# LC_LOAD_DYLIB onto a binary that already links liba.dylib normally --
# `-insert` never checks for an existing match, so it happily produces the
# duplicate, and does so through the tool's own tested code path rather than
# hand-built bytes.
build_main "$T/dupname_fixture"
"$CHANGE_DYLIB" "$T/dupname_fixture" -grow -insert "@loader_path/liba.dylib" >/dev/null \
    || bad "dupname fixture setup" "-insert failed unexpectedly"
dup_count=$(otool -l "$T/dupname_fixture" | grep -c "name @loader_path/liba.dylib")
[ "$dup_count" -eq 2 ] \
    && ok "dupname fixture: two LC_LOAD_DYLIBs now share an install name" \
    || bad "dupname fixture" "expected 2 load commands named @loader_path/liba.dylib, otool shows $dup_count"

DUP_LONG_PATH=$(printf 'Z%.0s' $(seq 1 3000))

# Plain run (no libgmalloc): proves correct BEHAVIOR -- no crash, and both
# matching commands actually got renamed, not just one silently dropped or
# truncated.
rc=0
"$CHANGE_DYLIB" "$T/dupname_fixture" -grow -change "@loader_path/liba.dylib" "$DUP_LONG_PATH" \
    >"$T/dupname_change.out" 2>"$T/dupname_change.err" || rc=$?
if [ "$rc" -gt 127 ]; then
    bad "dup-install-name -change" "tool was killed by a signal (exit $rc) -- the heap overflow this case exists to catch"
elif [ "$rc" -ne 0 ]; then
    bad "dup-install-name -change" "expected success but exited $rc: $(cat "$T/dupname_change.err")"
else
    ok "dup-install-name -change: completed without crashing (exit 0)"
fi
new_count=$(otool -l "$T/dupname_fixture" | grep -c "name $DUP_LONG_PATH")
[ "$new_count" -eq 2 ] \
    && ok "dup-install-name -change: BOTH matching load commands were renamed, not just one" \
    || bad "dup-install-name -change" "expected both duplicate commands renamed (2 occurrences), found $new_count"

# libgmalloc run: this is the assertion that actually DISCRIMINATES the bug --
# guard-malloc places each allocation so an overrun faults immediately instead
# of landing in unrelated heap memory, which is what makes the corrupted-but-
# doesn't-crash outcome above insufficient proof on its own. Skipped (loudly,
# not silently) if this host has no libgmalloc.
if [ -f /usr/lib/libgmalloc.dylib ]; then
    build_main "$T/dupname_fixture_gm"
    "$CHANGE_DYLIB" "$T/dupname_fixture_gm" -grow -insert "@loader_path/liba.dylib" >/dev/null \
        || bad "dupname fixture setup (libgmalloc copy)" "-insert failed unexpectedly"
    rc=0
    DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib \
        "$CHANGE_DYLIB" "$T/dupname_fixture_gm" -grow -change "@loader_path/liba.dylib" "$DUP_LONG_PATH" \
        >"$T/dupname_gm.out" 2>"$T/dupname_gm.err" || rc=$?
    if [ "$rc" -gt 127 ]; then
        bad "dup-install-name -change (libgmalloc)" "killed by a signal (exit $rc) under libgmalloc -- the heap overflow this case exists to catch"
    elif [ "$rc" -ne 0 ]; then
        bad "dup-install-name -change (libgmalloc)" "expected success but exited $rc: $(cat "$T/dupname_gm.err")"
    else
        ok "dup-install-name -change (libgmalloc): completed without crashing (exit 0)"
    fi
else
    skip "dup-install-name -change (libgmalloc)" "no /usr/lib/libgmalloc.dylib on this host"
fi

# --- 19. CRITICAL: build_lcs's malformed-LC_RPATH refusal must still ---------
#     refuse, and refuse WITHOUT writing anything, now that its walk runs
#     through the stop-capable mi_each_lc (Task 2a) instead of a hand-rolled
#     loop with its own `return -1`.
#
# build_lcs_lc (change_dylib.c) calls mo_lc_str_at on every LC_RPATH's own
# path.offset and refuses the whole rewrite if the offset is out of bounds
# for that command's cmdsize -- BEFORE this task, that early exit worked
# because the walk was hand-rolled and could just `return -1` straight out
# of the loop. After converting it to an mi_each_lc callback, "stop" means
# the callback returns 1 and mi_each_lc reports incomplete; build_lcs then
# has to translate that into its own -1. If that translation were wrong (or
# missing -- e.g. build_lcs treating "stopped early" the same as "finished
# normally"), the tool would use whatever partial `new_lcs` the callback had
# written up to the point it detected the corruption, and either write a
# truncated/corrupt load-command table or silently continue past a load
# command it could not safely interpret. This is exactly the failure mode
# the task brief calls out as "the worst possible outcome here".
#
# The fixture: link a real binary with one valid LC_RPATH (through
# change_dylib itself, so the command is genuinely well-formed to start),
# then use a tiny C patcher to corrupt ONLY that command's path.offset field
# in place to a value >= its own cmdsize -- everything else about the file,
# including cmdsize/ncmds/alignment, stays exactly what a real link produced,
# so mi_open's own structural validation still accepts it (offset-into-a-
# command bounds is deliberately NOT something mi_validate checks -- that is
# mo_lc_str_at's job, at the point something actually tries to read the
# string). Any op that reaches build_lcs's per-command walk (a plain
# -add-rpath here) must then hit this LC_RPATH and refuse, regardless of
# whether that op targets the corrupted command at all -- the check in
# build_lcs_lc fires unconditionally for every LC_RPATH it walks past, not
# just ones matched by name.
cat > "$T/corrupt_rpath_offset.c" <<'EOF'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
/* Rewrites the FIRST LC_RPATH's path.offset to cmdsize (one byte past the
 * command's own end -- mo_lc_str_at's bound is `offset >= cmdsize`, so this
 * is minimally out of range, not wildly so). Exit 0 on success, 2 if no
 * LC_RPATH was found (a fixture-building bug, not the thing under test). */
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s file\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); return 2; }
    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf || read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) {
        fprintf(stderr, "read failed\n"); return 2;
    }
    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    if (hdr->magic != MH_MAGIC_64) { fprintf(stderr, "not a 64-bit Mach-O\n"); return 2; }
    uint8_t *lcp = buf + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_RPATH) {
            struct rpath_command *rc = (struct rpath_command *)lcp;
            rc->path.offset = rc->cmdsize;   /* out of bounds by exactly 1 */
            if (pwrite(fd, buf, (size_t)st.st_size, 0) != (ssize_t)st.st_size) {
                perror("pwrite"); return 2;
            }
            close(fd);
            return 0;
        }
        lcp += lc->cmdsize;
    }
    fprintf(stderr, "no LC_RPATH found\n");
    return 2;
}
EOF
"$CC" -O2 -o "$T/corrupt_rpath_offset" "$T/corrupt_rpath_offset.c"

build_main "$T/bad_rpath_fixture"
"$CHANGE_DYLIB" "$T/bad_rpath_fixture" -add-rpath /orig/rp >/dev/null \
    || bad "bad-rpath-offset fixture setup" "-add-rpath failed unexpectedly"
rpath_present "$T/bad_rpath_fixture" "/orig/rp" \
    && ok "bad-rpath-offset fixture: starts with one well-formed LC_RPATH" \
    || bad "bad-rpath-offset fixture" "the LC_RPATH -add-rpath just wrote is not there"
"$T/corrupt_rpath_offset" "$T/bad_rpath_fixture" \
    || bad "bad-rpath-offset fixture" "corrupt_rpath_offset helper failed"

before_md5=$(md5 -q "$T/bad_rpath_fixture" 2>/dev/null || md5sum "$T/bad_rpath_fixture" | awk '{print $1}')
rc=0
"$CHANGE_DYLIB" "$T/bad_rpath_fixture" -add-rpath /another/rp \
    >"$T/bad_rpath.out" 2>"$T/bad_rpath.err" || rc=$?
after_md5=$(md5 -q "$T/bad_rpath_fixture" 2>/dev/null || md5sum "$T/bad_rpath_fixture" | awk '{print $1}')

[ "$rc" -ne 0 ] \
    && ok "bad-rpath-offset: build_lcs still refuses (exit $rc)" \
    || bad "bad-rpath-offset" "change_dylib exited 0 against a malformed LC_RPATH offset"
grep -qi "malformed LC_RPATH" "$T/bad_rpath.err" \
    && ok "bad-rpath-offset: refusal names the malformed LC_RPATH, not a generic error" \
    || bad "bad-rpath-offset" "refused without naming the malformed LC_RPATH: $(cat "$T/bad_rpath.err")"
[ "$before_md5" = "$after_md5" ] \
    && ok "bad-rpath-offset: input left completely untouched on refusal" \
    || bad "bad-rpath-offset" "input was modified despite the refusal"

# --- 20. THE MIXED-FAMILY DOUBLE GROW: does two machotool calls cost what one --
#     used to? compat/README.md claimed the rewritten bytes are identical to
#     the C tools' with ONE known exception (LC_LAZY_LOAD_DYLIB). Review found
#     a second, structural gap that claim did not cover: a MIXED-FAMILY old
#     invocation with -grow -- one that touches both the dylib table and the
#     rpath table -- becomes TWO machotool invocations (compat/translate.sh
#     emits a `dylib --allow-grow` line and a `rpath --allow-grow` line, in
#     that order), where compat/change_dylib.c used to build ONE mr_ops
#     carrying both families and call mr_apply_file ONCE. mg_grow_header
#     rounds each request up to a whole page, so growing twice for deltas a
#     and b can cost ceil(a/P) + ceil(b/P) pages where growing once for the
#     summed delta would have cost only ceil((a+b)/P) -- the two differ
#     whenever a's and b's within-page remainders sum past P.
#
# compat/change_dylib.c is gone, so "what would one pass have produced" is
# answered here by a harness that does exactly what that C tool's main() used
# to: parse everything into one mr_ops and call the shared rewriter (the same
# mr_apply_file this build's machotool calls) exactly once. That is the fair
# baseline, not a stand-in for it -- both routes below run the identical
# rewrite code, just a different number of times.
cat > "$T/one_pass.c" <<'EOF'
#include <string.h>
#include "rewrite.h"
/* one_pass FILE OUT OLD-DYLIB NEW-DYLIB NEW-RPATH -- one mr_apply_file call
 * carrying both a dylib change and an rpath append, exactly what
 * compat/change_dylib.c's main() used to build from
 * `-change OLD NEW -add-rpath NEW-RPATH -grow`. It writes OUT, not FILE:
 * mr_apply_file never writes the file it is given, so the caller installs
 * OUT the same way the compat wrappers do. */
int main(int argc, char **argv) {
    if (argc != 6) return 2;
    mr_change ch;
    ch.old_path = argv[3]; ch.new_path = argv[4]; ch.reexport = 0;
    const char *radd[1];
    radd[0] = argv[5];
    mr_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.dylib_changes = &ch;  ops.n_dylib_changes = 1;
    ops.rpath_appends = radd; ops.n_rpath_appends = 1;
    ops.allow_grow = 1;
    return mr_apply_file(argv[1], argv[2], &ops);
}
EOF
"$CC" -O2 -Wall -I "$SRC_DIR" -o "$T/one_pass" "$T/one_pass.c" "$SRC_DIR"/*.c \
    2>"$T/one_pass_build.err" \
    || bad "mixed-family double grow" "one_pass helper failed to build: $(cat "$T/one_pass_build.err")"

# 3000 bytes is already proven (case 17 above, DUP_LONG_PATH) enough to force
# this fixture's dylib table past its header pad and into ONE page-grow. That
# one grow leaves several kB of fresh pad behind it (mg_grow_header rounds up
# to a whole page), so the rpath addition has to ask for MORE than that
# leftover to force a SECOND grow rather than just fitting in the first one's
# slack -- 9000 bytes is already proven (LONG_PATH, above) to force a grow
# from a bare fixture, which this leftover pad is smaller than.
GROW_DYLIB=$(printf 'D%.0s' $(seq 1 3000))
GROW_RPATH=$(printf 'R%.0s' $(seq 1 9000))

build_main "$T/g_two"
cp "$T/g_two" "$T/g_one"

# Route A: the SHIPPED route -- the real compat/change_dylib.sh wrapper,
# exactly as a caller invokes it. This is not a simulation of what
# compat/translate.sh emits; it is that emission, run. It is ONE machotool
# command now (`edit`, with `allow-grow` and one statement per family), but
# still two rewrites of the image, which is what this case is about: each
# statement is its own pass and so its own chance to grow.
rc=0
"$CHANGE_DYLIB" "$T/g_two" -grow -change "@loader_path/liba.dylib" "$GROW_DYLIB" -add-rpath "$GROW_RPATH" \
    >"$T/g_two.out" 2>"$T/g_two.err" || rc=$?
[ "$rc" -eq 0 ] \
    && ok "mixed-family double grow: the shipped two-pass route succeeds" \
    || bad "mixed-family double grow" "the shipped two-pass route failed (exit $rc): $(cat "$T/g_two.err")"
two_grows=$(grep -c "grew header pad" "$T/g_two.out")
[ "$two_grows" -eq 2 ] \
    && ok "mixed-family double grow: the shipped route grows the header TWICE (measured, not assumed)" \
    || bad "mixed-family double grow" "expected 2 \"grew header pad\" lines from the shipped route, saw $two_grows: $(cat "$T/g_two.out")"

# Route B: ONE mr_apply_file call carrying both families -- growing once for
# the summed delta, the pre-wrapper C tool's shape.
rc=0
"$T/one_pass" "$T/g_one" "$T/g_one.new" "@loader_path/liba.dylib" "$GROW_DYLIB" "$GROW_RPATH" \
    >"$T/g_one.out" 2>"$T/g_one.err" || rc=$?
[ "$rc" -ne 0 ] || mv -f "$T/g_one.new" "$T/g_one"
[ "$rc" -eq 0 ] \
    && ok "mixed-family double grow: the one-call route succeeds" \
    || bad "mixed-family double grow" "the one-call route failed (exit $rc): $(cat "$T/g_one.err")"
one_grows=$(grep -c "grew header pad" "$T/g_one.out")
[ "$one_grows" -eq 1 ] \
    && ok "mixed-family double grow: the one-call route grows the header ONCE" \
    || bad "mixed-family double grow" "expected 1 \"grew header pad\" line from the one-call route, saw $one_grows: $(cat "$T/g_one.out")"

# Growing twice is a SIZE question, not a correctness one -- both results
# still have to be images machotool itself accepts, and both have to actually
# carry what was asked for.
"$MACHOTOOL" verify "$T/g_two" >/dev/null 2>"$T/g_two_verify.err" \
    && ok "mixed-family double grow: the two-pass route's result still verifies" \
    || bad "mixed-family double grow" "the two-pass route's result failed machotool verify: $(cat "$T/g_two_verify.err")"
"$MACHOTOOL" verify "$T/g_one" >/dev/null 2>"$T/g_one_verify.err" \
    && ok "mixed-family double grow: the one-call route's result still verifies" \
    || bad "mixed-family double grow" "the one-call route's result failed machotool verify: $(cat "$T/g_one_verify.err")"
"$T/has_bytes" "$T/g_two" "$GROW_RPATH" && "$T/has_bytes" "$T/g_two" "$GROW_DYLIB" \
    && ok "mixed-family double grow: the two-pass route's result carries both new strings" \
    || bad "mixed-family double grow" "the two-pass route's result is missing the new dylib path and/or rpath"
"$T/has_bytes" "$T/g_one" "$GROW_RPATH" && "$T/has_bytes" "$T/g_one" "$GROW_DYLIB" \
    && ok "mixed-family double grow: the one-call route's result carries both new strings" \
    || bad "mixed-family double grow" "the one-call route's result is missing the new dylib path and/or rpath"

# THE QUESTION ITSELF: does growing twice cost, and produce, what growing
# once would have? Recorded either way -- neither answer would be a bug in
# this branch, since nothing machotool offers combines both families into one
# mr_apply_file call today. `machotool edit` puts them in one INVOCATION, and
# one write, but still runs a pass per statement (src/edit.c says why it does
# not batch), so both grows still happen. Full byte comparison, not just size: mg_grow_header
# grows by the EXCESS over the pad IT SEES AT THAT MOMENT, rounded up to a
# whole page ("load commands need N more bytes than the M-byte pad" above),
# not by a fixed page count computed from the operation's own delta alone.
# Because a grow always leaves behind a whole number of pages, the leftover
# it hands to the NEXT call composes losslessly with that call's own excess:
# ceil(e1/P)*P, then ceil(e2 - leftover/P)*P from there, lands on the exact
# same page count as ceil((e1+e2)/P)*P computed once -- ceil distributes over
# an already-page-aligned addend. That is a property of this growth
# algorithm, not a coincidence of this fixture, but it is verified here only
# for this one case (two ops, dylib then rpath, both needing to grow); it is
# not a claim about three or more mixed families, a fat container, or either
# order producing byte-IDENTICAL results.
two_size=$(wc -c < "$T/g_two" | tr -d ' ')
one_size=$(wc -c < "$T/g_one" | tr -d ' ')
if cmp -s "$T/g_two" "$T/g_one"; then
    ok "mixed-family double grow: RESULT -- byte-identical to the one-call route ($two_size bytes) on this fixture; growing twice cost exactly what growing once would have"
elif [ "$two_size" -eq "$one_size" ]; then
    bad "mixed-family double grow" "same size ($two_size bytes) but the bytes differ -- same total growth, different layout"
elif [ "$two_size" -gt "$one_size" ]; then
    ok "mixed-family double grow: RESULT -- the two-pass route is $((two_size - one_size)) bytes LARGER ($two_size vs $one_size); growing twice cost a whole extra page here, the ceil(a/P)+ceil(b/P) > ceil((a+b)/P) case made concrete rather than theoretical"
else
    bad "mixed-family double grow" "the two-pass route ($two_size bytes) is SMALLER than the one-call route ($one_size bytes) -- growing twice should never cost less than growing once for the same total delta"
fi

echo
[ "$fails" -eq 0 ] && { echo "change_dylib_test: all cases pass"; exit 0; }
echo "change_dylib_test: $fails FAILED"; exit 1
