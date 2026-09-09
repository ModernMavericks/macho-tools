/* trie.c — see trie.h for the contract and the licensing/adaptation note. */

#include "trie.h"
#include "uleb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Matches mg_trie_node's own recursion-depth guard in macho_grow.h — kept in
 * sync deliberately, not shared, because the two walks are structurally
 * different (this one allocates; that one doesn't). */
#define MT_TRIE_MAX_DEPTH 128

#define MT_EXPORT_REEXPORT          0x08
#define MT_EXPORT_STUB_AND_RESOLVER 0x10

/* One child edge: the label and the parsed child node it leads to. `label`
 * points directly into the CALLER's input buffer (never copied) — safe
 * because the whole rebuild, decode through serialize, happens inside one
 * mt_trie_rebuild() call, before that buffer can move or be freed. */
struct mt_edge {
    const uint8_t *label;
    uint32_t label_len;
    int32_t child;   /* index into mt_builder.nodes; always valid once parsed */
};

struct mt_node {
    int has_term;
    uint64_t flags;

    int is_reexport;
    uint64_t reexport_ordinal;
    const uint8_t *reexport_name;
    uint32_t reexport_name_len;

    int is_stub_resolver;   /* a1/a2 = stub/resolver; else a1 = the address */
    uint64_t a1, a2;

    struct mt_edge *edges;
    uint32_t nedges, edges_cap;

    uint32_t out_off, out_sz;   /* filled in by mt_layout */
};

/* No global mutable state: everything lives in one of these, stack-allocated
 * in mt_trie_rebuild and passed down by pointer. Safe to call repeatedly (a
 * tool processing several fat slices in a loop, say) with no cross-call
 * state to reset. */
struct mt_builder {
    const uint8_t *trie;
    uint32_t size;
    uint64_t shift;

    struct mt_node *nodes;
    uint32_t nnodes, cap;

    uint8_t *seen;   /* size `size`; 1 once a node at that offset is parsed */
    int failed;      /* set once, so we print only the first reason */
};

static void mt_fail(struct mt_builder *b, const char *why) {
    if (!b->failed) fprintf(stderr, "trie: %s; refusing to rebuild\n", why);
    b->failed = 1;
}

static void mt_free_builder(struct mt_builder *b) {
    if (b->nodes) {
        for (uint32_t i = 0; i < b->nnodes; i++) free(b->nodes[i].edges);
        free(b->nodes);
    }
    free(b->seen);
    b->nodes = NULL; b->seen = NULL; b->nnodes = b->cap = 0;
}

static int32_t mt_alloc_node(struct mt_builder *b) {
    if (b->nnodes >= b->cap) {
        uint32_t ncap = b->cap ? b->cap * 2 : 64;
        struct mt_node *tmp = (struct mt_node *)realloc(b->nodes, (size_t)ncap * sizeof *tmp);
        if (!tmp) { mt_fail(b, "out of memory growing the node table"); return -1; }
        b->nodes = tmp; b->cap = ncap;
    }
    int32_t idx = (int32_t)b->nnodes++;
    memset(&b->nodes[idx], 0, sizeof b->nodes[idx]);
    return idx;
}

static int mt_add_edge(struct mt_builder *b, uint32_t node_idx, const uint8_t *label,
                        uint32_t label_len, int32_t child) {
    struct mt_node *n = &b->nodes[node_idx];
    if (n->nedges >= n->edges_cap) {
        uint32_t ncap = n->edges_cap ? n->edges_cap * 2 : 4;
        struct mt_edge *tmp = (struct mt_edge *)realloc(n->edges, (size_t)ncap * sizeof *tmp);
        if (!tmp) { mt_fail(b, "out of memory growing an edge list"); return -1; }
        n->edges = tmp; n->edges_cap = ncap;
    }
    n->edges[n->nedges].label = label;
    n->edges[n->nedges].label_len = label_len;
    n->edges[n->nedges].child = child;
    n->nedges++;
    return 0;
}

/* Parse the node at byte offset `off`, recursively parsing its children.
 * Returns the new node's index, or -1 on failure (b->failed already set with
 * a reason). `off` is bounds-checked against b->size before every use — a
 * malformed trie must refuse, never read out of bounds. */
static int32_t mt_parse(struct mt_builder *b, uint32_t off, int depth) {
    if (b->failed) return -1;
    if (depth > MT_TRIE_MAX_DEPTH) {
        mt_fail(b, "trie nesting exceeds 128 levels -- refusing rather than risk exhausting "
                   "the C stack on a pathological or adversarial trie");
        return -1;
    }
    if (off >= b->size) { mt_fail(b, "a node offset points outside the trie"); return -1; }
    if (b->seen[off]) {
        mt_fail(b, "a node offset is reachable more than one way (a cycle, or shared-subtree "
                   "compression this rebuild does not support -- no well-formed export trie "
                   "needs it)");
        return -1;
    }
    b->seen[off] = 1;

    int32_t idx = mt_alloc_node(b);
    if (idx < 0) return -1;
    /* NOTE: do not hold a `struct mt_node *` across the recursive mt_parse
     * calls below -- mt_alloc_node may realloc b->nodes for a child, which
     * would leave any such pointer dangling. Always re-index b->nodes[idx]. */

    const uint8_t *p = b->trie + off, *end = b->trie + b->size;
    uint64_t term;
    int k = mu_decode(p, end, &term);
    if (k == 0) { mt_fail(b, "malformed terminal-size ULEB"); return -1; }
    p += k;

    if (term) {
        /* Bounds-check BEFORE forming p+term: term is attacker-controlled
         * (decoded straight from the trie) and can be up to 2^64-1, so
         * computing p+term first and comparing pointers after is undefined
         * behaviour if it overflows -- comparing the byte COUNT against
         * `end - p` (always non-negative and in range here) has no such
         * hazard and gives the identical answer for every well-defined case. */
        if (term > (uint64_t)(end - p)) {
            mt_fail(b, "terminal size runs past the trie");
            return -1;
        }
        const uint8_t *tend = p + term;
        uint64_t flags;
        k = mu_decode(p, tend, &flags);
        if (k == 0) { mt_fail(b, "malformed export-flags ULEB"); return -1; }
        p += k;
        b->nodes[idx].has_term = 1;
        b->nodes[idx].flags = flags;
        if (flags & MT_EXPORT_REEXPORT) {
            uint64_t ord;
            k = mu_decode(p, tend, &ord);
            if (k == 0) { mt_fail(b, "malformed re-export ordinal ULEB"); return -1; }
            p += k;
            const uint8_t *namestart = p;
            while (p < tend && *p) p++;
            if (p >= tend) { mt_fail(b, "re-export import name has no terminator"); return -1; }
            b->nodes[idx].is_reexport = 1;
            b->nodes[idx].reexport_ordinal = ord;
            b->nodes[idx].reexport_name = namestart;
            b->nodes[idx].reexport_name_len = (uint32_t)(p - namestart);
            p++;   /* consume the NUL */
        } else if (flags & MT_EXPORT_STUB_AND_RESOLVER) {
            uint64_t stub, resolver;
            k = mu_decode(p, tend, &stub);
            if (k == 0) { mt_fail(b, "malformed stub-offset ULEB"); return -1; }
            p += k;
            k = mu_decode(p, tend, &resolver);
            if (k == 0) { mt_fail(b, "malformed resolver-offset ULEB"); return -1; }
            p += k;
            b->nodes[idx].is_stub_resolver = 1;
            b->nodes[idx].a1 = stub ? stub + b->shift : 0;
            b->nodes[idx].a2 = resolver ? resolver + b->shift : 0;
        } else {
            uint64_t addr;
            k = mu_decode(p, tend, &addr);
            if (k == 0) { mt_fail(b, "malformed export-address ULEB"); return -1; }
            p += k;
            b->nodes[idx].a1 = addr ? addr + b->shift : 0;
        }
        p = tend;
    }

    if (p >= end) { mt_fail(b, "truncated trie: no child-count byte"); return -1; }
    uint8_t nch = *p++;
    for (uint8_t i = 0; i < nch; i++) {
        const uint8_t *lbl = p;
        while (p < end && *p) p++;
        if (p >= end) { mt_fail(b, "edge label has no terminator"); return -1; }
        uint32_t lbl_len = (uint32_t)(p - lbl);
        p++;   /* consume the NUL */
        uint64_t coff;
        k = mu_decode(p, end, &coff);
        if (k == 0) { mt_fail(b, "malformed child-offset ULEB"); return -1; }
        p += k;
        if (coff >= (uint64_t)b->size) {
            mt_fail(b, "a child offset points outside the trie");
            return -1;
        }
        int32_t ci = mt_parse(b, (uint32_t)coff, depth + 1);
        if (ci < 0) return -1;
        if (mt_add_edge(b, (uint32_t)idx, lbl, lbl_len, ci) != 0) return -1;
    }
    return idx;
}

/* The payload length (everything after the terminal-size ULEB, before it) --
 * shared by mt_node_size and mt_serialize_node so the two can never disagree
 * about how many bytes a node's terminal data takes. */
static uint32_t mt_payload_len(const struct mt_node *n) {
    uint32_t plen = (uint32_t)mu_minlen(n->flags);
    if (n->is_reexport)
        plen += (uint32_t)mu_minlen(n->reexport_ordinal) + n->reexport_name_len + 1;
    else if (n->is_stub_resolver)
        plen += (uint32_t)mu_minlen(n->a1) + (uint32_t)mu_minlen(n->a2);
    else
        plen += (uint32_t)mu_minlen(n->a1);
    return plen;
}

static uint32_t mt_node_size(const struct mt_builder *b, const struct mt_node *n) {
    uint32_t sz;
    if (n->has_term) {
        uint32_t plen = mt_payload_len(n);
        sz = (uint32_t)mu_minlen(plen) + plen;
    } else {
        sz = 1;   /* terminal-size ULEB(0) */
    }
    sz += 1;   /* child-count byte */
    for (uint32_t i = 0; i < n->nedges; i++) {
        sz += n->edges[i].label_len + 1;
        sz += (uint32_t)mu_minlen(b->nodes[n->edges[i].child].out_off);
    }
    return sz;
}

/* Fixed-point layout: a node's SIZE depends on the byte WIDTH of its
 * children's offsets, which depends on the cumulative size of every node
 * before them -- including this one. Nodes are indexed in parse (pre-order
 * DFS) order, and a child's index is always higher than its parent's (this
 * rebuild refuses shared/revisited offsets, so the tree really is a tree),
 * so each pass can compute sizes left-to-right using the offsets the
 * PREVIOUS pass computed for anything not yet reached. Offsets only grow
 * across passes (a wider child offset never makes an earlier node smaller),
 * so this converges monotonically; capped at 10 passes -- one per possible
 * ULEB width -- matching the reference implementation this is adapted from. */
static int mt_layout(struct mt_builder *b, uint32_t *total_out) {
    for (int iter = 0; iter < 10; iter++) {
        int changed = 0;
        uint32_t off = 0;
        for (uint32_t i = 0; i < b->nnodes; i++) {
            if (b->nodes[i].out_off != off) { b->nodes[i].out_off = off; changed = 1; }
            uint32_t sz = mt_node_size(b, &b->nodes[i]);
            if (sz != b->nodes[i].out_sz) { b->nodes[i].out_sz = sz; changed = 1; }
            off += sz;
        }
        if (!changed) { *total_out = off; return 0; }
    }
    return -1;   /* did not converge -- refuse rather than emit a wrong trie */
}

static uint8_t *mt_put_uleb(uint8_t *p, uint64_t v) {
    int w = mu_minlen(v);
    mu_encode_fixed(p, v, w);   /* w == mu_minlen(v), so this cannot fail */
    return p + w;
}

/* Serialize one node into `out` at its laid-out offset, and check what got
 * written matches what mt_node_size predicted -- cheap insurance against the
 * two ever drifting apart (an internal-consistency check, not a defense
 * against attacker input: the input was already validated during parsing). */
static int mt_serialize_node(const struct mt_builder *b, uint8_t *out, const struct mt_node *n) {
    uint8_t *start = out + n->out_off;
    uint8_t *p = start;
    if (n->has_term) {
        uint32_t plen = mt_payload_len(n);
        p = mt_put_uleb(p, plen);
        p = mt_put_uleb(p, n->flags);
        if (n->is_reexport) {
            p = mt_put_uleb(p, n->reexport_ordinal);
            memcpy(p, n->reexport_name, n->reexport_name_len);
            p += n->reexport_name_len;
            *p++ = 0;
        } else if (n->is_stub_resolver) {
            p = mt_put_uleb(p, n->a1);
            p = mt_put_uleb(p, n->a2);
        } else {
            p = mt_put_uleb(p, n->a1);
        }
    } else {
        *p++ = 0;
    }
    *p++ = (uint8_t)n->nedges;   /* nedges <= 255: it was read from one byte */
    for (uint32_t i = 0; i < n->nedges; i++) {
        memcpy(p, n->edges[i].label, n->edges[i].label_len);
        p += n->edges[i].label_len;
        *p++ = 0;
        p = mt_put_uleb(p, b->nodes[n->edges[i].child].out_off);
    }
    if ((uint32_t)(p - start) != n->out_sz) return -1;   /* "cannot happen" */
    return 0;
}

int mt_trie_rebuild(const uint8_t *trie, uint32_t size, uint64_t shift,
                     uint8_t **out, uint32_t *out_size) {
    *out = NULL; *out_size = 0;
    if (size == 0) {
        fprintf(stderr, "trie: empty; refusing to rebuild\n");
        return -1;
    }

    struct mt_builder b;
    memset(&b, 0, sizeof b);
    b.trie = trie; b.size = size; b.shift = shift;
    b.seen = (uint8_t *)calloc(size, 1);
    if (!b.seen) {
        fprintf(stderr, "trie: out of memory\n");
        return -1;
    }

    int32_t root = mt_parse(&b, 0, 0);
    if (root < 0 || b.failed) { mt_free_builder(&b); return -1; }

    uint32_t total;
    if (mt_layout(&b, &total) != 0) {
        fprintf(stderr, "trie: layout did not converge after 10 passes; refusing to "
                        "rebuild rather than emit a possibly-wrong trie\n");
        mt_free_builder(&b);
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc(total ? total : 1);
    if (!buf) {
        fprintf(stderr, "trie: out of memory\n");
        mt_free_builder(&b);
        return -1;
    }
    for (uint32_t i = 0; i < b.nnodes; i++) {
        if (mt_serialize_node(&b, buf, &b.nodes[i]) != 0) {
            fprintf(stderr, "trie: internal error -- a node serialized to a different size "
                            "than its layout predicted; refusing rather than ship a "
                            "corrupt trie\n");
            free(buf);
            mt_free_builder(&b);
            return -1;
        }
    }

    mt_free_builder(&b);
    *out = buf; *out_size = total;
    return 0;
}
