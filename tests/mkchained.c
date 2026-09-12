/* tests/mkchained.c -- a chained-fixups fixture builder, shared by
 * tests/cli_test.sh and tests/wrapper_test.sh, exactly the way
 * tests/strip_version_min.c and tests/mkswift.c are shared between them. It
 * was a here-document inside cli_test.sh until wrapper_test.sh needed the same
 * fixture for the same reason: `patch_macho`'s CONVERTING path -- its stdout
 * contract, and the install that actually replaces bytes -- can only be reached
 * with an input that really carries chained fixups, and three mutations of that
 * path (installing the unconverted bytes, deleting the wrapper's `Wrote OUT (N
 * bytes)` line, skipping the install when IN == OUT) went unnoticed by every
 * suite while this file could only be built from one of them.
 *
 * mkchained make|make-weak|make-big|make-nosect|make-sectpast OUT
 *                        -- write a tiny 64-bit Mach-O that uses CHAINED
 *                          FIXUPS, the format `declassify`/patch_macho exists
 *                          to lower. No linker on any host this repo supports
 *                          can be asked to emit one on demand (10.9's predates
 *                          the format by a decade), so the bytes are laid out
 *                          by hand -- the same idiom tests/leaf-tool-crashes.sh
 *                          and tests/mkswift.c already use.
 *
 * mkchained check FILE   -- print, one per line, what the conversion is
 *                          supposed to have DONE to it. Structure read
 *                          directly out of the file; never otool text.
 *
 * The image: three segments (__TEXT, __DATA, __LINKEDIT), one section in each
 * of the first two, an LC_DYLD_CHAINED_FIXUPS pointing at a hand-built fixups
 * blob in __LINKEDIT, an LC_DYLD_EXPORTS_TRIE, and an LC_BUILD_VERSION -- the
 * three commands the conversion strips. __DATA holds a two-link chain: slot 0
 * is a REBASE of a base-relative target (0x1000), slot 1 (8 bytes later) is a
 * BIND of import 0, "_mkchained_sym", from library ordinal 1.
 *
 * After conversion, then, slot 0 must hold image_base + 0x1000 and slot 1 must
 * hold 0 (dyld fills a bind slot in at load time). Those two quadwords are the
 * whole point: they are the arithmetic the conversion does that nothing else
 * in this repo does, and they are observable in the output file's bytes.
 *
 * make-weak differs in ONE field: the import's library ordinal is -3
 * (BIND_SPECIAL_DYLIB_WEAK_LOOKUP), which 10.9's dyld rejects outright with
 * "bad special ordinal". The conversion has to remap it to flat lookup (-2)
 * plus the weak-import flag, and that remap is visible in the emitted opcodes.
 *
 * make-big differs in SIZE: a 2MB __DATA whose every quadword is one link of a
 * single rebase chain, ~262k fixups. At about 5 opcode bytes each that is well
 * past the 1MB the conversion buffers, so it must REFUSE. Before the bound
 * existed this fixture walked straight off the end of a 1MB malloc.
 *
 * make-nosect differs in its two sections' file offsets: both are 0, so no
 * section has file data and nothing bounds the header pad the new
 * LC_DYLD_INFO_ONLY goes into. make-sectpast puts __text's offset past the end
 * of the file (and __data's at 0), so the pad's bound lies outside the image.
 * The conversion must refuse both rather than guess where the pad ends.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
#include "mach_compat.h"

#define TEXT_VMADDR  0x100000000ULL
#define SECT_OFF     0x400          /* first section's file offset: the bound
                                     * the conversion checks for the 48 bytes
                                     * LC_DYLD_INFO_ONLY needs */
#define DATA_OFF     0x1000
#define DATA_SIZE    0x1000
#define BIG_DATA_SIZE 0x200000
#define TRIE_SIZE    0x10
#define FIXUPS_SIZE  0x100

#define CF_PTR_64_OFFSET 6
#define REBASE_TARGET    0x1000ULL  /* base-relative, so the converted slot
                                     * must read TEXT_VMADDR + this */
#define BIND_SLOT_OFF    8
#define SYMNAME          "_mkchained_sym"

enum { MK_PLAIN, MK_WEAK, MK_BIG, MK_NOSECT, MK_SECTPAST };

/* segname/sectname are char[16] and need NOT be NUL-terminated; see
 * tests/README.md's host-portability section for why strcpy is wrong here. */
static void set_name16(char *field, const char *name) {
    size_t len = strlen(name);
    if (len > 16) len = 16;
    memset(field, 0, 16);
    memcpy(field, name, len);
}

static struct segment_command_64 *put_seg(uint8_t *p, const char *name,
                                          uint64_t vmaddr, uint64_t vmsize,
                                          uint64_t fileoff, uint64_t filesize,
                                          uint32_t nsects) {
    struct segment_command_64 *s = (struct segment_command_64 *)p;
    s->cmd = LC_SEGMENT_64;
    s->cmdsize = (uint32_t)(sizeof *s + nsects * sizeof(struct section_64));
    set_name16(s->segname, name);
    s->vmaddr = vmaddr; s->vmsize = vmsize;
    s->fileoff = fileoff; s->filesize = filesize;
    s->maxprot = 7; s->initprot = 3;
    s->nsects = nsects; s->flags = 0;
    return s;
}

static void put_sect(struct segment_command_64 *seg, int i, const char *sect,
                     const char *segname, uint64_t addr, uint64_t size,
                     uint32_t offset) {
    struct section_64 *s = (struct section_64 *)(seg + 1) + i;
    memset(s, 0, sizeof *s);
    set_name16(s->sectname, sect);
    set_name16(s->segname, segname);
    s->addr = addr; s->size = size; s->offset = offset;
}

static int make(const char *path, int mode) {
    uint64_t data_size = (mode == MK_BIG) ? BIG_DATA_SIZE : DATA_SIZE;
    uint64_t linkedit_off = DATA_OFF + data_size;
    uint64_t fixups_off = linkedit_off;
    uint64_t trie_off = linkedit_off + FIXUPS_SIZE;
    size_t fsize = (size_t)(linkedit_off + 0x1000);

    uint8_t *buf = calloc(1, fsize);
    if (!buf) return 2;

    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    h->magic = MH_MAGIC_64;
    h->cputype = CPU_TYPE_X86_64;
    h->cpusubtype = CPU_SUBTYPE_X86_64_ALL;
    h->filetype = MH_DYLIB;
    h->flags = MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL;

    uint8_t *p = buf + sizeof *h;

    struct segment_command_64 *text = put_seg(p, "__TEXT", TEXT_VMADDR, 0x1000, 0, 0x1000, 1);
    uint32_t text_off = (mode == MK_NOSECT) ? 0
                      : (mode == MK_SECTPAST) ? (uint32_t)fsize + 0x1000 : SECT_OFF;
    put_sect(text, 0, "__text", "__TEXT", TEXT_VMADDR + SECT_OFF, 4, text_off);
    p += text->cmdsize;

    struct segment_command_64 *data = put_seg(p, "__DATA", TEXT_VMADDR + DATA_OFF, data_size,
                                              DATA_OFF, data_size, 1);
    put_sect(data, 0, "__data", "__DATA", TEXT_VMADDR + DATA_OFF, data_size,
             (mode == MK_NOSECT || mode == MK_SECTPAST) ? 0 : DATA_OFF);
    p += data->cmdsize;

    struct segment_command_64 *le = put_seg(p, "__LINKEDIT", TEXT_VMADDR + linkedit_off, 0x1000,
                                            linkedit_off, 0x1000, 0);
    p += le->cmdsize;

    struct linkedit_data_command *cf = (struct linkedit_data_command *)p;
    cf->cmd = LC_DYLD_CHAINED_FIXUPS; cf->cmdsize = sizeof *cf;
    cf->dataoff = (uint32_t)fixups_off; cf->datasize = FIXUPS_SIZE;
    p += cf->cmdsize;

    struct linkedit_data_command *tr = (struct linkedit_data_command *)p;
    tr->cmd = LC_DYLD_EXPORTS_TRIE; tr->cmdsize = sizeof *tr;
    tr->dataoff = (uint32_t)trie_off; tr->datasize = TRIE_SIZE;
    p += tr->cmdsize;

    /* LC_BUILD_VERSION by hand: 10.9's <mach-o/loader.h> has no
     * build_version_command struct, only the command number mach_compat.h
     * supplies. cmd, cmdsize, platform, minos, sdk, ntools. */
    uint32_t *bv = (uint32_t *)p;
    bv[0] = LC_BUILD_VERSION; bv[1] = 24; bv[2] = 1;
    bv[3] = 0x000C0000; bv[4] = 0x000C0000; bv[5] = 0;
    p += 24;

    h->ncmds = 6;
    h->sizeofcmds = (uint32_t)(p - (buf + sizeof *h));

    /* The chain in __DATA. Every link uses pointer format 6
     * (DYLD_CHAINED_PTR_64_OFFSET): bit 63 selects bind over rebase, bits
     * [62:51] are the distance to the next link in 4-byte strides, and the low
     * bits are a base-relative target (rebase) or an import ordinal (bind). */
    uint64_t *slot = (uint64_t *)(buf + DATA_OFF);
    if (mode == MK_BIG) {
        uint64_t n = data_size / 8;
        for (uint64_t i = 0; i < n; i++)
            slot[i] = REBASE_TARGET | ((i + 1 < n) ? ((uint64_t)2 << 51) : 0);
    } else {
        slot[0] = REBASE_TARGET | ((uint64_t)(BIND_SLOT_OFF / 4) << 51);
        slot[1] = (1ULL << 63) | 0ULL;   /* bind import 0, next = 0 = end of chain */
    }

    /* The fixups blob: header, starts-image, one starts-segment for __DATA
     * (segment index 1), one import, one symbol name. */
    uint8_t *fx = buf + fixups_off;
    uint32_t *fh = (uint32_t *)fx;
    fh[0] = 0;      /* fixups_version */
    fh[1] = 0x20;   /* starts_offset */
    fh[2] = 0x60;   /* imports_offset */
    fh[3] = 0x80;   /* symbols_offset */
    fh[4] = 1;      /* imports_count */
    fh[5] = 1;      /* imports_format: DYLD_CHAINED_IMPORT */
    fh[6] = 0;      /* symbols_format: uncompressed */

    uint32_t *starts = (uint32_t *)(fx + 0x20);
    starts[0] = 3;      /* seg_count: __TEXT, __DATA, __LINKEDIT */
    starts[1] = 0;      /* __TEXT: no fixups */
    starts[2] = 0x10;   /* __DATA: its starts-segment, relative to starts */
    starts[3] = 0;      /* __LINKEDIT: no fixups */

    uint8_t *ss = fx + 0x30;
    *(uint32_t *)(ss + 0)  = 24;                /* size */
    *(uint16_t *)(ss + 4)  = 0x1000;            /* page_size */
    *(uint16_t *)(ss + 6)  = CF_PTR_64_OFFSET;  /* pointer_format */
    *(uint64_t *)(ss + 8)  = DATA_OFF;          /* segment_offset */
    *(uint32_t *)(ss + 16) = 0;                 /* max_valid_pointer */
    *(uint16_t *)(ss + 20) = 1;                 /* page_count: one page START,
                                                 * whose chain may run on past
                                                 * that page -- which is what
                                                 * make-big's does */
    *(uint16_t *)(ss + 22) = 0;                 /* page_start[0]: chain at +0 */

    /* import 0. lib_ordinal 1 normally; 0xFD reads back as the signed -3 that
     * means BIND_SPECIAL_DYLIB_WEAK_LOOKUP, the ordinal 10.9's dyld refuses. */
    *(uint32_t *)(fx + 0x60) = (mode == MK_WEAK) ? 0xFDu : 1u;
    memcpy(fx + 0x80, SYMNAME, sizeof SYMNAME);

    memset(buf + trie_off, 0, TRIE_SIZE);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) { perror("create"); free(buf); return 2; }
    if (write(fd, buf, fsize) != (ssize_t)fsize) { perror("write"); close(fd); free(buf); return 2; }
    close(fd);
    free(buf);
    return 0;
}

static int has_cmd(uint8_t *buf, uint32_t want) {
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    uint8_t *p = buf + sizeof *h;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        struct load_command *lc = (struct load_command *)p;
        if (lc->cmd == want) return 1;
        p += lc->cmdsize;
    }
    return 0;
}

static int check(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 2; }
    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf || read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) {
        fprintf(stderr, "read failed\n"); close(fd); return 2;
    }
    close(fd);
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    if (h->magic != MH_MAGIC_64) { fprintf(stderr, "not a 64-bit Mach-O\n"); free(buf); return 2; }

    uint64_t *slot = (uint64_t *)(buf + DATA_OFF);
    printf("size=%llu\n", (unsigned long long)st.st_size);
    printf("slot0=0x%llx\n", (unsigned long long)slot[0]);
    printf("slot1=0x%llx\n", (unsigned long long)slot[1]);
    printf("chained=%d\n", has_cmd(buf, LC_DYLD_CHAINED_FIXUPS));
    printf("trie=%d\n", has_cmd(buf, LC_DYLD_EXPORTS_TRIE));
    printf("buildver=%d\n", has_cmd(buf, LC_BUILD_VERSION));
    printf("dyldinfo=%d\n", has_cmd(buf, LC_DYLD_INFO_ONLY));

    /* The LC_DYLD_INFO_ONLY the conversion added, and the __LINKEDIT it had to
     * extend to cover what that command points at. */
    uint8_t *p = buf + sizeof *h;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        struct load_command *lc = (struct load_command *)p;
        if (lc->cmd == LC_DYLD_INFO_ONLY) {
            struct dyld_info_command *di = (struct dyld_info_command *)lc;
            printf("rebase=%u+%u\n", di->rebase_off, di->rebase_size);
            printf("bind=%u+%u\n", di->bind_off, di->bind_size);
            printf("export=%u+%u\n", di->export_off, di->export_size);
            /* The bind stream's first three opcodes are SET_TYPE_IMM, the
             * dylib-ordinal opcode and SET_SYMBOL_TRAILING_FLAGS_IMM, each one
             * byte, and the symbol name follows the third as a NUL-terminated
             * string. Reading them back is how this proves the BIND link was
             * translated and not merely counted, and it is what makes the -3
             * weak remap observable: the second byte says which dylib opcode
             * was chosen and the third carries the weak-import flag. Reading
             * at a FIXED offset is deliberate: if the emitted opcode sequence
             * ever changes shape, that is a behaviour change in the conversion
             * and this must fail rather than adapt. */
            if (di->bind_size > 4 && di->bind_off + di->bind_size <= (uint32_t)st.st_size) {
                uint8_t *b = buf + di->bind_off;
                printf("bindops=%02x,%02x,%02x\n", b[0], b[1], b[2]);
                printf("bindsym=%.*s\n", (int)(di->bind_size - 3), (char *)b + 3);
            }
        }
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *s = (struct segment_command_64 *)lc;
            if (strncmp(s->segname, "__LINKEDIT", 16) == 0)
                printf("linkedit=%llu+%llu\n", (unsigned long long)s->fileoff,
                       (unsigned long long)s->filesize);
        }
        p += lc->cmdsize;
    }
    free(buf);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: mkchained make|make-weak|make-big|make-nosect|make-sectpast|check FILE\n"); return 2; }
    if (strcmp(argv[1], "make") == 0) return make(argv[2], MK_PLAIN);
    if (strcmp(argv[1], "make-weak") == 0) return make(argv[2], MK_WEAK);
    if (strcmp(argv[1], "make-big") == 0) return make(argv[2], MK_BIG);
    if (strcmp(argv[1], "make-nosect") == 0) return make(argv[2], MK_NOSECT);
    if (strcmp(argv[1], "make-sectpast") == 0) return make(argv[2], MK_SECTPAST);
    if (strcmp(argv[1], "check") == 0) return check(argv[2]);
    fprintf(stderr, "usage: mkchained make|make-weak|make-big|make-nosect|make-sectpast|check FILE\n");
    return 2;
}
