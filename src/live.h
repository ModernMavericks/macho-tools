/* live.h — the same structure queries as image.c, against an image already
 * mapped into the CURRENT process, not a file read into a buffer.
 *
 * HEADER-ONLY, NEVER GAINS A .c. Every function below is `static inline` and
 * every one of them touches nothing but the caller's own pointers and
 * arithmetic on them: no malloc/free, no stdio, no locks, no errno-setting
 * library call, nothing an async-signal-safe context cannot use. This is not
 * a style preference -- avxemu's SIGILL handler runs signal-handler-safe and
 * must stay VEX-free; it cannot link a translation unit that might allocate
 * or block, so the one thing this file is not allowed to do, ever, is grow a
 * .c or call into one. tests/live_probe.c plus tests/live_test.c's
 * `test_probe_object_is_allocation_free` pin this with a real check (compile
 * a TU that includes only this header, `nm` the object, assert no
 * malloc/free/stdio symbol appears) rather than leaving it as a comment
 * nobody enforces.
 *
 * Scope, deliberately narrow, mirroring image.h's own restraint: 64-bit
 * images only, read-only (this never writes into a live image -- there is no
 * rewriting use case here, only "what is at this address"), and only the
 * three queries avxemu's design actually needs: find a segment, find a
 * section, walk the load commands. Names are kept parallel to image.h's
 * (`mi_find_segment` / `mlive_find_segment`, `mi_find_section` /
 * `mlive_find_section`, `mi_each_lc` / `mlive_each_lc`) precisely because
 * they answer the SAME question -- "where is this segment/section/command in
 * this Mach-O" -- so a reader who knows one recognizes the other. The bodies
 * are not shared, and cannot be: image.c's version validates cmdsize strides
 * against a known file size before trusting them; this version has no size
 * to check against (see below), so the two functions differ in exactly, and
 * only, that one respect.
 *
 * THE CONTRACT THIS FILE DOES NOT AND CANNOT ENFORCE, stated precisely
 * because it replaces a bounds check image.c gets to make and this file
 * cannot:
 *
 *   The caller must pass a `struct mach_header_64 *` that genuinely is the
 *   base of a Mach-O image mapped into THIS process by dyld -- what
 *   `_dyld_get_image_header(i)` returns (cast from `struct mach_header *`;
 *   the two structs agree on every field up to the load commands, which is
 *   the standard idiom for this cast and the same layout mi_validate in
 *   image.c checks against a file's bytes) or an address a caller has
 *   independently proven the same way. Given such a pointer, this file
 *   trusts `ncmds`/`sizeofcmds` the way image.c trusts them only AFTER
 *   proving the region they claim fits inside the file -- but there is no
 *   "file" here, no fsize to bound against, because the image is mapped
 *   memory whose extent this code has no way to ask the kernel for without
 *   a syscall (forbidden, see above). The one thing checked, in
 *   `mlive_valid`, is `magic == MH_MAGIC_64` -- cheap, syscall-free, and the
 *   only defense a signal-handler context can afford. Everything past that
 *   is trusted, exactly as it must be: dyld already validated the image
 *   enough to load and run code from it before this process's first
 *   instruction executed, so by the time any of this file's callers can run
 *   at all, the image has already survived a stronger check than anything
 *   this file could perform. Handing this file a pointer that is NOT such
 *   an image (a heap buffer, a `mach_header_64` some other code built by
 *   hand, an address that merely looks plausible) is undefined behavior --
 *   exactly as undefined as walking any other unvalidated structure through
 *   raw pointer arithmetic, which is what every function here does.
 *
 *   Segment/section vmaddr fields are LINK-TIME addresses. To get where a
 *   segment or section actually sits in this process's address space, add
 *   the image's slide (`_dyld_get_image_vmaddr_slide(i)`) -- `mlive_addr`
 *   does exactly that and nothing else. image.c has no equivalent function
 *   because a file has no slide; this is the other place the two files
 *   genuinely cannot share a body, for the mirror-image reason to the one
 *   above (image.c bounds against fsize with no slide to add; this file
 *   adds a slide with no fsize to bound against).
 *
 * Segment and section names are `char[16]` and are NOT NUL-terminated --
 * same trap image.c's own `name_eq` comment documents, and this file needs
 * its own three-line copy of that comparison rather than reusing image.c's:
 * image.c's `name_eq` is `static` (private to that translation unit, which
 * is not header-only and links `malloc`), so there is no legal way for a
 * header-only file to call it. Duplicating three lines of `strncmp` is not
 * the kind of duplication constraint 4 warns against -- there is no
 * meaningful logic in "compare a fixed-width field", only the trap of
 * getting the width wrong, and both copies get it from the same place: the
 * Mach-O format's own field width.
 */

#ifndef MACHO9_LIVE_H
#define MACHO9_LIVE_H

#include <stdint.h>
#include <string.h>   /* strncmp only -- no allocation, no locking, no errno;
                       * signal-safety(7) lists it explicitly safe. */
#include <mach-o/loader.h>

/* Segment/section names are char[16], not necessarily NUL-terminated (see
 * this file's own top comment). strncmp against the field width, never
 * strcmp/strcpy. */
static inline int mlive_name_eq(const char *field, const char *want) {
    return strncmp(field, want, 16) == 0;
}

/* Is `mh` a 64-bit Mach-O header this file can walk at all? The only check
 * this file can make without a syscall or a size bound -- see the top
 * comment for why that is the whole story, not a shortcut. NULL is handled
 * so every other function below can call this once and be done, rather than
 * every caller needing its own NULL check first. */
static inline int mlive_valid(const struct mach_header_64 *mh) {
    return mh != NULL && mh->magic == MH_MAGIC_64;
}

/* The callback signature for mlive_each_lc, mirroring mi_lc_fn in image.h:
 * return 0 to keep walking, non-zero to stop early. There is deliberately
 * only this one iterator, for the same reason image.h gives only one --
 * see its comment. */
typedef int (*mlive_lc_fn)(const struct load_command *lc, void *ctx);

/* Visit each load command in order. Returns 1 if every command was visited,
 * 0 if a callback's non-zero return stopped the walk early, or if `mh`
 * fails mlive_valid. */
static inline int mlive_each_lc(const struct mach_header_64 *mh, mlive_lc_fn cb, void *ctx) {
    if (!mlive_valid(mh)) return 0;
    const uint8_t *p = (const uint8_t *)mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)(const void *)p;
        if (cb(lc, ctx)) return 0;
        p += lc->cmdsize;
    }
    return 1;
}

/* Find LC_SEGMENT_64 `name` (e.g. "__TEXT") in the image at `mh`, or NULL.
 * Read-only, unlike mi_find_segment: there is no rewriting use case for a
 * live image here (avxemu inspects, it does not patch, a running process's
 * own mapped segments through this path), so the returned pointer is const
 * -- a caller that genuinely needs to write through it (none does today)
 * would have to cast the const away itself, visibly. */
static inline const struct segment_command_64 *mlive_find_segment(
        const struct mach_header_64 *mh, const char *name) {
    if (!mlive_valid(mh)) return NULL;
    const uint8_t *p = (const uint8_t *)mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)(const void *)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg =
                (const struct segment_command_64 *)(const void *)p;
            if (mlive_name_eq(sg->segname, name)) return sg;
        }
        p += lc->cmdsize;
    }
    return NULL;
}

/* Find section `sect` within segment `seg` in the image at `mh`, or NULL. */
static inline const struct section_64 *mlive_find_section(
        const struct mach_header_64 *mh, const char *seg, const char *sect) {
    const struct segment_command_64 *sg = mlive_find_segment(mh, seg);
    if (!sg) return NULL;
    const struct section_64 *s = (const struct section_64 *)(const void *)(sg + 1);
    for (uint32_t i = 0; i < sg->nsects; i++) {
        if (mlive_name_eq(s[i].sectname, sect)) return &s[i];
    }
    return NULL;
}

/* Where `vmaddr` actually sits in THIS process's address space right now:
 * the link-time address plus the image's runtime slide. Pass a segment's or
 * section's own `->vmaddr`/`->addr` and the value
 * `_dyld_get_image_vmaddr_slide` returned for this same image. See the top
 * comment for why this exists here and has no image.c counterpart. */
static inline uintptr_t mlive_addr(uint64_t vmaddr, intptr_t slide) {
    return (uintptr_t)vmaddr + (uintptr_t)slide;
}

#endif /* MACHO9_LIVE_H */
