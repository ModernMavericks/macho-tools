/* image.h — open, validate, iterate a 64-bit Mach-O.
 *
 * The layer under every rewriter in this repo. Today each of the seven opens a
 * file by hand (fstat, malloc, read, check MH_MAGIC_64) and walks load commands
 * with its own `for (i = 0; i < hdr->ncmds; i++)`; macho_grow.h does the walk
 * eleven times. They agree by coincidence. This is the one they converge on.
 *
 * Scope, deliberately narrow: 64-bit thin Mach-O only, read whole file into a
 * heap buffer, no writing. Growing, ordinal renumbering and __LINKEDIT surgery
 * are other modules -- this one answers "what is in here?" and nothing else.
 * 32-bit and fat are known gaps, filed as Task 5 in the toolkit plan. */

#ifndef MACHO9_IMAGE_H
#define MACHO9_IMAGE_H

#include <stdint.h>
#include <stddef.h>
#include <mach-o/loader.h>

typedef struct {
    uint8_t               *buf;   /* whole file, heap; owned by the mi_image */
    size_t                 size;
    struct mach_header_64 *hdr;   /* == (struct mach_header_64 *)buf */
} mi_image;

/* Read `path` whole, validate it is a 64-bit Mach-O whose load commands fit
 * inside the file, and populate *out. Returns 0 on success, non-zero otherwise
 * (unreadable, too short, wrong magic, load commands running past the end).
 * On failure *out is untouched and nothing is allocated. */
int mi_open(const char *path, mi_image *out);

/* Free the buffer and zero the struct. Safe on an all-zero mi_image. */
void mi_close(mi_image *im);

/* Visit each load command in order. The callback must not modify the command
 * chain -- this is iteration, not editing. */
typedef void (*mi_lc_fn)(const struct load_command *lc, void *ctx);
void mi_each_lc(const mi_image *im, mi_lc_fn cb, void *ctx);

/* Find LC_SEGMENT_64 `name` (e.g. "__TEXT"), or NULL. Segment and section names
 * are 16 bytes and need not be NUL-terminated, which is the trap these wrap. */
struct segment_command_64 *mi_find_segment(const mi_image *im, const char *name);

/* Find section `sect` within segment `seg`, or NULL. */
struct section_64 *mi_find_section(const mi_image *im, const char *seg, const char *sect);

/* The image base: the vmaddr of the segment whose file range covers offset 0 --
 * the one the header itself lives in. That is what every base-relative fixup in
 * macho_grow.h means by "base". Usually __TEXT, but derived rather than assumed.
 * Returns 0 if no segment maps the header. */
uint64_t mi_text_base(const mi_image *im);

#endif /* MACHO9_IMAGE_H */
