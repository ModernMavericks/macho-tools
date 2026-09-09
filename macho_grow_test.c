/*
 * macho_grow_test.c — hermetic tests for the LC_FUNCTION_STARTS base re-encode
 * that macho_grow performs when it lowers the image base.
 *
 * THE bug this pins: change_dylib -grow lowers __TEXT.vmaddr by N to make header
 * room while keeping every section's VM address fixed. LC_FUNCTION_STARTS encodes
 * its FIRST delta relative to the image base, so after the grow that delta is N
 * too small and every function address avxemu reconstructs is N low — it then
 * can't map faulting instructions to functions and declines to patch them (a
 * SIGILL storm). The fix: add N to the leading delta, preserving its byte width
 * so the blob size is unchanged.
 *
 * Ground truth here is hand-computed (small ULEB values, synthetic function
 * address lists), so the test is host-agnostic. Build:
 *   clang -O2 -Wno-unused-function -o /tmp/mgtest macho_grow_test.c && /tmp/mgtest
 */
#include "macho_grow.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(cond, msg, ...) do { if (!(cond)) { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); fails++; } } while (0)

/* ---- ULEB128 primitives ---- */
static void test_uleb_decode(void) {
    uint64_t v; int n;
    uint8_t a[] = {0x00};                 n = mu_decode(a, a+1, &v); CHECK(n==1 && v==0,      "uleb 0x00 -> 0 (got n=%d v=%llu)", n, (unsigned long long)v);
    uint8_t b[] = {0x7f};                 n = mu_decode(b, b+1, &v); CHECK(n==1 && v==127,    "uleb 0x7f -> 127");
    uint8_t c[] = {0x80,0x01};            n = mu_decode(c, c+2, &v); CHECK(n==2 && v==128,    "uleb 80 01 -> 128");
    uint8_t d[] = {0xc0,0x15};            n = mu_decode(d, d+2, &v); CHECK(n==2 && v==2752,   "uleb c0 15 -> 2752 (the 2.1.227 leading delta)");
    uint8_t e[] = {0xff,0x7f};            n = mu_decode(e, e+2, &v); CHECK(n==2 && v==16383,  "uleb ff 7f -> 16383");
    uint8_t f[] = {0x80,0x80,0x01};       n = mu_decode(f, f+3, &v); CHECK(n==3 && v==16384,  "uleb 80 80 01 -> 16384");
    /* runs off the end (continuation bit set, no more bytes) -> malformed */
    uint8_t g[] = {0x80};                 n = mu_decode(g, g+1, &v); CHECK(n==0,              "uleb truncated -> 0 (got n=%d)", n);
}

static void test_uleb_minlen(void) {
    CHECK(mu_minlen(0)==1,       "minlen(0)=1");
    CHECK(mu_minlen(127)==1,     "minlen(127)=1");
    CHECK(mu_minlen(128)==2,     "minlen(128)=2");
    CHECK(mu_minlen(2752)==2,    "minlen(2752)=2");
    CHECK(mu_minlen(16383)==2,   "minlen(16383)=2");
    CHECK(mu_minlen(16384)==3,   "minlen(16384)=3");
    CHECK(mu_minlen(6848)==2,    "minlen(6848)=2  (2752 + one page)");
}

static void test_uleb_encode_fixed(void) {
    uint8_t buf[8]; uint64_t v; int n;
    /* minimal width */
    CHECK(mu_encode_fixed(buf, 6848, 2)==1, "encode 6848 in 2 bytes ok");
    n = mu_decode(buf, buf+2, &v); CHECK(n==2 && v==6848, "  round-trips to 6848");
    CHECK(buf[0]==0xc0 && buf[1]==0x35, "  bytes are c0 35 (expected 2.1.227 fixed leading delta)");
    /* non-minimal padding: 2752 forced into 3 bytes */
    memset(buf,0xAA,sizeof buf);
    CHECK(mu_encode_fixed(buf, 2752, 3)==1, "encode 2752 padded to 3 bytes ok");
    n = mu_decode(buf, buf+3, &v); CHECK(n==3 && v==2752, "  padded still decodes to 2752 in 3 bytes");
    /* does not fit: 16384 needs 3, width 2 -> refuse */
    CHECK(mu_encode_fixed(buf, 16384, 2)==0, "encode 16384 in 2 bytes refused");
}

/* ---- the leading-delta re-encode ---- */
static void test_reencode_same_width(void) {
    /* leading delta 2752 (c0 15) + a tail that must be preserved verbatim */
    uint8_t blob[] = {0xc0,0x15, /*tail*/ 0x50, 0x81,0x01, 0x00};
    uint8_t saved[sizeof blob]; memcpy(saved, blob, sizeof blob);
    int r = mg_reencode_funcstarts_base(blob, sizeof blob, 0x1000);
    CHECK(r==1, "reencode +0x1000 succeeds in place (got %d)", r);
    CHECK(blob[0]==0xc0 && blob[1]==0x35, "leading delta became c0 35 (2752+4096=6848)");
    CHECK(memcmp(blob+2, saved+2, sizeof blob - 2)==0, "tail bytes untouched");
}

static void test_reencode_widen_refuses(void) {
    /* leading delta 16000 (0x3e80): 16000+4096=20096 needs 3 bytes, was 2 -> refuse */
    uint8_t blob[] = {0x80,0x7d, /*tail*/ 0x40, 0x00};   /* 0x80,0x7d = 16000 */
    uint64_t chk; int n = mu_decode(blob, blob+2, &chk);
    CHECK(n==2 && chk==16000, "precondition: leading delta decodes to 16000");
    uint8_t saved[sizeof blob]; memcpy(saved, blob, sizeof blob);
    int r = mg_reencode_funcstarts_base(blob, sizeof blob, 0x1000);
    CHECK(r==0, "reencode refuses when the delta would widen (got %d)", r);
    CHECK(memcmp(blob, saved, sizeof blob)==0, "blob left untouched on refusal");
}

static void test_reencode_nonminimal_original_preserved(void) {
    /* leading delta 2752 encoded NON-minimally in 3 bytes (c0 95 00); +0x1000 must
     * stay 3 bytes and still decode correctly. */
    uint8_t blob[] = {0xc0,0x95,0x00, /*tail*/ 0x50, 0x00};
    int r = mg_reencode_funcstarts_base(blob, sizeof blob, 0x1000);
    CHECK(r==1, "reencode of a non-minimally-encoded leading delta succeeds");
    uint64_t v; int n = mu_decode(blob, blob+3, &v);
    CHECK(n==3 && v==6848, "leading delta still 3 bytes, decodes to 6848 (got n=%d v=%llu)", n, (unsigned long long)v);
    CHECK(blob[3]==0x50, "tail preserved");
}

static void test_reencode_malformed(void) {
    uint8_t empty[1] = {0};
    CHECK(mg_reencode_funcstarts_base(empty, 0, 0x1000)==-1, "empty blob -> -1");
    uint8_t trunc[] = {0x80};   /* continuation with no successor */
    CHECK(mg_reencode_funcstarts_base(trunc, 1, 0x1000)==-1, "truncated leading ULEB -> -1");
}

/* ---- THE INVARIANT: the grow must move no function ---- */
static void build_funcstarts(uint8_t *out, int *outlen, uint64_t base,
                             const uint64_t *addrs, int n) {
    int len = 0; uint64_t prev = base;
    for (int i = 0; i < n; i++) {
        int w = mu_minlen(addrs[i] - prev);
        mu_encode_fixed(out + len, addrs[i] - prev, w);
        len += w; prev = addrs[i];
    }
    out[len++] = 0x00;   /* terminator */
    *outlen = len;
}

static void test_invariant_addresses_preserved(void) {
    const uint64_t base = 0x100000000ull;
    const uint32_t N = 0x1000;
    /* a realistic ascending function list; first delta 0xac0 stays 2 bytes under +N */
    uint64_t addrs[] = { base+0xac0, base+0xb30, base+0x1200, base+0x1abc, base+0x2f00 };
    int n = (int)(sizeof addrs / sizeof addrs[0]);

    uint8_t blob[64]; int blen; build_funcstarts(blob, &blen, base, addrs, n);
    uint32_t orig_blen = (uint32_t)blen;

    /* grow: lower the base by N and re-encode the leading delta */
    int r = mg_reencode_funcstarts_base(blob, (uint32_t)blen, N);
    CHECK(r==1, "invariant setup: reencode succeeds");
    CHECK((uint32_t)blen == orig_blen, "blob size unchanged by reencode");

    /* decode at the LOWERED base; every absolute address must be identical */
    uint64_t got[16]; int gn = mg_funcstarts_decode(blob, (uint32_t)blen, base - N, got, 16);
    CHECK(gn == n, "same function count after grow (got %d want %d)", gn, n);
    for (int i = 0; i < n && i < gn; i++)
        CHECK(got[i] == addrs[i], "function[%d] address preserved: got %#llx want %#llx",
              i, (unsigned long long)got[i], (unsigned long long)addrs[i]);
}

/* ---- __TEXT,__init_offsets re-base ----
 * Entries are offsets from the mach header, so lowering the base leaves them
 * all `grow` too small. Sections are matched by TYPE (S_INIT_FUNC_OFFSETS)
 * rather than by name: the name is a linker convention, the type is what the
 * format guarantees. Driven against a synthetic image because the 10.9
 * toolchain cannot emit an __init_offsets section to build a fixture from. */
static void test_init_offsets_rebase(void) {
    static uint8_t img[8192];
    memset(img, 0, sizeof img);
    struct mach_header_64 *h = (struct mach_header_64 *)img;
    h->magic = MH_MAGIC_64;
    h->ncmds = 1;
    struct segment_command_64 *seg = (struct segment_command_64 *)(img + sizeof *h);
    seg->cmd = LC_SEGMENT_64;
    seg->cmdsize = sizeof(*seg) + sizeof(struct section_64);
    strcpy(seg->segname, "__TEXT");
    seg->nsects = 1;
    h->sizeofcmds = seg->cmdsize;
    struct section_64 *s = (struct section_64 *)((uint8_t *)seg + sizeof *seg);
    strncpy(s->sectname, "__init_offsets", sizeof s->sectname);
    strncpy(s->segname, "__TEXT", sizeof s->segname);
    s->offset = 4096;
    s->size = 3 * sizeof(uint32_t);
    s->flags = S_INIT_FUNC_OFFSETS;
    uint32_t *e = (uint32_t *)(img + 4096);
    e[0] = 0x1000; e[1] = 0x2000; e[2] = 0x3000;

    CHECK(mg_init_offsets_pass(img, sizeof img, 0x1000, 1) == 0, "init_offsets patch returns 0");
    CHECK(e[0] == 0x2000 && e[1] == 0x3000 && e[2] == 0x4000,
          "every entry gained grow (got %u %u %u)", e[0], e[1], e[2]);

    /* A section of another type must be left alone, even named __init_offsets. */
    s->flags = S_REGULAR;
    e[0] = 0x1000;
    CHECK(mg_init_offsets_pass(img, sizeof img, 0x1000, 1) == 0 && e[0] == 0x1000,
          "sections of other types untouched (got %u)", e[0]);

    /* An entry that would wrap is refused by the audit, before anything moves. */
    s->flags = S_INIT_FUNC_OFFSETS;
    e[0] = 0xffffffffu;
    CHECK(mg_init_offsets_pass(img, sizeof img, 0x1000, 0) == -1, "overflowing entry refused");
}

/* ---- the whole grow, end to end ----
 * Every other case here calls one helper directly. That is how a duplicated
 * re-base survived review: two functions each added `grow` to the same
 * __init_offsets entries, mg_grow_header called both, and no test ran the path
 * that used them. This builds the smallest image mg_grow_header will accept and
 * checks the entries afterwards, so any second application shows up as 2*grow.
 *
 * Deliberately carries no LC_FUNCTION_STARTS and no export trie: both are
 * audited separately, and leaving them out keeps this about the one structure.
 */
/* opts: MG_T_DICE adds an LC_DATA_IN_CODE whose entries are base-relative;
 * MG_T_UNWIND adds a __TEXT,__unwind_info section. macho_grow rebases neither,
 * so a grow of an image carrying either must refuse rather than corrupt it. */
/* offsets within the synthetic __unwind_info section */
#define UW_PERS_OFF  28
#define UW_IDX_OFF   32
#define UW_LSDA_OFF  56
#define UW_LSDA_END  64
#define UW_PAGE_OFF  72
#define UW_ENT_OFF   80
#define UW32(b, secoff, off) (*(uint32_t *)((b) + (secoff) + (off)))

#define MG_T_DICE   1
#define MG_T_UNWIND 2
#define MG_T_TRIE   4
#define MG_T_UNKNOWN_LC 8    /* a load command we have never classified */
#define MG_T_LOH   16    /* LC_LINKER_OPTIMIZATION_HINT: base-relative, unhandled */
#define MG_T_ODDSECT 32  /* a section whose TYPE we do not know */
#define MG_T_FUNCSTARTS 64
#define FS_OFF 7680
#define TRIE_OFF    7168
static uint8_t *build_image(size_t *fsize_out, uint32_t *sect_off_out, int opts) {
    const size_t fsize = 8192;
    uint8_t *buf = (uint8_t *)calloc(1, fsize);

    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    h->magic = MH_MAGIC_64;
    h->filetype = MH_EXECUTE;
    h->flags = MH_PIE;
    h->ncmds = 2;

    struct segment_command_64 *pz = (struct segment_command_64 *)(buf + sizeof *h);
    pz->cmd = LC_SEGMENT_64;
    pz->cmdsize = sizeof *pz;
    strcpy(pz->segname, "__PAGEZERO");
    pz->vmaddr = 0;
    pz->vmsize = 0x100000000ull;   /* room to lower the base into */
    pz->fileoff = 0;
    pz->filesize = 0;              /* filesize 0 keeps it out of the __TEXT probe */

    struct segment_command_64 *tx = (struct segment_command_64 *)((uint8_t *)pz + pz->cmdsize);
    tx->cmd = LC_SEGMENT_64;
    tx->cmdsize = sizeof *tx + (opts & MG_T_UNWIND ? 2 : 1) * sizeof(struct section_64);
    strcpy(tx->segname, "__TEXT");
    tx->vmaddr = 0x100000000ull;
    tx->vmsize = fsize;
    tx->fileoff = 0;
    tx->filesize = fsize;
    tx->nsects = (opts & MG_T_UNWIND) ? 2 : 1;

    struct section_64 *sc = (struct section_64 *)((uint8_t *)tx + sizeof *tx);
    strncpy(sc->sectname, "__init_offsets", sizeof sc->sectname);
    strncpy(sc->segname, "__TEXT", sizeof sc->segname);
    sc->addr = 0x100001000ull;
    sc->size = 2 * sizeof(uint32_t);
    sc->offset = 4096;
    sc->flags = S_INIT_FUNC_OFFSETS;   /* a real one carries both name and type */

    h->sizeofcmds = (uint32_t)(pz->cmdsize + tx->cmdsize);

    if (opts & MG_T_UNWIND) {
        struct section_64 *uw = sc + 1;
        strncpy(uw->sectname, "__unwind_info", sizeof uw->sectname);
        strncpy(uw->segname,  "__TEXT",        sizeof uw->segname);
        uw->addr = 0x100002000ull;
        uw->size = 128;
        uw->offset = 5120;
        uw->flags = S_REGULAR;   /* the TYPE says nothing here; the NAME is what matters */

        /* A compact-unwind section with one of every field family, so the test
         * can tell a handler that bumps the right things from one that bumps
         * everything. Layout mirrors the real format. */
        uint32_t *h32 = (uint32_t *)(buf + uw->offset);
        h32[0] = 1;                 /* version */
        h32[1] = 0;                 /* commonEncodingsArraySectionOffset */
        h32[2] = 0;                 /* commonEncodingsArrayCount */
        h32[3] = UW_PERS_OFF;       /* personalityArraySectionOffset */
        h32[4] = 1;                 /* personalityArrayCount */
        h32[5] = UW_IDX_OFF;        /* indexSectionOffset */
        h32[6] = 2;                 /* indexCount (one real entry + the sentinel) */

        UW32(buf, uw->offset, UW_PERS_OFF)      = 0x9000;   /* base-relative -> GOT */

        UW32(buf, uw->offset, UW_IDX_OFF + 0)   = 0x1000;   /* functionOffset  BASE-REL */
        UW32(buf, uw->offset, UW_IDX_OFF + 4)   = UW_PAGE_OFF; /* page   section-rel */
        UW32(buf, uw->offset, UW_IDX_OFF + 8)   = UW_LSDA_OFF; /* lsda   section-rel */
        UW32(buf, uw->offset, UW_IDX_OFF + 12)  = 0x8000;   /* sentinel fnOff  BASE-REL */
        UW32(buf, uw->offset, UW_IDX_OFF + 16)  = 0;        /* sentinel has no page */
        UW32(buf, uw->offset, UW_IDX_OFF + 20)  = UW_LSDA_END;

        UW32(buf, uw->offset, UW_LSDA_OFF + 0)  = 0x1100;   /* lsda functionOffset BASE-REL */
        UW32(buf, uw->offset, UW_LSDA_OFF + 4)  = 0x7000;   /* lsdaOffset          BASE-REL */

        UW32(buf, uw->offset, UW_PAGE_OFF + 0)  = 3;        /* kind = COMPRESSED */
        *(uint16_t *)(buf + uw->offset + UW_PAGE_OFF + 4) = 8;  /* entryPageOffset */
        *(uint16_t *)(buf + uw->offset + UW_PAGE_OFF + 6) = 2;  /* entryCount */
        /* Compressed entries: low 24 bits are a DELTA from this page's own
         * first-level functionOffset. Invariant under a uniform bump -- bumping
         * them is the silent corruption this test exists to catch. */
        UW32(buf, uw->offset, UW_ENT_OFF + 0)   = 0x00000010u | (1u << 24);
        UW32(buf, uw->offset, UW_ENT_OFF + 4)   = 0x00000040u | (2u << 24);
    }

    uint8_t *lcend = (uint8_t *)tx + tx->cmdsize;

    if (opts & MG_T_DICE) {
        struct linkedit_data_command *dc = (struct linkedit_data_command *)lcend;
        dc->cmd = LC_DATA_IN_CODE;
        dc->cmdsize = sizeof *dc;
        dc->dataoff = 6144;
        dc->datasize = 16;       /* two 8-byte entries */
        h->ncmds++; h->sizeofcmds += dc->cmdsize; lcend += dc->cmdsize;
        /* two data_in_code_entry: { uint32 offset; uint16 length; uint16 kind }.
         * Only `offset` is base-relative; length and kind must survive intact. */
        UW32(buf, dc->dataoff, 0) = 0x1500;
        *(uint16_t *)(buf + dc->dataoff +  4) = 0x20;
        *(uint16_t *)(buf + dc->dataoff +  6) = 4;      /* DICE_KIND_JUMP_TABLE32 */
        UW32(buf, dc->dataoff, 8) = 0x2500;
        *(uint16_t *)(buf + dc->dataoff + 12) = 0x40;
        *(uint16_t *)(buf + dc->dataoff + 14) = 4;
    }

    if (opts & MG_T_TRIE) {
        struct dyld_info_command *di = (struct dyld_info_command *)lcend;
        di->cmd = LC_DYLD_INFO_ONLY;
        di->cmdsize = sizeof *di;
        di->export_off = TRIE_OFF;
        di->export_size = 17;
        h->ncmds++; h->sizeofcmds += di->cmdsize; lcend += di->cmdsize;

        /* A hand-built export trie, 17 bytes:
         *   root: no terminal, two children "A" -> 8, "B" -> 13
         *   node A: terminal, flags 0, address 0x1000 (2-byte ULEB)
         *   node B: terminal, flags 0, address 0 -- the __mh_execute_header
         *           case, which names the header and must STAY 0. */
        static const uint8_t trie[17] = {
            0x00, 0x02,
            'A', 0x00, 8,
            'B', 0x00, 13,
            0x03, 0x00, 0x80, 0x20, 0x00,     /* A: termsz 3, flags 0, addr 0x1000, 0 kids */
            0x02, 0x00, 0x00, 0x00            /* B: termsz 2, flags 0, addr 0,      0 kids */
        };
        memcpy(buf + TRIE_OFF, trie, sizeof trie);
    }

    if (opts & MG_T_UNKNOWN_LC) {
        struct load_command *xc = (struct load_command *)lcend;
        xc->cmd = 0x7fff;                 /* not a real load command */
        xc->cmdsize = sizeof *xc;
        h->ncmds++; h->sizeofcmds += xc->cmdsize; lcend += xc->cmdsize;
    }
    if (opts & MG_T_LOH) {
        struct linkedit_data_command *lc2 = (struct linkedit_data_command *)lcend;
        lc2->cmd = LC_LINKER_OPTIMIZATION_HINT;
        lc2->cmdsize = sizeof *lc2;
        lc2->dataoff = 6656; lc2->datasize = 8;
        h->ncmds++; h->sizeofcmds += lc2->cmdsize; lcend += lc2->cmdsize;
    }
    if (opts & MG_T_FUNCSTARTS) {
        struct linkedit_data_command *fc = (struct linkedit_data_command *)lcend;
        fc->cmd = LC_FUNCTION_STARTS;
        fc->cmdsize = sizeof *fc;
        fc->dataoff = FS_OFF; fc->datasize = 5;
        h->ncmds++; h->sizeofcmds += fc->cmdsize; lcend += fc->cmdsize;
        /* ULEB deltas: first is from the image base. 0x1000 then +0x1000, so the
         * function starts are base+0x1000 and base+0x2000 -- exactly where the
         * two __init_offsets entries point. */
        static const uint8_t fsb[5] = { 0x80, 0x20, 0x80, 0x20, 0x00 };
        memcpy(buf + FS_OFF, fsb, sizeof fsb);
    }
    if (opts & MG_T_ODDSECT) sc->flags = 0x7e;   /* unknown SECTION_TYPE */

    uint32_t *e = (uint32_t *)(buf + sc->offset);
    e[0] = 0x1000; e[1] = 0x2000;

    *fsize_out = fsize;
    *sect_off_out = sc->offset;
    return buf;
}

static uint8_t *build_growable_image(size_t *fsize_out, uint32_t *sect_off_out) {
    return build_image(fsize_out, sect_off_out, 0);
}

/* After a grow, find __init_offsets again -- its file offset moved with the data. */
static uint32_t *find_init_offsets(uint8_t *buf) {
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    uint8_t *lcp = buf + sizeof *h;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *seg = (struct segment_command_64 *)lcp;
            struct section_64 *sect = (struct section_64 *)(lcp + sizeof *seg);
            for (uint32_t j = 0; j < seg->nsects; j++)
                if ((sect[j].flags & SECTION_TYPE) == S_INIT_FUNC_OFFSETS)
                    return (uint32_t *)(buf + sect[j].offset);
        }
        lcp += lc->cmdsize;
    }
    return NULL;
}

static void test_grow_applies_init_offsets_once(void) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_growable_image(&fsize, &sect_off);
    const uint32_t grow = 0x1000;

    int r = mg_grow_header(&buf, &fsize, grow);
    CHECK(r == 0, "mg_grow_header succeeds on the synthetic image (got %d)", r);
    if (r != 0) { free(buf); return; }

    uint32_t *e = find_init_offsets(buf);
    CHECK(e != NULL, "__init_offsets still locatable after the grow");
    if (e) {
        CHECK(e[0] == 0x1000 + grow, "entry 0 gained grow exactly once: got %#x want %#x",
              e[0], 0x1000 + grow);
        CHECK(e[1] == 0x2000 + grow, "entry 1 gained grow exactly once: got %#x want %#x",
              e[1], 0x2000 + grow);
    }
    free(buf);
}

/* ---- refuse what we cannot rebase ----
 * Both structures below store offsets from the image base, exactly like
 * __init_offsets and the function-starts leading delta. macho_grow relocates
 * LC_DATA_IN_CODE's blob but never rewrites the offsets inside it, and does not
 * mention __unwind_info at all. Until handlers exist, growing such an image MUST
 * fail: a silent success ships a binary whose data-in-code ranges and
 * compact-unwind entries are all `grow` bytes low, which nothing notices until
 * something unwinds. A refusal leaves the caller's buffer byte-identical.
 */
static void check_refused_unchanged(const char *what, int opts) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_image(&fsize, &sect_off, opts);
    size_t fsize0 = fsize;
    uint8_t *before = (uint8_t *)malloc(fsize0);
    memcpy(before, buf, fsize0);

    int r = mg_grow_header(&buf, &fsize, 0x1000);
    CHECK(r == -1, "%s: mg_grow_header refuses (got %d)", what, r);
    CHECK(fsize == fsize0, "%s: size unchanged on refusal (got %zu want %zu)",
          what, fsize, fsize0);
    if (fsize == fsize0)
        CHECK(memcmp(before, buf, fsize0) == 0,
              "%s: buffer byte-identical on refusal", what);
    free(before);
    free(buf);
}

static uint8_t *find_dice(uint8_t *buf) {
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    uint8_t *lcp = buf + sizeof *h;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_DATA_IN_CODE)
            return buf + ((struct linkedit_data_command *)lcp)->dataoff;
        lcp += lc->cmdsize;
    }
    return NULL;
}

/* Every entry's `offset` is measured from the image base; `length` and `kind`
 * are not offsets at all. A handler that treats the entry as three bumpable
 * words would pass a "did it change" test and corrupt every range. */
static void test_grow_rebases_data_in_code(void) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_image(&fsize, &sect_off, MG_T_DICE);
    const uint32_t g = 0x1000;

    int r = mg_grow_header(&buf, &fsize, g);
    CHECK(r == 0, "grow succeeds on an image with LC_DATA_IN_CODE (got %d)", r);
    if (r != 0) { free(buf); return; }

    uint8_t *d = find_dice(buf);
    CHECK(d != NULL, "LC_DATA_IN_CODE still locatable after the grow");
    if (!d) { free(buf); return; }
    CHECK(*(uint32_t *)(d + 0) == 0x1500 + g, "entry 0 offset gains grow: got %#x",
          *(uint32_t *)(d + 0));
    CHECK(*(uint32_t *)(d + 8) == 0x2500 + g, "entry 1 offset gains grow: got %#x",
          *(uint32_t *)(d + 8));
    CHECK(*(uint16_t *)(d +  4) == 0x20 && *(uint16_t *)(d +  6) == 4,
          "entry 0 length/kind UNTOUCHED");
    CHECK(*(uint16_t *)(d + 12) == 0x40 && *(uint16_t *)(d + 14) == 4,
          "entry 1 length/kind UNTOUCHED");
    free(buf);
}

/* After a grow, __unwind_info's file offset moved with the data. */
static uint8_t *find_unwind(uint8_t *buf) {
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    uint8_t *lcp = buf + sizeof *h;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *seg = (struct segment_command_64 *)lcp;
            struct section_64 *sect = (struct section_64 *)(lcp + sizeof *seg);
            for (uint32_t j = 0; j < seg->nsects; j++)
                if (strncmp(sect[j].sectname, "__unwind_info", 16) == 0)
                    return buf + sect[j].offset;
        }
        lcp += lc->cmdsize;
    }
    return NULL;
}

/* The handler must bump the four base-relative field families and leave the
 * compressed second-level entries ALONE -- those are deltas from their own
 * page's first-level functionOffset, so a uniform bump leaves them correct and
 * bumping them corrupts the tables silently. */
static void test_grow_rebases_unwind_info(void) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_image(&fsize, &sect_off, MG_T_UNWIND);
    const uint32_t g = 0x1000;

    int r = mg_grow_header(&buf, &fsize, g);
    CHECK(r == 0, "grow succeeds on an image with __unwind_info (got %d)", r);
    if (r != 0) { free(buf); return; }

    uint8_t *u = find_unwind(buf);
    CHECK(u != NULL, "__unwind_info still locatable after the grow");
    if (!u) { free(buf); return; }
    uint32_t *at = (uint32_t *)u;
#define UW_IS(off, want, what) \
    CHECK(*(uint32_t *)(u + (off)) == (uint32_t)(want), \
          "%s: got %#x want %#x", what, *(uint32_t *)(u + (off)), (uint32_t)(want))

    UW_IS(UW_PERS_OFF,      0x9000 + g, "personality entry gains grow");
    UW_IS(UW_IDX_OFF + 0,   0x1000 + g, "first-level functionOffset gains grow");
    UW_IS(UW_IDX_OFF + 12,  0x8000 + g, "sentinel functionOffset gains grow");
    UW_IS(UW_LSDA_OFF + 0,  0x1100 + g, "LSDA functionOffset gains grow");
    UW_IS(UW_LSDA_OFF + 4,  0x7000 + g, "LSDA lsdaOffset gains grow");

    /* section-relative fields must NOT move */
    UW_IS(UW_IDX_OFF + 4,  UW_PAGE_OFF, "page section-offset unchanged");
    UW_IS(UW_IDX_OFF + 8,  UW_LSDA_OFF, "LSDA section-offset unchanged");
    UW_IS(3 * 4,           UW_PERS_OFF, "personality section-offset unchanged");

    /* THE trap: compressed entries are deltas and must be untouched */
    UW_IS(UW_ENT_OFF + 0, 0x00000010u | (1u << 24), "compressed entry 0 UNTOUCHED");
    UW_IS(UW_ENT_OFF + 4, 0x00000040u | (2u << 24), "compressed entry 1 UNTOUCHED");
    (void)at;
#undef UW_IS
    free(buf);
}

/* ---- mg_verify: the grow must move nothing ----
 * The invariant is not "the entries changed by grow", it is "the RESOLVED
 * addresses did not change". Stating it that way is what makes the check catch
 * bugs it was not written for: a handler that never ran, one that ran twice
 * (PR #10 -- two correct __init_offsets re-basers met in a merge and composed
 * into 2*grow), or one that ran with the wrong delta all look the same to it.
 *
 * The two failing cases below are the point. A verify that cannot fail is not a
 * verify, so each one perturbs the grown image by exactly one handler's worth of
 * work and asserts mg_verify rejects it.
 */
static void test_verify_accepts_a_correct_grow(void) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_growable_image(&fsize, &sect_off);
    mg_snapshot snap;
    CHECK(mg_snapshot_take(buf, fsize, &snap) == 0, "snapshot taken before the grow");
    CHECK(snap.n == 2, "snapshot found both __init_offsets entries (got %u)", snap.n);

    int r = mg_grow_header(&buf, &fsize, 0x1000);
    CHECK(r == 0, "grow succeeds (got %d)", r);
    if (r == 0)
        CHECK(mg_verify(buf, fsize, &snap) == 0, "verify ACCEPTS a correct grow");
    mg_snapshot_free(&snap);
    free(buf);
}

/* Perturb every __init_offsets entry by `delta` after a correct grow, then
 * demand mg_verify notices. delta=+grow is the double-apply; -grow is a handler
 * that never ran. */
static void check_verify_rejects(const char *what, int32_t delta) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_growable_image(&fsize, &sect_off);
    mg_snapshot snap;
    if (mg_snapshot_take(buf, fsize, &snap) != 0) { free(buf); CHECK(0, "%s: snapshot", what); return; }
    if (mg_grow_header(&buf, &fsize, 0x1000) != 0) {
        mg_snapshot_free(&snap); free(buf); CHECK(0, "%s: grow", what); return;
    }
    uint32_t *e = find_init_offsets(buf);
    if (e) { e[0] = (uint32_t)(e[0] + delta); e[1] = (uint32_t)(e[1] + delta); }
    CHECK(mg_verify(buf, fsize, &snap) == -1, "verify REJECTS %s", what);
    mg_snapshot_free(&snap);
    free(buf);
}

static void test_verify_rejects_double_apply(void) {
    check_verify_rejects("a double-applied re-base (the PR #10 defect)", 0x1000);
}

static void test_verify_rejects_handler_that_never_ran(void) {
    check_verify_rejects("a handler that never ran", -0x1000);
}

/* Coverage, not just correctness: a handler is only as safe as verify's
 * willingness to contradict it. If mg_collect ever stops walking compact unwind,
 * the count assertion fails here rather than silently going unwatched. */
static void test_verify_watches_unwind_info(void) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_image(&fsize, &sect_off, MG_T_UNWIND);
    mg_snapshot snap;
    CHECK(mg_snapshot_take(buf, fsize, &snap) == 0, "snapshot with unwind taken");
    /* 2 __init_offsets + 1 personality + 2 first-level (incl. sentinel)
     * + 2 LSDA fields = 7. The compressed entries are deltas and must NOT
     * be counted -- if they were, this would be 9. */
    CHECK(snap.n == 7, "verify watches all 7 base-relative unwind+init fields (got %u)", snap.n);

    if (mg_grow_header(&buf, &fsize, 0x1000) != 0) {
        CHECK(0, "grow succeeded"); mg_snapshot_free(&snap); free(buf); return;
    }
    /* Perturb one unwind field the handler is responsible for. */
    uint8_t *u = find_unwind(buf);
    if (u) *(uint32_t *)(u + UW_IDX_OFF) += 4;
    CHECK(mg_verify(buf, fsize, &snap) == -1,
          "verify REJECTS a perturbed first-level functionOffset");
    mg_snapshot_free(&snap);
    free(buf);
}

/* The export trie stores each address as a ULEB offset from the image base. The
 * fix that makes this tractable: adding `grow` never widens the encoding on any
 * real binary (measured across all 670 entries of Claude Code 2.1.263 at 4K, 8K
 * and 16K), so the address is re-encoded at its ORIGINAL byte width and the trie
 * -- and every __LINKEDIT offset after it -- keeps its size.
 *
 * __mh_execute_header is exported at 0 and must stay 0: it names the header,
 * which moved down with the base, so 0 is still correct. */
static uint8_t *find_trie(uint8_t *buf) {
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    uint8_t *lcp = buf + sizeof *h;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_DYLD_INFO_ONLY || lc->cmd == LC_DYLD_INFO)
            return buf + ((struct dyld_info_command *)lcp)->export_off;
        lcp += lc->cmdsize;
    }
    return NULL;
}

static uint32_t trie_size(uint8_t *buf) {
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    uint8_t *lcp = buf + sizeof *h;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_DYLD_INFO_ONLY || lc->cmd == LC_DYLD_INFO)
            return ((struct dyld_info_command *)lcp)->export_size;
        lcp += lc->cmdsize;
    }
    return 0;
}

static void test_grow_rebases_export_trie(void) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_image(&fsize, &sect_off, MG_T_TRIE);
    const uint32_t g = 0x1000;

    int r = mg_grow_header(&buf, &fsize, g);
    CHECK(r == 0, "grow succeeds on an image with an export trie (got %d)", r);
    if (r != 0) { free(buf); return; }

    CHECK(trie_size(buf) == 17, "trie size UNCHANGED (got %u) -- no __LINKEDIT resize",
          trie_size(buf));
    uint8_t *t = find_trie(buf);
    CHECK(t != NULL, "export trie still locatable");
    if (!t) { free(buf); return; }
    /* node A's address, still a 2-byte ULEB at the same place */
    uint64_t a = 0; int n = mu_decode(t + 10, t + 17, &a);
    CHECK(n == 2, "node A address still encoded in 2 bytes (got %d)", n);
    CHECK(a == 0x1000 + g, "node A address gains grow: got %#llx want %#llx",
          (unsigned long long)a, (unsigned long long)(0x1000 + g));
    CHECK(t[15] == 0x00, "__mh_execute_header-style export STAYS 0 (got %#x)", t[15]);
    free(buf);
}

/* ---- THE gap this task closes: a trie that genuinely WIDENS under grow ----
 * Node A's address is 16000 (0x3E80): a 2-byte ULEB (16000 < 16384), but
 * 16000 + 0x1000 = 20096 needs 3 (>= 16384). An in-place patch (the path
 * above) cannot absorb that -- see mg_trie_node's `return 1`. Before this
 * task, mg_grow_header refused outright; now it must REBUILD the trie via
 * src/trie.c's mt_trie_rebuild and, when the rebuild no longer fits the
 * original space (it doesn't here: 18 bytes where there were 17), grow
 * __LINKEDIT to hold it.
 *
 * This fixture is deliberately NOT build_image()'s 8192-byte layout: that
 * fixture has trailing zero padding past its trie, which would make
 * __LINKEDIT's declared end fall short of the file's actual end -- exactly
 * the "unknown trailing data" shape mg_grow_header's append path refuses
 * rather than guess about. This one is sized so __LINKEDIT's export trie is
 * the LAST thing in the file, byte for byte, so the append path's own
 * precondition holds. */
#define WT_LC_END   ((uint32_t)(sizeof(struct mach_header_64) \
                     + sizeof(struct segment_command_64)                         /* __PAGEZERO */ \
                     + sizeof(struct segment_command_64) + sizeof(struct section_64) /* __TEXT */ \
                     + sizeof(struct segment_command_64)                         /* __LINKEDIT */ \
                     + sizeof(struct dyld_info_command)))
#define WT_SECT_OFF 4096u
#define WT_TEXT_FILESIZE 4352u          /* > WT_SECT_OFF+4, page-friendly */
#define WT_TRIE_OFF WT_TEXT_FILESIZE    /* __LINKEDIT starts right after __TEXT */
#define WT_TRIE_SIZE 17u
#define WT_FSIZE (WT_TRIE_OFF + WT_TRIE_SIZE)   /* trie is the LAST file byte */

static uint8_t *build_widening_trie_image(size_t *fsize_out) {
    uint8_t *buf = (uint8_t *)calloc(1, WT_FSIZE);

    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    h->magic = MH_MAGIC_64;
    h->filetype = MH_EXECUTE;
    h->flags = MH_PIE;
    h->ncmds = 4;

    struct segment_command_64 *pz = (struct segment_command_64 *)(buf + sizeof *h);
    pz->cmd = LC_SEGMENT_64;
    pz->cmdsize = sizeof *pz;
    strcpy(pz->segname, "__PAGEZERO");
    pz->vmaddr = 0; pz->vmsize = 0x100000000ull;
    pz->fileoff = 0; pz->filesize = 0;

    struct segment_command_64 *tx = (struct segment_command_64 *)((uint8_t *)pz + pz->cmdsize);
    tx->cmd = LC_SEGMENT_64;
    tx->cmdsize = sizeof *tx + sizeof(struct section_64);
    strcpy(tx->segname, "__TEXT");
    tx->vmaddr = 0x100000000ull;
    tx->vmsize = WT_TEXT_FILESIZE;
    tx->fileoff = 0;
    tx->filesize = WT_TEXT_FILESIZE;
    tx->nsects = 1;
    struct section_64 *sc = (struct section_64 *)((uint8_t *)tx + sizeof *tx);
    strncpy(sc->sectname, "__data", sizeof sc->sectname);
    strncpy(sc->segname, "__TEXT", sizeof sc->segname);
    sc->addr = 0x100000000ull + WT_SECT_OFF;
    sc->size = 4;
    sc->offset = WT_SECT_OFF;
    sc->flags = S_REGULAR;

    struct segment_command_64 *le =
        (struct segment_command_64 *)((uint8_t *)tx + tx->cmdsize);
    le->cmd = LC_SEGMENT_64;
    le->cmdsize = sizeof *le;
    strcpy(le->segname, "__LINKEDIT");
    le->vmaddr = tx->vmaddr + tx->vmsize;
    le->vmsize = 0x1000;
    le->fileoff = WT_TRIE_OFF;
    le->filesize = WT_TRIE_SIZE;   /* == exactly the (original) trie: it is
                                     * the only thing in __LINKEDIT here */

    struct dyld_info_command *di =
        (struct dyld_info_command *)((uint8_t *)le + le->cmdsize);
    di->cmd = LC_DYLD_INFO_ONLY;
    di->cmdsize = sizeof *di;
    di->export_off = WT_TRIE_OFF;
    di->export_size = WT_TRIE_SIZE;

    h->sizeofcmds = (uint32_t)(pz->cmdsize + tx->cmdsize + le->cmdsize + di->cmdsize);
    CHECK(sizeof(*h) + h->sizeofcmds == WT_LC_END,
          "fixture invariant: load commands end where WT_LC_END says (got %zu want %u)",
          sizeof(*h) + h->sizeofcmds, WT_LC_END);
    CHECK(WT_LC_END <= WT_SECT_OFF, "fixture invariant: load commands fit before the section");

    /* Same 17-byte hand-built trie as test_grow_rebases_export_trie's
     * MG_T_TRIE fixture, except node A's address is 16000 (0x3E80, ULEB
     * 80 7D) instead of 0x1000 -- see the block comment above this
     * function for why that one value forces the widen. */
    static const uint8_t trie[WT_TRIE_SIZE] = {
        0x00, 0x02,
        'A', 0x00, 8,
        'B', 0x00, 13,
        0x03, 0x00, 0x80, 0x7D, 0x00,     /* A: termsz3 flags0 addr16000(2B) nch0 */
        0x02, 0x00, 0x00, 0x00            /* B: termsz2 flags0 addr0       nch0 */
    };
    memcpy(buf + WT_TRIE_OFF, trie, sizeof trie);

    *fsize_out = WT_FSIZE;
    return buf;
}

static void test_grow_rebuilds_widening_export_trie(void) {
    size_t fsize;
    uint8_t *buf = build_widening_trie_image(&fsize);
    const uint32_t g = 0x1000;
    CHECK(fsize == WT_FSIZE, "fixture is exactly WT_FSIZE bytes (got %zu)", fsize);

    int r = mg_grow_header(&buf, &fsize, g);
    CHECK(r == 0, "grow succeeds on a WIDENING export trie -- no longer refuses (got %d)", r);
    if (r != 0) { free(buf); return; }

    /* Hand-computed rebuilt trie (see the task report / commit message for
     * the by-hand ULEB derivation): 18 bytes, one more than the original 17
     *   root (8B):   00 02 'A' 00 08 'B' 00 0E
     *   node A (6B): 04 00 80 9D 01 00     (addr 20096 = 0x4E80, ULEB 80 9D 01)
     *   node B (4B): 02 00 00 00
     */
    static const uint8_t expect[18] = {
        0x00, 0x02, 'A', 0x00, 0x08, 'B', 0x00, 0x0E,
        0x04, 0x00, 0x80, 0x9D, 0x01, 0x00,
        0x02, 0x00, 0x00, 0x00,
    };

    uint32_t new_export_off = 0, new_export_size = 0;
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    uint8_t *lcp = buf + sizeof *h;
    struct segment_command_64 *le2 = NULL;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *seg = (struct segment_command_64 *)lcp;
            if (strcmp(seg->segname, "__LINKEDIT") == 0) le2 = seg;
        } else if (lc->cmd == LC_DYLD_INFO_ONLY) {
            struct dyld_info_command *di = (struct dyld_info_command *)lcp;
            new_export_off = di->export_off; new_export_size = di->export_size;
        }
        lcp += lc->cmdsize;
    }

    CHECK(new_export_size == sizeof expect,
          "export_size grew to 18 (got %u) -- __LINKEDIT genuinely resized", new_export_size);
    CHECK(fsize == WT_FSIZE + g + sizeof expect,
          "file grew by header-pad(%u) + the trie's 1-byte growth: got %zu want %u",
          g, fsize, (unsigned)(WT_FSIZE + g + sizeof expect));
    CHECK(le2 != NULL, "__LINKEDIT segment still present");
    if (le2) {
        CHECK(le2->fileoff + le2->filesize == fsize,
              "__LINKEDIT still ends exactly at the (new) end of the file "
              "(fileoff=%llu filesize=%llu file=%zu)",
              (unsigned long long)le2->fileoff, (unsigned long long)le2->filesize, fsize);
        CHECK(new_export_off == le2->fileoff + le2->filesize - new_export_size,
              "export_off points at the rebuilt trie's actual location");
    }
    if (new_export_off && new_export_size == sizeof expect &&
        (uint64_t)new_export_off + new_export_size <= fsize) {
        CHECK(memcmp(buf + new_export_off, expect, sizeof expect) == 0,
              "rebuilt trie bytes match the hand-computed result exactly");
        uint64_t a = 0;
        int n = mu_decode(buf + new_export_off + 10, buf + new_export_off + new_export_size, &a);
        CHECK(n == 3 && a == 16000 + g, "node A address is 16000+grow=20096 in 3 bytes "
              "(got n=%d v=%#llx)", n, (unsigned long long)a);
    }
    free(buf);
}

/* ---- unknown means unsafe ----
 * The handlers above cover what we know. This is about what we do not: a load
 * command or section type nobody classified might carry offsets from the image
 * base exactly as __init_offsets and compact unwind do, and there is no way to
 * tell by looking at a number. Growing anyway is how LC_DATA_IN_CODE and
 * __unwind_info were silently corrupted for months. So the default is refusal,
 * and adding support for something means adding it to the table on purpose.
 */
static void test_grow_refuses_unknown_load_command(void) {
    check_refused_unchanged("an unclassified load command", MG_T_UNKNOWN_LC);
}

/* Known to carry base-relative ULEB payloads, and we do not re-base them.
 * Refusing is the honest answer, not silence. */
static void test_grow_refuses_linker_optimization_hint(void) {
    check_refused_unchanged("LC_LINKER_OPTIMIZATION_HINT", MG_T_LOH);
}

static void test_grow_refuses_unknown_section_type(void) {
    check_refused_unchanged("an unclassified section type", MG_T_ODDSECT);
}

/* ---- 32-bit stays refused, on purpose ----
 * mg_grow_header's image-base trick and every helper it calls (mg_first_sect_off,
 * mg_collect/mg_verify, mg_classify, mg_unwind_walk, mg_init_offsets_pass, and
 * the segment-patching loop inside mg_grow_header itself) walk LC_SEGMENT_64 and
 * struct section_64 -- roughly seven places that would each need a parallel
 * LC_SEGMENT/struct section path, in a file whose correctness already rests on
 * ULEB-precise, snapshot-verified arithmetic (see mg_verify/mg_plausible above).
 * That is a lot of new surface, in the riskiest possible place, for a format
 * this toolkit's own image.h already drew the same line against ("32-bit and
 * fat are known gaps, filed as Task 5") -- and every one of the seven rewriters
 * in this repo (fix_macho, patch_macho, ...) already refuses non-64-bit input
 * the same way, at the very first header check. So this stays a refusal: the
 * check at the top of mg_grow_header already catches it (magic != MH_MAGIC_64)
 * before anything is touched, this test just makes that refusal a pinned,
 * regression-tested fact rather than an accidental side effect of the 64-bit-
 * only design. See docs/prior-art.md for the write-up.
 *
 * Reviewed and found tautological in its first form: it built a minimal
 * ncmds=0 image with a 32-bit magic and checked mg_grow_header returned -1.
 * Under a mutated magic check (`!= MH_MAGIC_64` -> `!= MH_MAGIC_64 && !=
 * MH_MAGIC`) it still passed -- ncmds=0 means mg_first_sect_off finds no
 * sections and refuses on its OWN account, so the test was really pinning
 * "an image with no sections gets refused somewhere", not "32-bit magic
 * gets refused at the magic check". First fix attempt: build the fixture from
 * build_growable_image()'s output -- a genuinely complete, otherwise-valid
 * image that mg_grow_header actually succeeds on -- and change ONLY its
 * magic. That still doesn't discriminate THIS check on its own: mg_first_sect_off
 * calls mi_wrap (src/image.c), which independently re-validates the magic, so
 * mutating ONLY mg_grow_header's own check leaves mi_wrap's guard catching the
 * same fixture a few lines later, still returning -1 -- true defense in depth,
 * but it means the return code alone can't tell which check fired. So this
 * asserts on the SPECIFIC diagnostic mg_grow_header's own check prints
 * ("deliberately unsupported format"), not just the return code: mutate away
 * mg_grow_header's check and mi_wrap's still refuses (r stays -1) but with ITS
 * message ("fails validation... refusing to guess the header pad boundary"),
 * which does not contain that phrase -- so the message assertion below is what
 * flips to FAIL. Confirmed by hand: mutating macho_grow.h's check flips this
 * exact CHECK, though not `r == -1`. */
static int stderr_contains_during(int (*call)(uint8_t **, size_t *, uint32_t),
                                   uint8_t **pbuf, size_t *pfsize, uint32_t grow,
                                   const char *needle, int *ret_out) {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/macho_grow_test_stderr.%d", tmpdir, (int)getpid());

    fflush(stderr);
    int saved_fd = dup(fileno(stderr));
    if (!freopen(path, "w", stderr)) { *ret_out = call(pbuf, pfsize, grow); return 0; }

    *ret_out = call(pbuf, pfsize, grow);

    fflush(stderr);
    dup2(saved_fd, fileno(stderr));   /* restore the real stderr */
    close(saved_fd);
    clearerr(stderr);

    int found = 0;
    FILE *rf = fopen(path, "r");
    if (rf) {
        char line[1024];
        while (fgets(line, sizeof line, rf))
            if (strstr(line, needle)) { found = 1; break; }
        fclose(rf);
    }
    unlink(path);
    return found;
}

static void test_grow_refuses_32bit_mach_header(void) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_growable_image(&fsize, &sect_off);
    struct mach_header *h = (struct mach_header *)buf;   /* same offset as ->magic in _64 */
    h->magic = MH_MAGIC;

    uint8_t *before = (uint8_t *)malloc(fsize);
    memcpy(before, buf, fsize);

    size_t got_fsize = fsize;
    int r;
    int mentioned = stderr_contains_during(mg_grow_header, &buf, &got_fsize, 0x1000,
                                            "deliberately unsupported format", &r);
    CHECK(r == -1, "mg_grow_header refuses a 32-bit Mach-O (got %d)", r);
    CHECK(mentioned, "refusal is mg_grow_header's OWN 32-bit check, not a downstream "
                     "guard incidentally catching the same fixture");
    CHECK(got_fsize == fsize, "size unchanged on refusal (got %zu want %zu)", got_fsize, fsize);
    if (got_fsize == fsize)
        CHECK(memcmp(before, buf, fsize) == 0, "buffer byte-identical on refusal");
    free(before);
    free(buf);
}

/* ---- plausibility: verification without a "before" ----
 * The invariant check is strictly stronger, but it needs a snapshot taken before
 * the transform -- which the wrapper cannot have, because it verifies the end
 * state of a pipeline whose earlier stages ran in other processes.
 *
 * This is the check that works from the finished file alone: initializers and
 * compact-unwind entries name FUNCTIONS, so their targets must land exactly on
 * an address LC_FUNCTION_STARTS lists. Measured on Claude Code 2.1.263 that
 * holds perfectly -- 13/13 first-level, 198/198 LSDA, 9/9 initializers, against
 * 71,974 known starts -- while a mere range check would be near-useless there,
 * since __text is 63 MB and a one-page error stays inside it.
 */
static void test_plausible_accepts_a_well_formed_image(void) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_image(&fsize, &sect_off, MG_T_FUNCSTARTS);
    CHECK(mg_plausible(buf, fsize) == 0,
          "plausible ACCEPTS initializers that land on function starts");
    free(buf);
}

static void test_plausible_rejects_an_offset_that_names_no_function(void) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_image(&fsize, &sect_off, MG_T_FUNCSTARTS);
    uint32_t *e = (uint32_t *)(buf + sect_off);
    e[0] += 0x10;              /* still inside __text, but not a function start */
    CHECK(mg_plausible(buf, fsize) == -1,
          "plausible REJECTS an initializer pointing into the middle of a function");
    free(buf);
}

/* The failure this is really for: a structure left un-re-based by a grow. */
static void test_plausible_rejects_an_unrebased_initializer(void) {
    size_t fsize; uint32_t sect_off;
    uint8_t *buf = build_image(&fsize, &sect_off, MG_T_FUNCSTARTS);
    uint32_t *e = (uint32_t *)(buf + sect_off);
    e[0] -= 0x1000;            /* exactly what forgetting to re-base looks like */
    CHECK(mg_plausible(buf, fsize) == -1,
          "plausible REJECTS an initializer left a page low");
    free(buf);
}

int main(void) {
    test_uleb_decode();
    test_uleb_minlen();
    test_uleb_encode_fixed();
    test_reencode_same_width();
    test_reencode_widen_refuses();
    test_reencode_nonminimal_original_preserved();
    test_reencode_malformed();
    test_invariant_addresses_preserved();
    test_init_offsets_rebase();
    test_grow_applies_init_offsets_once();
    test_grow_rebases_data_in_code();
    test_grow_rebases_export_trie();
    test_grow_rebuilds_widening_export_trie();
    test_grow_refuses_unknown_load_command();
    test_grow_refuses_linker_optimization_hint();
    test_grow_refuses_unknown_section_type();
    test_grow_refuses_32bit_mach_header();
    test_plausible_accepts_a_well_formed_image();
    test_plausible_rejects_an_offset_that_names_no_function();
    test_plausible_rejects_an_unrebased_initializer();
    test_grow_rebases_unwind_info();
    test_verify_watches_unwind_info();
    test_verify_accepts_a_correct_grow();
    test_verify_rejects_double_apply();
    test_verify_rejects_handler_that_never_ran();
    if (fails) { printf("macho_grow_test: %d FAILURE(S)\n", fails); return 1; }
    printf("macho_grow_test: all cases pass\n");
    return 0;
}
