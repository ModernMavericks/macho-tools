/*
 * tests/trie_test.c — hermetic tests for src/trie.c's mt_trie_rebuild.
 *
 * Ground truth is hand-built trie byte buffers and hand-computed ULEB
 * encodings (small values, worked by hand in the comments), so this is
 * host-agnostic: no fixture file, no compiler-dependent byte sizes.
 *
 * Build: clang -O2 -Wall -Isrc -o /tmp/trietest tests/trie_test.c src/trie.c
 *   src/uleb.c && /tmp/trietest
 */
#include "trie.h"
#include "../src/uleb.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, msg, ...) do { if (!(cond)) { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); fails++; } } while (0)

/* ---- a trivial one-node trie: root is itself a terminal, no children ----
 * term=2 (flags 1 byte + addr 1 byte), flags=0, addr=0x10, nch=0 */
static void test_rebuild_root_terminal_shifts_address(void) {
    uint8_t in[] = { 0x02, 0x00, 0x10, 0x00 };
    uint8_t *out = NULL; uint32_t osz = 0;
    int r = mt_trie_rebuild(in, sizeof in, 0x1000, &out, &osz);
    CHECK(r == 0, "rebuild of a root-terminal trie succeeds (got %d)", r);
    if (r != 0) return;
    /* addr 0x10 + 0x1000 = 0x1010, still 2 bytes (< 16384) -> plen=1+2=3,
     * termsz=3, total = 1(termsz)+3(payload)+1(nch) = 5 */
    CHECK(osz == 5, "output is 5 bytes (got %u)", osz);
    uint64_t v; int n = mu_decode(out + 2, out + osz, &v);
    CHECK(n == 2 && v == 0x1010, "address shifted to 0x1010 (got n=%d v=%#llx)",
          n, (unsigned long long)v);
    CHECK(out[0] == 3 && out[1] == 0, "termsz=3, flags=0 (got %u %u)", out[0], out[1]);
    CHECK(out[4] == 0, "nch=0");
    free(out);
}

/* ---- the widening case: address crosses a ULEB byte boundary ----
 * root: term=0, two children "A"->8, "B"->13 (an in-place patch cannot
 * absorb this: node A's address needs 3 bytes where it had 2, which would
 * cascade into every offset after it -- exactly why this module exists).
 * Bytes and the expected result are worked out by hand in the comments
 * below; docs/... nothing else needs to agree with this, it's the ground
 * truth. */
static void test_rebuild_widens_when_needed(void) {
    /* Same 17-byte shape as macho_grow_test.c's MG_T_TRIE fixture, except
     * node A's address is 16000 (0x3E80) instead of 0x1000 -- still a
     * 2-byte ULEB (16000 < 16384), but 16000 + 0x1000 = 20096 >= 16384,
     * which needs 3 bytes. Node B's address is 0 (the __mh_execute_header
     * case) and must stay 0. */
    static const uint8_t in[17] = {
        0x00, 0x02,                       /* root: no terminal, 2 children */
        'A', 0x00, 8,                     /* "A" -> offset 8 */
        'B', 0x00, 13,                    /* "B" -> offset 13 */
        0x03, 0x00, 0x80, 0x7D, 0x00,     /* node A: termsz3 flags0 addr16000(2B) nch0 */
        0x02, 0x00, 0x00, 0x00            /* node B: termsz2 flags0 addr0       nch0 */
    };
    uint8_t *out = NULL; uint32_t osz = 0;
    int r = mt_trie_rebuild(in, sizeof in, 0x1000, &out, &osz);
    CHECK(r == 0, "rebuild succeeds on a widening trie (got %d)", r);
    if (r != 0) return;

    /* Hand-computed expected output (see the task report / commit message
     * for the by-hand derivation):
     *   root (8B):  00 02 'A' 00 08 'B' 00 0E
     *   node A (6B): 04 00 80 9D 01 00        (addr 20096 = 80 9D 01)
     *   node B (4B): 02 00 00 00
     * total 18 bytes -- ONE MORE than the original 17, which is exactly the
     * "does not fit in place" case macho_grow.h must now handle by growing
     * __LINKEDIT instead of refusing. */
    static const uint8_t expect[18] = {
        0x00, 0x02, 'A', 0x00, 0x08, 'B', 0x00, 0x0E,
        0x04, 0x00, 0x80, 0x9D, 0x01, 0x00,
        0x02, 0x00, 0x00, 0x00,
    };
    CHECK(osz == sizeof expect, "output grew by exactly 1 byte: 17 -> %u", osz);
    CHECK(osz == sizeof expect && memcmp(out, expect, sizeof expect) == 0,
          "rebuilt trie matches the hand-computed bytes exactly");

    /* Independently decode node A's address out of the result, rather than
     * only trusting the byte-for-byte compare above. */
    if (osz >= 13) {
        uint64_t v; int n = mu_decode(out + 10, out + osz, &v);
        CHECK(n == 3 && v == 20096, "node A address is 20096 in 3 bytes (got n=%d v=%llu)",
              n, (unsigned long long)v);
    }
    free(out);
}

/* ---- address 0 is never shifted, even under a large shift ---- */
static void test_rebuild_zero_address_stays_zero(void) {
    uint8_t in[] = { 0x01, 0x00, 0x00 };   /* term=1 (flags only!) -- malformed on
                                             * purpose? No: flags=0 non-reexport
                                             * non-stub needs an address field too.
                                             * Use a correct 1-terminal below instead. */
    (void)in;
    uint8_t in2[] = { 0x02, 0x00, 0x00, 0x00 };  /* termsz2: flags0 addr0; nch0 */
    uint8_t *out = NULL; uint32_t osz = 0;
    int r = mt_trie_rebuild(in2, sizeof in2, 0xFFFFFF, &out, &osz);
    CHECK(r == 0, "rebuild succeeds (got %d)", r);
    if (r != 0) return;
    uint64_t v; int n = mu_decode(out + 2, out + osz, &v);
    CHECK(n == 1 && v == 0, "address 0 stays 0 even with a huge shift (got n=%d v=%llu)",
          n, (unsigned long long)v);
    free(out);
}

/* ---- re-export: ordinal + import name, no address to shift ---- */
static void test_rebuild_reexport_untouched(void) {
    /* flags=0x08 (REEXPORT), ordinal=3, name="orig\0". term covers flags(1)
     * + ordinal(1) + name(5) = 7. */
    uint8_t in[] = {
        0x07,                    /* termsz */
        0x08,                    /* flags: REEXPORT */
        0x03,                    /* ordinal */
        'o','r','i','g',0x00,    /* import name */
        0x00,                    /* nch */
    };
    uint8_t *out = NULL; uint32_t osz = 0;
    int r = mt_trie_rebuild(in, sizeof in, 0x2000, &out, &osz);
    CHECK(r == 0, "rebuild of a re-export node succeeds (got %d)", r);
    if (r != 0) return;
    CHECK(osz == sizeof in, "re-export node is byte-identical in size (got %u want %zu)",
          osz, sizeof in);
    CHECK(memcmp(in, out, sizeof in) == 0, "re-export bytes are UNCHANGED (no address to shift)");
    free(out);
}

/* ---- stub-and-resolver: both addresses shift, independently of 0-ness ---- */
static void test_rebuild_stub_and_resolver_both_shift(void) {
    /* flags=0x10 (STUB_AND_RESOLVER), stub=0x50, resolver=0 (stays 0).
     * term = flags(1) + stub(1) + resolver(1) = 3. */
    uint8_t in[] = { 0x03, 0x10, 0x50, 0x00, 0x00 };
    uint8_t *out = NULL; uint32_t osz = 0;
    int r = mt_trie_rebuild(in, sizeof in, 0x30, &out, &osz);
    CHECK(r == 0, "rebuild of a stub-and-resolver node succeeds (got %d)", r);
    if (r != 0) return;
    const uint8_t *p = out + 2, *end = out + osz;
    uint64_t stub, resolver;
    int n1 = mu_decode(p, end, &stub); p += n1;
    int n2 = mu_decode(p, end, &resolver); (void)n2;
    CHECK(stub == 0x50 + 0x30, "stub shifted (got %#llx)", (unsigned long long)stub);
    CHECK(resolver == 0, "resolver (was 0) stays 0 (got %#llx)", (unsigned long long)resolver);
    free(out);
}

/* ---- malformed input: bounds-checked, must refuse, never read OOB ----
 * (run under libgmalloc during verification -- these are exactly the shapes
 * a fuzzer or a hostile binary would hand this code.) */
static void test_rebuild_malformed_refuses(void) {
    uint8_t *out; uint32_t osz;

    uint8_t empty[1]; out = (uint8_t*)1; osz = 1;
    CHECK(mt_trie_rebuild(empty, 0, 0, &out, &osz) == -1, "size 0 refuses");
    CHECK(out == NULL && osz == 0, "size 0: out/out_size cleared");

    uint8_t trunc_term[] = { 0x80 };   /* continuation with nothing after */
    out = (uint8_t*)1;
    CHECK(mt_trie_rebuild(trunc_term, sizeof trunc_term, 0, &out, &osz) == -1,
          "truncated terminal-size ULEB refuses");
    CHECK(out == NULL, "out cleared on refusal");

    uint8_t term_past_end[] = { 0x7F, 0x00 };   /* claims 127 bytes of terminal, has 1 */
    CHECK(mt_trie_rebuild(term_past_end, sizeof term_past_end, 0, &out, &osz) == -1,
          "terminal size running past the buffer refuses");

    uint8_t no_child_count[] = { 0x00 };   /* term=0, then nothing -- no nch byte */
    CHECK(mt_trie_rebuild(no_child_count, sizeof no_child_count, 0, &out, &osz) == -1,
          "missing child-count byte refuses");

    uint8_t label_no_nul[] = { 0x00, 0x01, 'x' };   /* nch=1, label never terminates */
    CHECK(mt_trie_rebuild(label_no_nul, sizeof label_no_nul, 0, &out, &osz) == -1,
          "unterminated edge label refuses");

    /* child offset points outside the trie */
    uint8_t bad_child[] = { 0x00, 0x01, 'x', 0x00, 99 };
    CHECK(mt_trie_rebuild(bad_child, sizeof bad_child, 0, &out, &osz) == -1,
          "out-of-range child offset refuses");

    /* child offset decodes to a huge 64-bit value -- must not truncate to a
     * small in-range uint32_t and wander off somewhere plausible-looking. */
    uint8_t huge_child[] = { 0x00, 0x01, 'x', 0x00,
                              0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x01 };
    CHECK(mt_trie_rebuild(huge_child, sizeof huge_child, 0, &out, &osz) == -1,
          "huge child offset refuses rather than truncating");
}

/* ---- cycle: a child offset points back at an ancestor ----
 * A well-formed export trie is a tree; this rebuild does not attempt to
 * support (or silently mishandle) a cyclic one -- it must refuse rather than
 * recurse forever. (In THIS particular shape -- a direct self-loop -- the
 * depth cap below would also eventually catch it on its own; the dedicated
 * discriminator for the seen[] guard specifically is the diamond test right
 * after this one, which terminates in O(1) depth and so cannot be caught by
 * the depth cap -- only by seen[].) */
static void test_rebuild_cycle_refuses(void) {
    /* node@0: term0, nch1, child "x" -> 0 (itself) */
    uint8_t in[] = { 0x00, 0x01, 'x', 0x00, 0x00 };
    uint8_t *out = (uint8_t*)1; uint32_t osz = 1;
    int r = mt_trie_rebuild(in, sizeof in, 0, &out, &osz);
    CHECK(r == -1, "a self-referential child offset refuses (got %d)", r);
    CHECK(out == NULL, "out cleared on refusal");
}

/* ---- diamond: two DIFFERENT edges reach the SAME offset, no cycle at all --
 * This terminates immediately (shallow, finite recursion either way), so
 * unlike the self-loop above, only the seen[] guard -- not the depth cap --
 * can catch it. This rebuild does not support shared subtrees (no
 * suffix-compressed export trie has ever been observed from ld64; every
 * offset in a real one is reached exactly once), so it must refuse rather
 * than silently emit two copies or, worse, one node with two owners. */
static void test_rebuild_shared_offset_refuses(void) {
    /* root@0 (8 bytes: term0,nch2,"A\0"+off,"B\0"+off): "A"->8, "B"->8, both
     * pointing at the SAME leaf starting right after the root. */
    uint8_t in[] = {
        0x00, 0x02,
        'A', 0x00, 8,
        'B', 0x00, 8,
        0x02, 0x00, 0x00, 0x00,   /* leaf@8: termsz2 flags0 addr0 nch0 */
    };
    uint8_t *out = (uint8_t*)1; uint32_t osz = 1;
    int r = mt_trie_rebuild(in, sizeof in, 0, &out, &osz);
    CHECK(r == -1, "two edges sharing one child offset refuses (got %d)", r);
    CHECK(out == NULL, "out cleared on refusal");
}

/* ---- depth cap: refuses explicitly rather than blowing the C stack ----
 * A chain of 200 single-child nodes, each one byte long (term=0, nch=1,
 * label="" i.e. immediate NUL, child offset = next byte), terminated by a
 * leaf. 128 is macho_grow.h's own existing depth guard (mg_trie_node); this
 * module matches it on purpose. */
static void test_rebuild_depth_cap_refuses(void) {
    const int CHAIN = 200;
    /* each link: term(1B)=0x00, nch(1B)=0x01, label NUL(1B)=0x00, child
     * offset -- forced to a FIXED 2-byte ULEB (mu_encode_fixed), since the
     * chain runs well past 127 bytes and a real single-byte ULEB can't
     * address that; a first version of this test hand-cast the offset into
     * one raw byte, which silently corrupted every link past #31 and made
     * the test pass for the wrong reason (a parse error, not the depth
     * guard) -- caught by mutation-testing this same test against a
     * disabled depth check, which it did NOT catch until this fix. */
    int cap = CHAIN * 5 + 8;
    uint8_t *buf = (uint8_t *)malloc((size_t)cap);
    int len = 0;
    for (int i = 0; i < CHAIN; i++) {
        int next = len + 5;
        buf[len++] = 0x00;   /* term = 0 */
        buf[len++] = 0x01;   /* nch = 1 */
        buf[len++] = 0x00;   /* empty label + NUL */
        mu_encode_fixed(buf + len, (uint64_t)next, 2);   /* child offset, 2B ULEB */
        len += 2;
    }
    /* final leaf: term=2 (flags0 addr0) */
    buf[len++] = 0x02; buf[len++] = 0x00; buf[len++] = 0x00; buf[len++] = 0x00;

    uint8_t *out = (uint8_t*)1; uint32_t osz = 1;
    int r = mt_trie_rebuild(buf, (uint32_t)len, 0x100, &out, &osz);
    CHECK(r == -1, "a 200-deep chain refuses (depth cap is 128) (got %d)", r);
    CHECK(out == NULL, "out cleared on refusal");
    free(buf);
}

/* ---- no silent truncation: 200 edges on one node, ALL of them preserved ----
 * The reference implementation this is adapted from caps at TRIE_MAX_EDGES
 * (128) and silently drops anything past it. This module has no such cap --
 * edges are bounded only by the input's own nch byte (max 255) -- so this
 * proves 200 distinct edges all survive a rebuild, none merged or dropped. */
static void test_rebuild_many_edges_none_dropped(void) {
    const int N = 200;
    /* Layout: root at offset 0 (term=0, nch=N, then N edges each
     * "<2-digit-index>\0<uleb child offset>"), followed by N leaf nodes
     * (term=2: flags0 addr(index) -- each leaf's address is its own index,
     * so after rebuild we can confirm both the LABEL and the SHIFTED
     * address survived for every single one of the 200). */
    int root_len = 1 + 1;   /* term byte + nch byte */
    for (int i = 0; i < N; i++) root_len += 3 + 2;  /* "NN\0" (3) + uleb child offset (<=2B here, but keep it simple: compute exactly below) */
    /* Above overestimates were fiddly to keep exact by hand; build it
     * programmatically with a growable buffer instead, computing each
     * child's offset as we place it, since child offsets are ALWAYS after
     * the root here (root is written first, offset 0, and leaves start
     * right after it) so no forward-reference math is needed beyond simple
     * arithmetic on already-known sizes. */
    uint8_t label[3]; int leaf_sz = 4;   /* term2 flags0 addr(1B for <128... but N=200 needs 2B for values>=128) */
    /* addresses 0..199: minlen is 1 byte for <128, 2 bytes for 128..199.
     * Use leaf size 5 uniformly (term=3: flags0 + addr padded... simplest:
     * just always encode the terminal with a 2-byte ULEB address via
     * mu_encode_fixed, so every leaf is the same size and offsets are easy
     * to compute by hand -- termsz=3 (flags1+addr2), total=1+3+1=5.) */
    leaf_sz = 5;

    /* First pass: compute root_len exactly, using the KNOWN leaf offsets
     * (leaves start right after the root, each leaf_sz bytes, in index
     * order) -- so child offsets, and their ULEB width, are known up front. */
    int first_leaf_off_guess = 2 + N * 8;  /* upper bound: "NN\0"+2B uleb = 6B max per edge; refine below */
    (void)first_leaf_off_guess;

    /* Simpler and exact: two passes over a dynamic buffer using a fixed,
     * generous per-edge encoding so the math is trivial --
     * label "NN" (2 bytes, zero-padded index) + NUL + child offset ALWAYS
     * encoded in exactly 2 ULEB bytes (mu_encode_fixed forces the width),
     * so each edge is exactly 2+1+2 = 5 bytes, and root_len = 2 + N*5. */
    root_len = 2 + N * 6;   /* label "NNN"(3) + NUL(1) + child-offset forced to 2 bytes */
    int total = root_len + N * leaf_sz;
    uint8_t *buf = (uint8_t *)malloc((size_t)total);
    buf[0] = 0x00;              /* root term = 0 */
    buf[1] = (uint8_t)N;        /* nch = 200 (needs a full byte: fits, 200<256) */
    int p = 2;
    for (int i = 0; i < N; i++) {
        int child_off = root_len + i * leaf_sz;
        label[0] = (uint8_t)('0' + (i / 100));
        label[1] = (uint8_t)('0' + ((i / 10) % 10));
        label[2] = (uint8_t)('0' + (i % 10));
        buf[p++] = label[0]; buf[p++] = label[1]; buf[p++] = label[2];
        buf[p++] = 0x00;   /* NUL */
        /* child offset, forced to exactly 2 ULEB bytes */
        mu_encode_fixed(buf + p, (uint64_t)child_off, 2);
        p += 2;
    }
    CHECK(p == root_len, "root pre-computed length matches actual (got %d want %d)", p, root_len);
    for (int i = 0; i < N; i++) {
        int off = root_len + i * leaf_sz;
        buf[off + 0] = 0x03;    /* termsz = 3 (flags1 + addr2) */
        buf[off + 1] = 0x00;    /* flags = 0 */
        mu_encode_fixed(buf + off + 2, (uint64_t)i, 2);   /* address = i, forced 2 bytes */
        buf[off + 4] = 0x00;    /* nch = 0 */
    }

    uint8_t *out = NULL; uint32_t osz = 0;
    int r = mt_trie_rebuild(buf, (uint32_t)total, 0x10000, &out, &osz);
    CHECK(r == 0, "rebuild of a 200-edge root succeeds (got %d)", r);
    free(buf);
    if (r != 0) return;

    CHECK(out[0] == 0 && out[1] == (uint8_t)N, "rebuilt root still has nch=200 (got %u)", out[1]);

    /* Walk the rebuilt root's 200 edges (labels are minimally re-encoded
     * offsets now, but still start with the same 3-char label bytes) and
     * confirm all 200 labels are present, in order, none merged/dropped,
     * and that following each one lands on a leaf whose address is
     * i + 0x10000. */
    const uint8_t *q = out + 2, *oend = out + osz;
    int seen_count = 0;
    for (int i = 0; i < N && q < oend; i++) {
        const uint8_t *lbl = q;
        while (q < oend && *q) q++;
        if (q >= oend) break;
        int lbl_len = (int)(q - lbl);
        q++;
        uint64_t coff; int n = mu_decode(q, oend, &coff);
        if (n == 0) break;
        q += n;
        char want[4]; snprintf(want, sizeof want, "%03d", i);
        if (lbl_len == 3 && memcmp(lbl, want, 3) == 0) seen_count++;
        else { printf("FAIL: edge %d label mismatch\n", i); fails++; continue; }
        if (coff < osz) {
            const uint8_t *leaf = out + coff;
            uint64_t addr; mu_decode(leaf + 2, oend, &addr);
            /* leaf 0's address is 0, which the trie format reserves to mean
             * "no address" (the __mh_execute_header case) and this rebuild
             * therefore never shifts -- see mt_trie_rebuild's contract. */
            uint64_t want = (i == 0) ? 0 : (uint64_t)i + 0x10000;
            CHECK(addr == want, "edge %d's leaf address is i+shift, 0 excepted (got %llu want %llu)",
                  i, (unsigned long long)addr, (unsigned long long)want);
        }
    }
    CHECK(seen_count == N, "all %d edges present in order, none dropped (got %d)", N, seen_count);
    free(out);
}

int main(void) {
    test_rebuild_root_terminal_shifts_address();
    test_rebuild_widens_when_needed();
    test_rebuild_zero_address_stays_zero();
    test_rebuild_reexport_untouched();
    test_rebuild_stub_and_resolver_both_shift();
    test_rebuild_malformed_refuses();
    test_rebuild_cycle_refuses();
    test_rebuild_shared_offset_refuses();
    test_rebuild_depth_cap_refuses();
    test_rebuild_many_edges_none_dropped();

    if (fails) { printf("%d FAIL(S)\n", fails); return 1; }
    printf("ALL PASS\n");
    return 0;
}
