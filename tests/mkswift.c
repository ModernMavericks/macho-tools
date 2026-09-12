/* tests/mkswift.c -- a fixture builder for retag-swift, shared by
 * tests/cli_test.sh and tests/wrapper_test.sh, exactly the way
 * tests/strip_version_min.c is shared between them.
 *
 * mkswift make OUT   -- write a tiny 64-bit Mach-O with one __DATA segment
 *                       holding __objc_classlist -> one class record, whose
 *                       isa points at a metaclass record. Both records carry
 *                       the STABLE-ABI is-Swift tag (low bits == 2).
 * mkswift tags FILE   -- print "class <low2> <word>" then "meta <low2> <word>"
 *                       for the two records this layout puts at fixed offsets.
 *
 * Fixed layout (file offsets == vm offsets; the segment maps at vmaddr
 * VMBASE with fileoff 0, so file_off(va) == va - VMBASE):
 *   0x800  __objc_classlist: one 8-byte VA, pointing at the class record
 *   0x900  class record:     +0 isa -> metaclass VA, +32 data word
 *   0x940  metaclass record: +0 isa == 0,            +32 data word
 *
 * WHY THIS EXISTS AT ALL. Nothing on a 10.9 host can emit a real Swift
 * binary (10.9 predates Swift entirely), and a fixture whose bits nothing in
 * the repo chose would prove less, not more -- so this is a hand-built
 * Mach-O with a known layout, read back by the same program that wrote it
 * (the leaf-tool-crashes.sh pattern).
 *
 * WHY IT IS SHARED, NOT PER-SUITE. cli_test.sh's own `retagged 2 class
 * record(s)` assertions need a binary with Swift class records to retag;
 * wrapper_test.sh's retag_swift_classes assertions need the exact same thing
 * for the exact same reason strip_version_min.c is shared rather than
 * copied -- every assertion the wrapper suite used to run against fixture
 * `f` (zero Swift class records) proved nothing about whether an install
 * actually happened, because "total: 0" is what a wrapper that installs
 * NOTHING also prints.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mach-o/loader.h>

#define FSIZE      0x1000
#define VMBASE     0x100000000ULL
#define LISTOFF    0x800
#define CLASSOFF   0x900
#define METAOFF    0x940
#define DATAOFF    32
#define PAYLOAD    0x00000001000009c0ULL   /* plausible non-tag bits, preserved */

static void set_name16(char *field, const char *name) {
    size_t len = strlen(name);
    if (len > 16) len = 16;
    memset(field, 0, 16);
    memcpy(field, name, len);
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "read %s\n", path); exit(2); }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: mkswift make|tags FILE\n"); return 2; }

    if (strcmp(argv[1], "tags") == 0) {
        size_t n; uint8_t *b = slurp(argv[2], &n);
        if (n < FSIZE) { fprintf(stderr, "fixture truncated\n"); return 2; }
        uint64_t c = *(uint64_t *)(b + CLASSOFF + DATAOFF);
        uint64_t m = *(uint64_t *)(b + METAOFF + DATAOFF);
        printf("class %llu 0x%llx\n", (unsigned long long)(c & 3), (unsigned long long)c);
        printf("meta %llu 0x%llx\n", (unsigned long long)(m & 3), (unsigned long long)m);
        return 0;
    }
    if (strcmp(argv[1], "make") != 0) { fprintf(stderr, "unknown mode\n"); return 2; }

    uint8_t *buf = calloc(1, FSIZE);
    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    h->magic = MH_MAGIC_64;
    h->cputype = CPU_TYPE_X86_64;
    h->cpusubtype = 3;
    h->filetype = MH_EXECUTE;
    h->ncmds = 1;

    struct segment_command_64 *sg = (struct segment_command_64 *)(buf + sizeof *h);
    sg->cmd = LC_SEGMENT_64;
    sg->cmdsize = (uint32_t)(sizeof *sg + 2 * sizeof(struct section_64));
    set_name16(sg->segname, "__DATA");
    sg->vmaddr = VMBASE;
    sg->vmsize = FSIZE;
    sg->fileoff = 0;
    sg->filesize = FSIZE;
    sg->nsects = 2;
    h->sizeofcmds = sg->cmdsize;

    struct section_64 *sc = (struct section_64 *)(sg + 1);
    set_name16(sc[0].sectname, "__objc_classlist");
    set_name16(sc[0].segname, "__DATA");
    sc[0].addr = VMBASE + LISTOFF;
    sc[0].size = 8;
    sc[0].offset = LISTOFF;
    set_name16(sc[1].sectname, "__objc_data");
    set_name16(sc[1].segname, "__DATA");
    sc[1].addr = VMBASE + CLASSOFF;
    sc[1].size = 0x100;
    sc[1].offset = CLASSOFF;

    *(uint64_t *)(buf + LISTOFF) = VMBASE + CLASSOFF;
    *(uint64_t *)(buf + CLASSOFF) = VMBASE + METAOFF;   /* class->isa */
    *(uint64_t *)(buf + CLASSOFF + DATAOFF) = PAYLOAD | 2;
    *(uint64_t *)(buf + METAOFF) = 0;                   /* metaclass->isa */
    *(uint64_t *)(buf + METAOFF + DATAOFF) = PAYLOAD | 2;

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror("fopen"); return 1; }
    if (fwrite(buf, 1, FSIZE, f) != FSIZE) { perror("fwrite"); fclose(f); return 1; }
    fclose(f);
    return 0;
}
