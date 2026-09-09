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
# src/uleb.h, change_dylib.c itself now includes src/ordinals.h and src/fat.h
# (the shared fat_header/fat_arch validator both it and fix_macho use), so the
# toolkit sources it needs are listed here too.
"$CC" -O2 -I src -o "$T/change_dylib" change_dylib.c src/uleb.c src/image.c src/ordinals.c src/fat.c
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

# makefat/fatcheck: build and inspect a fat (universal) Mach-O without
# depending on system lipo, whose accepted architecture list is not this
# suite's to pin -- a hand-crafted fat container is something we control
# completely, on either host. Reads/writes the on-disk convention every real
# fat file uses (big-endian fat_header/fat_arch, i.e. FAT_CIGAM as observed
# from a little-endian x86_64/arm64 host), by construction, not detection.
cat > "$T/makefat.c" <<'EOF'
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
    if (argc != 10) { fprintf(stderr, "usage: %s out s0 ct0 cs0 al0 s1 ct1 cs1 al1\n", argv[0]); return 2; }
    size_t sz0, sz1;
    uint8_t *b0 = readfile(argv[2], &sz0);
    uint32_t ct0 = (uint32_t)strtoul(argv[3], NULL, 0);
    uint32_t cs0 = (uint32_t)strtoul(argv[4], NULL, 0);
    uint32_t al0 = (uint32_t)strtoul(argv[5], NULL, 0);
    uint8_t *b1 = readfile(argv[6], &sz1);
    uint32_t ct1 = (uint32_t)strtoul(argv[7], NULL, 0);
    uint32_t cs1 = (uint32_t)strtoul(argv[8], NULL, 0);
    uint32_t al1 = (uint32_t)strtoul(argv[9], NULL, 0);
    uint32_t hdrlen = (uint32_t)(sizeof(struct fat_header) + 2 * sizeof(struct fat_arch));
    uint32_t a0mask = (1u << al0) - 1;
    uint32_t off0 = (hdrlen + a0mask) & ~a0mask;
    uint32_t a1mask = (1u << al1) - 1;
    uint32_t off1 = (uint32_t)((off0 + sz0 + a1mask) & ~(uint64_t)a1mask);
    uint32_t total = (uint32_t)(off1 + sz1);
    uint8_t *out = calloc(1, total);
    struct fat_header *fh = (struct fat_header *)out;
    fh->magic = sw32(FAT_MAGIC);
    fh->nfat_arch = sw32(2);
    struct fat_arch *ar = (struct fat_arch *)(out + sizeof(struct fat_header));
    ar[0].cputype = (cpu_type_t)sw32(ct0);
    ar[0].cpusubtype = (cpu_subtype_t)sw32(cs0);
    ar[0].offset = sw32(off0);
    ar[0].size = sw32((uint32_t)sz0);
    ar[0].align = sw32(al0);
    ar[1].cputype = (cpu_type_t)sw32(ct1);
    ar[1].cpusubtype = (cpu_subtype_t)sw32(cs1);
    ar[1].offset = sw32(off1);
    ar[1].size = sw32((uint32_t)sz1);
    ar[1].align = sw32(al1);
    memcpy(out + off0, b0, sz0);
    memcpy(out + off1, b1, sz1);
    int ofd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) { perror("open out"); return 2; }
    if (write(ofd, out, total) != (ssize_t)total) { perror("write"); return 2; }
    close(ofd);
    return 0;
}
EOF
"$CC" -O2 -o "$T/makefat" "$T/makefat.c"

cat > "$T/fatcheck.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
static uint8_t *readfile(const char *path, size_t *outsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); exit(2); }
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
static void locate_arch(const uint8_t *buf, size_t sz, int idx, uint32_t *off, uint32_t *size,
                         uint32_t *align) {
    uint32_t magic = *(const uint32_t *)buf;
    if (magic != FAT_MAGIC && magic != FAT_CIGAM) { fprintf(stderr, "not a fat file\n"); exit(2); }
    int swap = (magic == FAT_CIGAM);
    const struct fat_header *fh = (const struct fat_header *)buf;
    uint32_t narch = swap ? sw32(fh->nfat_arch) : fh->nfat_arch;
    if ((uint32_t)idx >= narch) { fprintf(stderr, "arch %d out of range (narch=%u)\n", idx, narch); exit(2); }
    const struct fat_arch *ar = (const struct fat_arch *)(buf + sizeof(struct fat_header));
    uint32_t o = swap ? sw32((uint32_t)ar[idx].offset) : (uint32_t)ar[idx].offset;
    uint32_t s = swap ? sw32((uint32_t)ar[idx].size)   : (uint32_t)ar[idx].size;
    uint32_t a = swap ? sw32(ar[idx].align) : ar[idx].align;
    if ((size_t)o + s > sz) { fprintf(stderr, "arch %d out of bounds\n", idx); exit(2); }
    *off = o; *size = s; *align = a;
}
static void dump_dylibs(const uint8_t *p, size_t sz) {
    if (sz < sizeof(struct mach_header_64)) { fprintf(stderr, "slice too small\n"); exit(2); }
    const struct mach_header_64 *hdr = (const struct mach_header_64 *)p;
    if (hdr->magic != MH_MAGIC_64) { fprintf(stderr, "slice not 64-bit Mach-O (magic=0x%x)\n", hdr->magic); exit(2); }
    const uint8_t *lcp = p + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)lcp;
        if (lc->cmd == LC_LOAD_DYLIB || lc->cmd == LC_ID_DYLIB ||
            lc->cmd == LC_LOAD_WEAK_DYLIB || lc->cmd == LC_REEXPORT_DYLIB) {
            const struct dylib_command *dc = (const struct dylib_command *)lcp;
            printf("%s\n", (const char *)lcp + dc->dylib.name.offset);
        }
        lcp += lc->cmdsize;
    }
}
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: fatcheck <mode> <file> [args...]\n"); return 2; }
    const char *mode = argv[1];
    size_t sz; uint8_t *buf = readfile(argv[2], &sz);
    if (strcmp(mode, "archinfo") == 0) {
        uint32_t magic = *(uint32_t *)buf;
        if (magic != FAT_MAGIC && magic != FAT_CIGAM) { fprintf(stderr, "not a fat file\n"); return 2; }
        int swap = (magic == FAT_CIGAM);
        const struct fat_header *fh = (const struct fat_header *)buf;
        uint32_t narch = swap ? sw32(fh->nfat_arch) : fh->nfat_arch;
        printf("narch=%u\n", narch);
        for (uint32_t i = 0; i < narch; i++) {
            uint32_t o, s, a; locate_arch(buf, sz, (int)i, &o, &s, &a);
            printf("%u %u %u %u\n", i, o, s, a);
        }
        return 0;
    } else if (strcmp(mode, "dump") == 0) {
        if (argc != 5) { fprintf(stderr, "usage: fatcheck dump <file> <idx> <outfile>\n"); return 2; }
        int idx = atoi(argv[3]);
        uint32_t o, s, a; locate_arch(buf, sz, idx, &o, &s, &a);
        int ofd = open(argv[4], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ofd < 0) { perror("open out"); return 2; }
        if (write(ofd, buf + o, s) != (ssize_t)s) { perror("write"); return 2; }
        close(ofd);
        return 0;
    } else if (strcmp(mode, "dylibs") == 0) {
        if (argc != 4) { fprintf(stderr, "usage: fatcheck dylibs <file> <idx>\n"); return 2; }
        int idx = atoi(argv[3]);
        uint32_t o, s, a; locate_arch(buf, sz, idx, &o, &s, &a);
        dump_dylibs(buf + o, s);
        return 0;
    }
    fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
}
EOF
"$CC" -O2 -o "$T/fatcheck" "$T/fatcheck.c"

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

# --- 10/11. fat binaries in the rewrite path ---------------------------------
# fix_macho already walks fat/thin; until now change_dylib only understood
# thin. Both cases build a genuine 2-slice fat binary: a real, linked x86_64
# executable (the same $T/main built above) plus a slice this tool cannot
# and must not try to rewrite -- a syntactically valid but deliberately
# minimal 32-bit (MH_MAGIC, CPU_TYPE_I386) Mach-O, built by hand rather than
# `clang -arch i386`, which a modern toolchain may no longer support at all.
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
"$T/makefat" "$T/main_fat" "$T/main" 0x1000007 3 12 "$T/slice32.bin" 7 3 12
arch1_before=$("$T/fatcheck" archinfo "$T/main_fat" | sed -n '3p')
"$T/change_dylib" "$T/main_fat" -change "@loader_path/liba.dylib" "@loader_path/liba_fat.dylib" >/dev/null \
    || bad "fat tool run" "change_dylib failed on a fat input"

narch=$("$T/fatcheck" archinfo "$T/main_fat" | head -1)
[ "$narch" = "narch=2" ] && ok "fat: narch unchanged (2)" || bad "fat narch" "got '$narch'"

dylibs0=$("$T/fatcheck" dylibs "$T/main_fat" 0)
if echo "$dylibs0" | grep -q '^@loader_path/liba_fat\.dylib$'; then
    ok "fat: the x86_64 slice's dylib path was actually changed"
else
    bad "fat dylib change" "x86_64 slice does not name the new path: $dylibs0"
fi

"$T/fatcheck" dump "$T/main_fat" 1 "$T/fat_slice1_after.bin"
if cmp -s "$T/slice32.bin" "$T/fat_slice1_after.bin"; then
    ok "fat: the slice this tool cannot understand is preserved byte-for-byte"
else
    bad "fat slice preserved" "the 32-bit slice's bytes changed"
fi

arch1_after=$("$T/fatcheck" archinfo "$T/main_fat" | sed -n '3p')
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
"$T/makefat" "$T/main_fat_grow" "$T/main" 0x1000007 3 12 "$T/slice32.bin" 7 3 12
before_arch0=$("$T/fatcheck" archinfo "$T/main_fat_grow" | sed -n '2p')
before_arch0_size=$(echo "$before_arch0" | awk '{print $3}')

set -- ; i=0
while [ $i -lt 32 ]; do
    set -- "$@" -add "@loader_path/libpad_a_pretty_long_synthetic_name_used_only_to_force_real_header_growth_$i.dylib"
    i=$((i+1))
done
"$T/change_dylib" "$T/main_fat_grow" -grow "$@" >/dev/null 2>"$T/fatgrow.err" \
    || bad "fat grow tool run" "change_dylib failed: $(head -1 "$T/fatgrow.err")"

after_arch0=$("$T/fatcheck" archinfo "$T/main_fat_grow" | sed -n '2p')
after_arch1=$("$T/fatcheck" archinfo "$T/main_fat_grow" | sed -n '3p')
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

"$T/fatcheck" dump "$T/main_fat_grow" 1 "$T/fat_slice1_after_grow.bin"
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

if "$T/change_dylib" "$T/main_descfat" \
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

dylibs_hi=$("$T/fatcheck" dylibs "$T/main_descfat" 0)
if echo "$dylibs_hi" | grep -q '^@loader_path/liba_desc\.dylib$'; then
    ok "fat descending-offset: the high-offset slice's dylib path was actually changed"
else
    bad "fat descending-offset dylib" "high-offset slice does not name the new path: $dylibs_hi"
fi

"$T/fatcheck" dump "$T/main_descfat" 1 "$T/descfat_lo_after.bin"
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
"$T/change_dylib" "$T/main_fat3" -grow "$@" >/dev/null 2>"$T/fat3.err" || rc=$?

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

echo
[ "$fails" -eq 0 ] && { echo "change_dylib_test: all cases pass"; exit 0; }
echo "change_dylib_test: $fails FAILED"; exit 1
