/* image.c — open, validate, iterate a 64-bit Mach-O. See image.h. */

#include "image.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Segment and section names are char[16] and are NOT required to be
 * NUL-terminated -- a name of exactly 16 characters fills the field. strcmp
 * would run off the end of one; strncmp against the field width is the
 * comparison the format actually calls for. */
static int name_eq(const char *field, const char *want) {
    return strncmp(field, want, 16) == 0;
}

int mi_open(const char *path, mi_image *out) {
    return mi_open_slack(path, 0, out);
}

int mi_open_slack(const char *path, size_t slack, mi_image *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 1;

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return 1; }
    if (st.st_size < (off_t)sizeof(struct mach_header_64)) { close(fd); return 1; }

    size_t cap = (size_t)st.st_size + slack;
    if (cap < (size_t)st.st_size) { close(fd); return 1; }   /* overflow */
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) { close(fd); return 1; }
    if (read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) {
        free(buf); close(fd); return 1;
    }
    close(fd);

    struct mach_header_64 *hdr = (struct mach_header_64 *)buf;
    if (hdr->magic != MH_MAGIC_64) { free(buf); return 1; }

    /* The load commands must fit in the file, and each must be large enough to
     * be a load command and not stride past the end of the region. A rewriter
     * that trusts ncmds and walks off the buffer is the failure this prevents,
     * and it is why every caller gets to drop its own bounds check. */
    size_t region = sizeof(*hdr) + (size_t)hdr->sizeofcmds;
    if (region > (size_t)st.st_size) { free(buf); return 1; }

    size_t off = 0;
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        if (off + sizeof(struct load_command) > (size_t)hdr->sizeofcmds) { free(buf); return 1; }
        const struct load_command *lc =
            (const struct load_command *)(buf + sizeof(*hdr) + off);
        if (lc->cmdsize < sizeof(struct load_command)) { free(buf); return 1; }
        if (off + lc->cmdsize > (size_t)hdr->sizeofcmds) { free(buf); return 1; }
        off += lc->cmdsize;
    }

    out->buf  = buf;
    out->size = (size_t)st.st_size;
    out->cap  = cap;
    out->hdr  = hdr;
    return 0;
}

void mi_close(mi_image *im) {
    if (!im) return;
    free(im->buf);
    im->buf = NULL; im->size = 0; im->cap = 0; im->hdr = NULL;
}

uint8_t *mi_release(mi_image *im) {
    uint8_t *buf = im->buf;
    im->buf = NULL; im->size = 0; im->cap = 0; im->hdr = NULL;
    return buf;
}

void mi_each_lc(const mi_image *im, mi_lc_fn cb, void *ctx) {
    /* mi_open has already proved this walk stays in bounds. */
    const uint8_t *p = im->buf + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < im->hdr->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)p;
        cb(lc, ctx);
        p += lc->cmdsize;
    }
}

struct segment_command_64 *mi_find_segment(const mi_image *im, const char *name) {
    uint8_t *p = im->buf + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < im->hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)p;
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *sg = (struct segment_command_64 *)lc;
            if (name_eq(sg->segname, name)) return sg;
        }
        p += lc->cmdsize;
    }
    return NULL;
}

struct section_64 *mi_find_section(const mi_image *im, const char *seg, const char *sect) {
    struct segment_command_64 *sg = mi_find_segment(im, seg);
    if (!sg) return NULL;
    struct section_64 *s = (struct section_64 *)(sg + 1);
    for (uint32_t i = 0; i < sg->nsects; i++) {
        if (name_eq(s[i].sectname, sect)) return &s[i];
    }
    return NULL;
}

uint64_t mi_text_base(const mi_image *im) {
    /* The segment that maps the header: fileoff 0 with actual file content.
     * The filesize test is what excludes __PAGEZERO, which also has fileoff 0
     * but maps nothing -- taking its vmaddr (0) as the base would make every
     * base-relative fixup wrong by exactly the real base. */
    uint8_t *p = im->buf + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < im->hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)p;
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *sg = (struct segment_command_64 *)lc;
            if (sg->fileoff == 0 && sg->filesize > 0) return sg->vmaddr;
        }
        p += lc->cmdsize;
    }
    return 0;
}
