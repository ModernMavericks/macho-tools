/* mkimplausible OUT -- write a thin 64-bit Mach-O that mg_plausible REFUSES,
 * and that is otherwise perfectly well formed.
 *
 * WHY IT EXISTS. src/rewrite.c's mr_process_thin skips mg_plausible when the
 * operation set is a rename only, because that gate asks an OFFSET question
 * and a segment rename moves no offset (the reasoning is at the site). Two
 * suites assert that: tests/cli_test.sh at the `macho9 segment` level and
 * tests/wrapper_test.sh through the `rename_segment` wrapper. Both need an
 * input the gate rejects.
 *
 * They used to find one by scanning /usr/lib for a dylib mg_plausible
 * refused -- all 26 thin 64-bit dylibs there did on 10.9 -- and SKIP if none
 * turned up. That count is NOT evidence the LC_FUNCTION_STARTS heuristic gets
 * real dylibs wrong, and this comment used to say it was. The heuristic never
 * ran on any of them: mg_plausible took its image base from mi_text_base,
 * whose 0 return means BOTH "no segment maps the header" and "the base is 0",
 * and a dylib is linked at base 0 -- so it bailed at the precondition. Since
 * mi_image_base separated those two answers (src/image.h) all 26 pass, and
 * the scan this fixture replaced would now find nothing to use on ANY host.
 * That is a second, independent reason to build the input rather than look
 * for one -- and the original reason still stands on its own: a scan
 * passes on the target and silently covers NOTHING on the cross/CI runner,
 * where those dylibs live only in the dyld shared cache: the one behavioural
 * change this repo made to macho9 would have shipped with no coverage at all
 * anywhere it is actually built. A committed, hand-built fixture asks the same
 * question on every host, which is this suite's rule for exactly this reason
 * (tests/README.md, "A fixture built without -mmacosx-version-min=10.9 asks a
 * different question on a modern host").
 *
 * HOW IT TRIPS THE GATE. mg_plausible (src/grow.c) collects every
 * base-relative entry that is supposed to name a FUNCTION and requires each to
 * be one of the addresses LC_FUNCTION_STARTS decodes to. This image has:
 *
 *   LC_FUNCTION_STARTS   one entry, ULEB delta 0x400 -- so the only known
 *                        function address is base + 0x400
 *   __DATA,__init_offsets  section type S_INIT_FUNC_OFFSETS, one uint32 entry
 *                        holding 0x999 -- an initializer, which mg_collect
 *                        tags MG_K_FUNC, resolving to base + 0x999
 *
 * base + 0x999 is not base + 0x400, so mg_plausible refuses. Everything else
 * about the image is deliberately ordinary: it passes mi_wrap's load-command
 * validation and mg_classify's per-command and per-section-type checks, it has
 * header pad to spare, and it carries
 *
 *   an LC_UUID    so that `macho9 lc -delete uuid` has something to strip and
 *                 therefore reaches the gate at all (mr_apply_file returns
 *                 early, before the gate, when nothing changed), and
 *   a __DATA segment with two sections
 *                 so that `macho9 segment __DATA __X` has something to rename
 *                 AND has section segname copies to rename with it.
 *
 * It carries NO dylib load command, so it cannot be refused earlier by
 * mo_map_build (see compat/rename_segment.sh's divergence 5 for what that
 * refuses and why), and no LC_LAZY_LOAD_DYLIB.
 *
 * segname/sectname are char[16] and need NOT be NUL-terminated, so every name
 * is written with memcpy through set16 and never strcpy -- tests/README.md's
 * ninth lesson, which aborts a modern-clang build at run time and warns at
 * compile time on every host.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <mach-o/loader.h>

#include "mach_compat.h"

#define TEXT_VMADDR 0x100000000ULL
#define SECT_OFF    0x400        /* the one address LC_FUNCTION_STARTS names */
#define DATA_OFF    0x1000
#define DATA_SIZE   0x1000
#define LE_OFF      0x2000
#define LE_SIZE     0x1000
#define INIT_OFF    (DATA_OFF + 0x800)
#define FS_SIZE     8
#define BAD_INIT    0x999u       /* deliberately NOT SECT_OFF */

static void set16(char *field, const char *name) {
    size_t len = strlen(name);
    if (len > 16) len = 16;
    memset(field, 0, 16);
    memcpy(field, name, len);
}

static struct segment_command_64 *put_seg(uint8_t *p, const char *name, uint64_t vmaddr,
                                          uint64_t vmsize, uint64_t fileoff,
                                          uint64_t filesize, uint32_t nsects) {
    struct segment_command_64 *s = (struct segment_command_64 *)p;
    s->cmd = LC_SEGMENT_64;
    s->cmdsize = (uint32_t)(sizeof *s + nsects * sizeof(struct section_64));
    set16(s->segname, name);
    s->vmaddr = vmaddr; s->vmsize = vmsize;
    s->fileoff = fileoff; s->filesize = filesize;
    s->maxprot = 7; s->initprot = 3;
    s->nsects = nsects; s->flags = 0;
    return s;
}

static void put_sect(struct segment_command_64 *seg, int i, const char *sect,
                     const char *segname, uint64_t addr, uint64_t size,
                     uint32_t offset, uint32_t flags) {
    struct section_64 *s = (struct section_64 *)(seg + 1) + i;
    memset(s, 0, sizeof *s);
    set16(s->sectname, sect);
    set16(s->segname, segname);
    s->addr = addr; s->size = size; s->offset = offset; s->flags = flags;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s OUT\n", argv[0]); return 2; }

    size_t fsize = LE_OFF + LE_SIZE;
    uint8_t *buf = (uint8_t *)calloc(1, fsize);
    if (!buf) { fprintf(stderr, "out of memory\n"); return 2; }

    struct mach_header_64 *h = (struct mach_header_64 *)buf;
    h->magic = MH_MAGIC_64;
    h->cputype = CPU_TYPE_X86_64;
    h->cpusubtype = CPU_SUBTYPE_X86_64_ALL;
    h->filetype = MH_DYLIB;
    h->flags = MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL;

    uint8_t *p = buf + sizeof *h;

    struct segment_command_64 *text = put_seg(p, "__TEXT", TEXT_VMADDR, 0x1000, 0, 0x1000, 1);
    put_sect(text, 0, "__text", "__TEXT", TEXT_VMADDR + SECT_OFF, 4, SECT_OFF, 0);
    p += text->cmdsize;

    struct segment_command_64 *data = put_seg(p, "__DATA", TEXT_VMADDR + DATA_OFF, DATA_SIZE,
                                              DATA_OFF, DATA_SIZE, 2);
    put_sect(data, 0, "__data", "__DATA", TEXT_VMADDR + DATA_OFF, 8, DATA_OFF, 0);
    put_sect(data, 1, "__init_offsets", "__DATA", TEXT_VMADDR + INIT_OFF, 4,
             INIT_OFF, S_INIT_FUNC_OFFSETS);
    p += data->cmdsize;

    struct segment_command_64 *le = put_seg(p, "__LINKEDIT", TEXT_VMADDR + LE_OFF, LE_SIZE,
                                            LE_OFF, LE_SIZE, 0);
    p += le->cmdsize;

    struct uuid_command *uu = (struct uuid_command *)p;
    uu->cmd = LC_UUID; uu->cmdsize = sizeof *uu;
    memset(uu->uuid, 0xab, sizeof uu->uuid);
    p += uu->cmdsize;

    struct linkedit_data_command *fs = (struct linkedit_data_command *)p;
    fs->cmd = LC_FUNCTION_STARTS; fs->cmdsize = sizeof *fs;
    fs->dataoff = LE_OFF; fs->datasize = FS_SIZE;
    p += fs->cmdsize;

    h->ncmds = 5;
    h->sizeofcmds = (uint32_t)(p - (buf + sizeof *h));

    /* One function start at base + 0x400, then the 0 terminator.
     * ULEB128 of 0x400 is 0x80 0x08. */
    uint8_t *f = buf + LE_OFF;
    f[0] = 0x80; f[1] = 0x08; f[2] = 0x00;

    /* The initializer that names no function start. */
    uint32_t bad = BAD_INIT;
    memcpy(buf + INIT_OFF, &bad, sizeof bad);

    int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); free(buf); return 2; }
    if (write(fd, buf, fsize) != (ssize_t)fsize) { perror("write"); close(fd); free(buf); return 2; }
    close(fd);
    free(buf);
    return 0;
}
