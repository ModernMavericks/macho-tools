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
#   ./change_dylib_test.sh          (needs only clang + otool)
set -e
cd "$(dirname "$0")"
CC="${CC:-clang}"

# The FIXTURES must be 10.9-targeted, not host-targeted. A modern linker emits
# LC_DYLD_CHAINED_FIXUPS by default, and change_dylib refuses those on purpose --
# 10.9's dyld cannot read them, which is why patch_macho exists. Without this the
# suite passes on 10.9 and fails on a modern runner, having silently changed what
# it tests. -mmacosx-version-min=10.9 gets the classic LC_DYLD_INFO_ONLY form on
# either host, so the test asks the same question everywhere.
#
# Note this applies only to the fixtures. change_dylib itself (line below) is a
# host tool and is built for the host.
FIXTURE_FLAGS="-mmacosx-version-min=10.9"
T="${TMPDIR:-/tmp}/change_dylib_test.$$"
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT INT TERM

# Builds change_dylib from source rather than consuming a CMake target, so this
# script keeps working standalone (`./change_dylib_test.sh`, clang + otool only).
# That means it must track what change_dylib includes: macho_grow.h now pulls in
# src/uleb.h, and change_dylib.c itself now includes src/ordinals.h, so the
# toolkit sources it needs are listed here too.
"$CC" -O2 -I src -o "$T/change_dylib" change_dylib.c src/uleb.c src/image.c src/ordinals.c
fails=0
ok()   { echo "PASS $1"; }
bad()  { echo "FAIL $1: $2"; fails=$((fails+1)); }

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
"$T/change_dylib" "$T/main_ins" -grow -insert "@loader_path/libspare.dylib" >/dev/null || bad "tool run" "change_dylib failed"
first=$(otool -L "$T/main_ins" | sed -n '2p' | awk '{print $1}')
case "$first" in
    *libspare.dylib) ok "-insert: libspare is the first dependency" ;;
    *) bad "-insert order" "first dep is '$first'" ;;
esac
if out=$(cd "$T" && ./main_ins 2>&1) && [ "$out" = "33" ]; then
    ok "-insert: renumbered, binary still resolves a_sym/b_sym (33)"
else
    bad "-insert renumber" "got '$out'"
fi

# --- 2. -delete of an EARLIER dylib renumbers the survivors -------------------
# libspare is linked first but unreferenced; deleting it shifts liba 2->1,
# libb 3->2. Without renumbering, a_sym would be looked up in libb.
"$CC" -O2 $FIXTURE_FLAGS "$T/main.c" "$T/libspare.dylib" "$T/liba.dylib" "$T/libb.dylib" -o "$T/main_del"
out=$(cd "$T" && ./main_del) && [ "$out" = "33" ] \
    || bad "delete fixture" "fixture itself broken: '$out'"
"$T/change_dylib" "$T/main_del" -delete "@loader_path/libspare.dylib" >/dev/null || bad "tool run" "change_dylib failed"
otool -L "$T/main_del" | grep -q libspare \
    && bad "-delete" "libspare still present" \
    || ok "-delete: libspare removed"
if out=$(cd "$T" && ./main_del 2>&1) && [ "$out" = "33" ]; then
    ok "-delete: renumbered, survivors still resolve (33)"
else
    bad "-delete renumber" "got '$out'"
fi

# --- 3. deleting a dylib that symbols still bind to must be refused ----------
build_main "$T/main_bad"
if "$T/change_dylib" "$T/main_bad" -delete "@loader_path/liba.dylib" >/dev/null 2>&1; then
    bad "-delete in-use" "tool accepted deleting a dylib that still has bound symbols"
else
    ok "-delete: refuses to orphan symbols bound to the deleted dylib"
fi

# --- 4. -insert composes with -change ----------------------------------------
build_main "$T/main_both"
"$T/change_dylib" "$T/main_both" -grow -insert "@loader_path/libspare.dylib" \
    -change "@loader_path/libb.dylib" "@loader_path/libb2.dylib" >/dev/null || bad "tool run" "change_dylib failed"
cp "$T/libb.dylib" "$T/libb2.dylib"
otool -L "$T/main_both" | grep -q libb2 \
    && ok "-insert + -change compose" || bad "compose" "libb2 not present"
if out=$(cd "$T" && ./main_both 2>&1) && [ "$out" = "33" ]; then
    ok "-insert + -change: still resolves (33)"
else
    bad "compose run" "got '$out'"
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
"$T/change_dylib" "$T/main_dup" -delete "@loader_path/libspare.dylib" >/dev/null \
    || bad "tool run" "change_dylib failed"
out=$(cd "$T" && ./main_dup 2>&1) || true
if [ "$out" = "1" ]; then
    ok "-delete: still calls libdup1 (no silent rebind to libdup2)"
else
    bad "-delete silent rebind" "got '$out' — bound to the wrong dylib"
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
"$T/change_dylib" "$T/main_conflict" \
    -change "@loader_path/libspare.dylib" "@loader_path/libspare_renamed.dylib" \
    -delete "@loader_path/libspare.dylib" >/dev/null 2>&1
rc=$?
deps=$(otool -L "$T/main_conflict")
if [ $rc -eq 0 ] && ! echo "$deps" | grep -q libspare; then
    if out=$(cd "$T" && ./main_conflict 2>&1) && [ "$out" = "33" ]; then
        ok "-change and -delete of the same path: delete wins, still runs (33)"
    else
        bad "-change+-delete conflict" "tool accepted it but the binary is broken: '$out'"
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
    before=$("$T/ordinal_of" "$T/libupd_a.dylib" _getpid 2>&1)
    [ "$before" = "3" ] || bad "upward fixture" "fixture itself not as expected before any rewrite: _getpid ordinal is '$before', wanted 3"
    # ordinals as linked: 1=libspare, 2=libupd_b (upward), 3=libSystem, so
    # _getpid (a real libSystem call, not foldable by the optimizer) starts
    # at ordinal 3. Deleting 1 and 2 must leave only libSystem, now ordinal 1,
    # with _getpid's nlist entry renumbered to match -- not left stale at 3
    # (now out of range) and not left pointing at whatever load command
    # happens to occupy slot 1 in a table that disagreed with the map.
    #
    # This fixture's plain __TEXT layout doesn't satisfy mg_plausible's
    # LC_FUNCTION_STARTS heuristic (macho_grow.h) on this host regardless of
    # any rewrite -- confirmed by running mg_plausible on a copy of this file
    # untouched by change_dylib, so it's not something the ordinal fix
    # introduces. MACHO_NO_VERIFY=1 opts out of that unrelated gate so this
    # case tests ordinal renumbering, not mg_plausible.
    MACHO_NO_VERIFY=1 "$T/change_dylib" "$T/libupd_a.dylib" \
        -delete "@loader_path/libspare.dylib" \
        -delete "@loader_path/libupd_b.dylib" >/dev/null \
        || bad "tool run" "change_dylib failed on the upward-dylib fixture"
    deps=$(otool -L "$T/libupd_a.dylib")
    if echo "$deps" | grep -Eq 'libspare|libupd_b'; then
        bad "-delete upward" "libspare or libupd_b still present: $deps"
    else
        ok "-delete: an LC_LOAD_UPWARD_DYLIB is matched/deleted like any other dylib LC"
    fi
    after=$("$T/ordinal_of" "$T/libupd_a.dylib" _getpid 2>&1)
    case "$after" in
        [0-9]*)
            if [ "$after" = "1" ]; then
                ok "-delete: _getpid's ordinal renumbered to the surviving libSystem (1)"
            else
                bad "-delete upward renumber" "_getpid's ordinal is $after, expected 1"
            fi
            ;;
        *)
            bad "-delete upward renumber" "ordinal_of returned no number, not a wrong number: $after"
            ;;
    esac
fi

# --- 9. more operations than the option arrays hold must be refused ----------
# Each option accumulates into a fixed-size array. Without a bounds check the
# writes run off the end into whatever follows -- silently, because nothing
# reads back a length. Only -strip-lc checked, so the rest could overflow.
# One case per array, each one past its capacity.
cap_case() {
    desc=$1; shift
    build_main "$T/main_cap"
    if "$T/change_dylib" "$T/main_cap" "$@" >/dev/null 2>"$T/cap.err"; then
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
if "$T/change_dylib" "$T/main_atcap" -grow "$@" >/dev/null 2>"$T/atcap.err"; then
    ok "-add exactly at capacity is accepted"
else
    bad "-add at capacity" "refused at the cap: $(head -1 "$T/atcap.err")"
fi

echo
[ "$fails" -eq 0 ] && { echo "change_dylib_test: all cases pass"; exit 0; }
echo "change_dylib_test: $fails FAILED"; exit 1
