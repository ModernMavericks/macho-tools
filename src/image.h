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
    uint8_t               *buf;   /* the image; heap-owned by mi_open*, or a
                                    * caller's buffer wrapped by mi_wrap */
    size_t                 size;  /* the FILE's size -- what was read in, or
                                    * the wrapped buffer's size for mi_wrap */
    size_t                 cap;   /* how much buffer there is; >= size */
    struct mach_header_64 *hdr;   /* == (struct mach_header_64 *)buf */
    int                     owned; /* nonzero if mi_close must free(buf); 0 for
                                     * a buffer mi_wrap merely views */
} mi_image;

/* Read `path` whole, validate it is a 64-bit Mach-O whose load commands fit
 * inside the file, and populate *out. Returns 0 on success, non-zero otherwise
 * (unreadable, too short, wrong magic, load commands running past the end).
 * On failure *out is untouched and nothing is allocated. */
int mi_open(const char *path, mi_image *out);

/* As mi_open, but allocate `slack` writable bytes beyond the file, for a caller
 * that appends into the tail of the buffer rather than reallocating (patch_macho
 * builds its rebase/bind streams that way). `size` still reports the file size;
 * `cap` reports the allocation. Prefer plain mi_open and a realloc where the
 * growth is bounded and known -- this exists for the case where it is not. */
int mi_open_slack(const char *path, size_t slack, mi_image *out);

/* Build an mi_image over `buf` (exactly `size` bytes), which the CALLER owns --
 * no file, no read, no allocation. Runs the same validation mi_open does
 * (magic, load commands fitting inside `size`, no cmdsize striding past the
 * end) and returns non-zero on failure, leaving *out untouched. `cap` is set
 * to `size`. This is how a synthetic, in-memory Mach-O (macho_grow_test.c
 * builds several) gets the same validated view mi_open gives a file --
 * without a file to read. mi_close on a wrapped image never frees `buf`: the
 * caller still owns it. */
int mi_wrap(uint8_t *buf, size_t size, mi_image *out);

/* Free the buffer and zero the struct -- but only if the image owns it: a
 * buffer handed to mi_wrap belongs to the caller and is left untouched. Safe
 * on an all-zero mi_image, and safe after mi_release. */
void mi_close(mi_image *im);

/* Hand the buffer to the caller and empty the image. Use this when the caller
 * will realloc the buffer itself -- change_dylib's mg_grow_header(&buf, &fsize,
 * n) does, and an image still pointing at the old allocation would be a
 * dangling pointer waiting for mi_close. The caller must free() the result. */
uint8_t *mi_release(mi_image *im);

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
