/*
 * tests/linkedit_test.c — hermetic tests for src/linkedit.c's ml_bump_all,
 * the __LINKEDIT offset-bump table extracted out of macho_grow.h (Task 2b,
 * docs/superpowers/plans/2026-09-09-finish-the-convergence.md).
 *
 * Ground truth is a synthetic image built by hand (via mi_wrap, not a real
 * linker's output), so this is host-agnostic -- no fixture file, no
 * compiler-dependent load-command shapes. Every offset field this module is
 * documented to touch gets a value chosen to land on one of three sides of
 * `insert` (strictly below -- must stay put; exactly at -- must bump;
 * strictly above -- must bump) or exactly 0 (an absent stream -- must stay
 * 0, since 0 < insert always holds for any nonzero insert). Every field this
 * module must NOT touch (symbol/relocation counts and indices, datasize
 * fields, and a whole unrelated LC_UUID load command) gets a sentinel value
 * checked byte-for-byte unchanged afterward -- that is the "and that nothing
 * else changed" half of the brief.
 *
 * Build: clang -O2 -Wall -Isrc -o /tmp/linkedit_test tests/linkedit_test.c
 *   src/linkedit.c src/image.c && /tmp/linkedit_test
 */
#include "image.h"
#include "linkedit.h"
#include "mach_compat.h"

#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, msg, ...) do { if (!(cond)) { \
    printf("FAIL: " msg "\n", ##__VA_ARGS__); fails++; } } while (0)

#define IMG_SIZE 4096
#define INSERT   0x2000u
#define GROW     0x1000u

/* Fixed sentinel values for every field ml_bump_all must NEVER touch.
 * Deliberately ALL ABOVE `insert` (0x2000), not small distinguishable
 * numbers: ml_bump only ever fires when a field's value is >= insert, so a
 * sentinel of, say, 7 would stay 7 whether or not a mutant wrongly ran
 * ml_bump on it -- silently defeating the "untouched" assertion. Each of
 * these is instead a unique value past insert, so a mutant that bumps one
 * by mistake changes it by exactly `grow` and gets caught. (Verified: an
 * earlier version of this fixture used small values and a mutation that
 * bumped LC_DYSYMTAB's nlocrel alongside locreloff was NOT caught until
 * this fix.) */
#define SENT_NSYMS     0xe007u
#define SENT_STRSIZE   0xe063u
#define SENT_ILOCAL    0xe011u
#define SENT_NLOCAL    0xe022u
#define SENT_IEXTDEF   0xe033u
#define SENT_NEXTDEF   0xe044u
#define SENT_IUNDEF    0xe055u
#define SENT_NUNDEF    0xe066u
#define SENT_NTOC      0xe077u
#define SENT_NMODTAB   0xe088u
#define SENT_NEXTREF   0xe099u
#define SENT_NINDIRECT 0xe0aau
#define SENT_NEXTREL   0xe0bbu
#define SENT_NLOCREL   0xe0ccu
#define SENT_REBASESZ  0xe0ddu
#define SENT_BINDSZ    0xe0eeu
#define SENT_WBINDSZ   0xe0ffu
#define SENT_LBINDSZ   0xe100u
#define SENT_EXPORTSZ  0xe111u
#define SENT_FS_SIZE   0xe122u
#define SENT_DIC_SIZE  0xe133u
#define SENT_CS_SIZE   0xe144u

struct built {
    uint8_t *buf;
    struct mach_header_64 *hdr;
    struct symtab_command *symtab;
    struct dysymtab_command *dysymtab;
    struct dyld_info_command *dyld_info;
    struct linkedit_data_command *funcstarts;
    struct linkedit_data_command *dice;
    struct linkedit_data_command *codesig;
    struct uuid_command *uuid;
    uint8_t uuid_snapshot[sizeof(struct uuid_command)];
};

/* Appends one load command's worth of zeroed bytes and returns a pointer to
 * it; advances *lcp and bumps hdr->ncmds/sizeofcmds. `size` must already be
 * a multiple of 8 -- true of every command struct used below. */
static void *append_lc(struct mach_header_64 *hdr, uint8_t **lcp, uint32_t cmd, uint32_t size) {
    struct load_command *lc = (struct load_command *)*lcp;
    lc->cmd = cmd;
    lc->cmdsize = size;
    hdr->ncmds++;
    hdr->sizeofcmds += size;
    *lcp += size;
    return lc;
}

static struct built build_image(void) {
    struct built b;
    memset(&b, 0, sizeof b);
    b.buf = (uint8_t *)calloc(1, IMG_SIZE);
    b.hdr = (struct mach_header_64 *)b.buf;
    b.hdr->magic = MH_MAGIC_64;
    b.hdr->filetype = MH_EXECUTE;

    uint8_t *lcp = b.buf + sizeof(*b.hdr);

    b.symtab = (struct symtab_command *)append_lc(b.hdr, &lcp, LC_SYMTAB, sizeof(struct symtab_command));
    b.symtab->symoff  = 0x1000;   /* < insert: unchanged */
    b.symtab->nsyms   = SENT_NSYMS;
    b.symtab->stroff  = 0x3000;   /* > insert: bumps */
    b.symtab->strsize = SENT_STRSIZE;

    b.dysymtab = (struct dysymtab_command *)append_lc(b.hdr, &lcp, LC_DYSYMTAB, sizeof(struct dysymtab_command));
    b.dysymtab->ilocalsym      = SENT_ILOCAL;
    b.dysymtab->nlocalsym      = SENT_NLOCAL;
    b.dysymtab->iextdefsym     = SENT_IEXTDEF;
    b.dysymtab->nextdefsym     = SENT_NEXTDEF;
    b.dysymtab->iundefsym      = SENT_IUNDEF;
    b.dysymtab->nundefsym      = SENT_NUNDEF;
    b.dysymtab->tocoff         = INSERT;    /* == insert: bumps */
    b.dysymtab->ntoc           = SENT_NTOC;
    b.dysymtab->modtaboff      = 0x1500;    /* < insert: unchanged */
    b.dysymtab->nmodtab        = SENT_NMODTAB;
    b.dysymtab->extrefsymoff   = 0x5000;    /* > insert: bumps */
    b.dysymtab->nextrefsyms    = SENT_NEXTREF;
    b.dysymtab->indirectsymoff = 0x1800;    /* < insert: unchanged */
    b.dysymtab->nindirectsyms  = SENT_NINDIRECT;
    b.dysymtab->extreloff      = 0x9000;    /* > insert: bumps */
    b.dysymtab->nextrel        = SENT_NEXTREL;
    b.dysymtab->locreloff      = 0;         /* absent: stays 0 */
    b.dysymtab->nlocrel        = SENT_NLOCREL;

    b.dyld_info = (struct dyld_info_command *)append_lc(b.hdr, &lcp, LC_DYLD_INFO_ONLY, sizeof(struct dyld_info_command));
    b.dyld_info->rebase_off     = 0x1200;   /* < insert: unchanged */
    b.dyld_info->rebase_size    = SENT_REBASESZ;
    b.dyld_info->bind_off       = INSERT;   /* == insert: bumps */
    b.dyld_info->bind_size      = SENT_BINDSZ;
    b.dyld_info->weak_bind_off  = 0;        /* absent: stays 0 */
    b.dyld_info->weak_bind_size = SENT_WBINDSZ;
    b.dyld_info->lazy_bind_off  = 0x7000;   /* > insert: bumps */
    b.dyld_info->lazy_bind_size = SENT_LBINDSZ;
    b.dyld_info->export_off     = 0x1fff;   /* < insert (by 1): unchanged */
    b.dyld_info->export_size    = SENT_EXPORTSZ;

    b.funcstarts = (struct linkedit_data_command *)append_lc(b.hdr, &lcp, LC_FUNCTION_STARTS, sizeof(struct linkedit_data_command));
    b.funcstarts->dataoff  = 0x6000;        /* > insert: bumps */
    b.funcstarts->datasize = SENT_FS_SIZE;

    b.dice = (struct linkedit_data_command *)append_lc(b.hdr, &lcp, LC_DATA_IN_CODE, sizeof(struct linkedit_data_command));
    b.dice->dataoff  = 0x1000;              /* < insert: unchanged */
    b.dice->datasize = SENT_DIC_SIZE;

    b.codesig = (struct linkedit_data_command *)append_lc(b.hdr, &lcp, LC_CODE_SIGNATURE, sizeof(struct linkedit_data_command));
    b.codesig->dataoff  = INSERT;           /* == insert: bumps */
    b.codesig->datasize = SENT_CS_SIZE;

    /* A whole load command this module has no business touching -- no file
     * offset field at all. If ml_bump_lc's default case were ever widened
     * (the thing this project's "-grow refuses rather than guesses" rule
     * exists to prevent generally), this is what would catch it. */
    b.uuid = (struct uuid_command *)append_lc(b.hdr, &lcp, LC_UUID, sizeof(struct uuid_command));
    for (int i = 0; i < 16; i++) b.uuid->uuid[i] = (uint8_t)(0xA0 + i);
    memcpy(b.uuid_snapshot, b.uuid, sizeof b.uuid_snapshot);

    return b;
}

static void test_bump_all_moves_exactly_the_right_fields(void) {
    struct built b = build_image();

    mi_image im;
    int wr = mi_wrap(b.buf, IMG_SIZE, &im);
    CHECK(wr == 0, "synthetic image validates via mi_wrap (got %d)", wr);
    if (wr != 0) { free(b.buf); return; }

    ml_bump_all(&im, INSERT, GROW);

    /* ---- LC_SYMTAB ---- */
    CHECK(b.symtab->symoff == 0x1000, "symoff below insert unchanged (got %#x)", b.symtab->symoff);
    CHECK(b.symtab->stroff == 0x3000 + GROW, "stroff above insert bumped (got %#x)", b.symtab->stroff);
    CHECK(b.symtab->nsyms == SENT_NSYMS, "nsyms untouched");
    CHECK(b.symtab->strsize == SENT_STRSIZE, "strsize untouched");

    /* ---- LC_DYSYMTAB: the six file offsets ---- */
    CHECK(b.dysymtab->tocoff == INSERT + GROW, "tocoff at insert bumped (got %#x)", b.dysymtab->tocoff);
    CHECK(b.dysymtab->modtaboff == 0x1500, "modtaboff below insert unchanged (got %#x)", b.dysymtab->modtaboff);
    CHECK(b.dysymtab->extrefsymoff == 0x5000 + GROW, "extrefsymoff bumped (got %#x)", b.dysymtab->extrefsymoff);
    CHECK(b.dysymtab->indirectsymoff == 0x1800, "indirectsymoff below insert unchanged (got %#x)", b.dysymtab->indirectsymoff);
    CHECK(b.dysymtab->extreloff == 0x9000 + GROW, "extreloff bumped (got %#x)", b.dysymtab->extreloff);
    CHECK(b.dysymtab->locreloff == 0, "locreloff absent stays 0 (got %#x)", b.dysymtab->locreloff);
    /* ---- LC_DYSYMTAB: every index/count field, untouched ---- */
    CHECK(b.dysymtab->ilocalsym == SENT_ILOCAL, "ilocalsym untouched");
    CHECK(b.dysymtab->nlocalsym == SENT_NLOCAL, "nlocalsym untouched");
    CHECK(b.dysymtab->iextdefsym == SENT_IEXTDEF, "iextdefsym untouched");
    CHECK(b.dysymtab->nextdefsym == SENT_NEXTDEF, "nextdefsym untouched");
    CHECK(b.dysymtab->iundefsym == SENT_IUNDEF, "iundefsym untouched");
    CHECK(b.dysymtab->nundefsym == SENT_NUNDEF, "nundefsym untouched");
    CHECK(b.dysymtab->ntoc == SENT_NTOC, "ntoc untouched");
    CHECK(b.dysymtab->nmodtab == SENT_NMODTAB, "nmodtab untouched");
    CHECK(b.dysymtab->nextrefsyms == SENT_NEXTREF, "nextrefsyms untouched");
    CHECK(b.dysymtab->nindirectsyms == SENT_NINDIRECT, "nindirectsyms untouched");
    CHECK(b.dysymtab->nextrel == SENT_NEXTREL, "nextrel untouched");
    CHECK(b.dysymtab->nlocrel == SENT_NLOCREL, "nlocrel untouched");

    /* ---- LC_DYLD_INFO_ONLY ---- */
    CHECK(b.dyld_info->rebase_off == 0x1200, "rebase_off below insert unchanged (got %#x)", b.dyld_info->rebase_off);
    CHECK(b.dyld_info->bind_off == INSERT + GROW, "bind_off at insert bumped (got %#x)", b.dyld_info->bind_off);
    CHECK(b.dyld_info->weak_bind_off == 0, "weak_bind_off absent stays 0 (got %#x)", b.dyld_info->weak_bind_off);
    CHECK(b.dyld_info->lazy_bind_off == 0x7000 + GROW, "lazy_bind_off bumped (got %#x)", b.dyld_info->lazy_bind_off);
    CHECK(b.dyld_info->export_off == 0x1fff, "export_off just below insert unchanged (got %#x)", b.dyld_info->export_off);
    CHECK(b.dyld_info->rebase_size == SENT_REBASESZ, "rebase_size untouched");
    CHECK(b.dyld_info->bind_size == SENT_BINDSZ, "bind_size untouched");
    CHECK(b.dyld_info->weak_bind_size == SENT_WBINDSZ, "weak_bind_size untouched");
    CHECK(b.dyld_info->lazy_bind_size == SENT_LBINDSZ, "lazy_bind_size untouched");
    CHECK(b.dyld_info->export_size == SENT_EXPORTSZ, "export_size untouched");

    /* ---- LC_FUNCTION_STARTS / LC_DATA_IN_CODE / LC_CODE_SIGNATURE ---- */
    CHECK(b.funcstarts->dataoff == 0x6000 + GROW, "function-starts dataoff bumped (got %#x)", b.funcstarts->dataoff);
    CHECK(b.funcstarts->datasize == SENT_FS_SIZE, "function-starts datasize untouched");
    CHECK(b.dice->dataoff == 0x1000, "data-in-code dataoff below insert unchanged (got %#x)", b.dice->dataoff);
    CHECK(b.dice->datasize == SENT_DIC_SIZE, "data-in-code datasize untouched");
    CHECK(b.codesig->dataoff == INSERT + GROW, "code-signature dataoff at insert bumped (got %#x)", b.codesig->dataoff);
    CHECK(b.codesig->datasize == SENT_CS_SIZE, "code-signature datasize untouched");

    /* ---- an unrelated load command: byte-for-byte untouched ---- */
    CHECK(memcmp(b.uuid, b.uuid_snapshot, sizeof b.uuid_snapshot) == 0,
          "LC_UUID (no offset field) left completely untouched");

    /* ---- and the header itself: ncmds/sizeofcmds untouched (mi_each_lc's
     * own contract; belt-and-suspenders since ml_bump_lc never assigns to
     * them) ---- */
    CHECK(b.hdr->ncmds == 7, "ncmds untouched (got %u)", b.hdr->ncmds);

    free(b.buf);
}

/* insert==0 is the boundary case for ml_bump's `>=`: 0 >= 0 is true, so
 * EVERY field -- including a true 0 ("this stream is absent") -- qualifies
 * and bumps by `grow`. Real callers never pass insert=0 (it is always a
 * file offset past the header), but ml_bump's own comparison is unsigned
 * and unconditional, so this pins what it actually does at that edge
 * rather than an assumption about how it is called. */
static void test_bump_all_insert_zero_bumps_every_qualifying_field(void) {
    struct built b = build_image();
    mi_image im;
    CHECK(mi_wrap(b.buf, IMG_SIZE, &im) == 0, "wrap ok");

    ml_bump_all(&im, 0, GROW);

    CHECK(b.symtab->symoff == 0x1000 + GROW, "symoff bumps when insert=0 (got %#x)", b.symtab->symoff);
    CHECK(b.dysymtab->locreloff == GROW, "even a true-absent (0) stream bumps at insert=0, "
          "since 0 >= 0 (got %#x)", b.dysymtab->locreloff);
    free(b.buf);
}

/* grow==0 is a no-op: every qualifying field's value is unchanged even
 * though the >= comparison still fires. */
static void test_bump_all_zero_grow_is_a_no_op(void) {
    struct built b = build_image();
    mi_image im;
    CHECK(mi_wrap(b.buf, IMG_SIZE, &im) == 0, "wrap ok");

    uint32_t saved_stroff = b.symtab->stroff;
    ml_bump_all(&im, INSERT, 0);
    CHECK(b.symtab->stroff == saved_stroff, "grow=0 changes nothing (got %#x want %#x)",
          b.symtab->stroff, saved_stroff);
    free(b.buf);
}

int main(void) {
    test_bump_all_moves_exactly_the_right_fields();
    test_bump_all_insert_zero_bumps_every_qualifying_field();
    test_bump_all_zero_grow_is_a_no_op();

    if (fails) {
        printf("%d FAILURE(S)\n", fails);
        return 1;
    }
    printf("all linkedit_test checks passed\n");
    return 0;
}
