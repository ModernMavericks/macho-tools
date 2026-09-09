/* live.h — the same structure queries as image.c, against an image already
 * mapped into the CURRENT process, not a file read into a buffer.
 *
 * HEADER-ONLY, NEVER GAINS A .c. Every function below is `static inline` and
 * every one of them touches nothing but the caller's own pointers and
 * arithmetic on them: no malloc/free, no stdio, no locks, no errno-setting
 * library call, and (see the name-compare note below) NO CALL THROUGH THE
 * DYNAMIC LINKER AT ALL -- nothing an async-signal-safe context cannot use.
 * This is not a style preference -- avxemu's SIGILL handler runs
 * signal-handler-safe and must stay VEX-free; it cannot link a translation
 * unit that might allocate or block, so the one thing this file is not
 * allowed to do, ever, is grow a .c or call into one. tests/live_probe.c
 * plus tests/live_test.c's `test_probe_object_is_allocation_free` pin this
 * with a real check: compile a TU that includes only this header, run
 * `nm -u` on the object, and assert its undefined-symbol list is EMPTY --
 * an ALLOWLIST of nothing, not a denylist of a few names, so a call into
 * errno, getenv, pthread_once, `_dyld_get_image_header` (a dyld-locking
 * call, ironically the shape of the very lookup this file's callers already
 * did before calling in), or an accidentally-added `.c` are all caught the
 * same way an explicit malloc/free would be, rather than only the specific
 * names anyone thought to list.
 *
 * ON strncmp, AND WHY THIS FILE DOES NOT USE THE LIBRARY ONE: an earlier
 * revision called libc's `strncmp` for the 16-byte name compare below,
 * reasoning that it is on the (Linux) signal-safety(7) safe list. That
 * reasoning missed a macOS-specific hazard signal-safety(7) does not cover:
 * on this platform an extern call like `strncmp` is lazily bound through
 * `__la_symbol_ptr` -- its FIRST call from anywhere in the process runs
 * `dyld_stub_binder`, which locks and can allocate. A SIGILL arriving while
 * the interrupted thread already held that same dyld lock would deadlock
 * the handler on its own first comparison. `sigaction(2)`'s async-signal-safe
 * list (not signal-safety(7)'s Linux one) is the correct citation for a
 * macOS handler, and it says nothing rescues a lazily-bound symbol either --
 * the fix is to not have one. `mlive_name_eq` below is 16 bytes of inline
 * comparison instead, which is also why this header calls NO external
 * function of any kind and its allocation-free probe's allowlist can be
 * empty rather than "empty except strncmp".
 *
 * Scope, deliberately narrow, mirroring image.h's own restraint: 64-bit
 * images only, read-only (this never writes into a live image -- there is no
 * rewriting use case here, only "what is at this address"), and only the
 * three queries avxemu's design actually needs: find a segment, find a
 * section, walk the load commands. Names are kept parallel to image.h's
 * (`mi_find_segment` / `mlive_find_segment`, `mi_find_section` /
 * `mlive_find_section`, `mi_each_lc` / `mlive_each_lc`) precisely because
 * they answer the SAME question -- "where is this segment/section/command in
 * this Mach-O" -- so a reader who knows one recognizes the other, and
 * `mlive_find_segment`/`mlive_find_section` are now implemented ON TOP OF
 * `mlive_each_lc` rather than re-walking by hand, so the bounds-checked
 * stride logic below lives in exactly one place, not three -- the class of
 * bug this whole codebase keeps naming (image.h's own comment on why there
 * is only one iterator makes the same argument).
 *
 * WHAT THIS FILE VALIDATES, PRECISELY, versus what image.c validates and why
 * the two genuinely cannot share a body:
 *
 *   image.c's mi_validate bounds every load command's `cmdsize` against
 *   `hdr->sizeofcmds` (`cmdsize >= sizeof(load_command)`, `cmdsize % 8 == 0`,
 *   the running offset never exceeding `sizeofcmds`) AND bounds
 *   `sizeofcmds` itself against the whole file's size, `fsize`, which it has
 *   because it just read the file and knows how many bytes it holds.
 *
 *   `mlive_each_lc` below performs the FIRST set of checks -- everything
 *   bounded against `sizeofcmds`, including (mirroring mi_validate's own
 *   LC_SEGMENT_64 case) that a segment's own `cmdsize` actually covers its
 *   trailing `section_64` array, so `mlive_find_section` never trusts
 *   `nsects` unchecked either. All four checks are read entirely from the
 *   header itself -- no syscall, no allocation, so there is no reason for a
 *   signal-handler-safe walker to skip them, and an earlier revision of this
 *   file that DID skip them was measured, by code review, to spin for
 *   twenty seconds on a header with `ncmds=4e9, cmdsize=0` inside what is
 *   supposed to be a signal handler; with these checks the same input is
 *   refused on the FIRST iteration (`cmdsize(0) < sizeof(load_command)`
 *   fails immediately).
 *
 *   What this file CANNOT do, and image.c can: bound `sizeofcmds` itself
 *   against anything larger, because there is no `fsize` for mapped memory
 *   -- no syscall this code is allowed to make would tell it how far the
 *   image's real mapping extends. So the contract is: the caller must pass
 *   a `struct mach_header_64 *` that genuinely is the base of a Mach-O image
 *   mapped into THIS process by dyld -- what `_dyld_get_image_header(i)`
 *   returns (cast from `struct mach_header *`; the two structs agree on
 *   every field up to the load commands, the standard idiom for this cast)
 *   -- or an address a caller has independently proven the same way. GIVEN
 *   such a pointer, `sizeofcmds` is trustworthy by construction: dyld
 *   validated and mapped this exact region before this process's first
 *   instruction ran, so by the time any caller of this file can run at all,
 *   the image has already survived a stronger check than this file could
 *   ever perform without a syscall. Handing this file a pointer that is NOT
 *   such an image (a heap buffer, a `mach_header_64` some other code built
 *   by hand with a lying `sizeofcmds`, an address that merely looks
 *   plausible) is undefined behavior once `sizeofcmds` bytes are trusted to
 *   be mapped and readable -- exactly as undefined as walking any other
 *   unvalidated structure through raw pointer arithmetic. `mlive_valid`'s
 *   magic check catches a WRONG pointer (a non-Mach-O address); it cannot
 *   catch a RIGHT-shaped header with a LYING `sizeofcmds`, and nothing
 *   syscall-free can.
 *
 *   Segment/section vmaddr fields are LINK-TIME addresses. To get where a
 *   segment or section actually sits in this process's address space, add
 *   the image's slide (`_dyld_get_image_vmaddr_slide(i)`) -- `mlive_addr`
 *   does exactly that and nothing else. image.c has no equivalent function
 *   because a file has no slide; this is the other genuine difference
 *   between the two files' bodies, the mirror image of the fsize point
 *   above (image.c bounds against fsize with no slide to add; this file
 *   adds a slide with no fsize to bound against).
 *
 * Segment and section names are `char[16]` and are NOT NUL-terminated --
 * same trap image.c's own `name_eq` comment documents. This file needs its
 * own copy rather than reusing image.c's (that one is `static`, private to
 * a translation unit that is not header-only and links `malloc`, so there
 * is no legal way for a header-only file to call it) -- and, per the
 * strncmp note above, needs it to be a hand-rolled loop rather than a call
 * to the library function of the same name.
 */

#ifndef MACHO9_LIVE_H
#define MACHO9_LIVE_H

#include <stddef.h>   /* NULL, size_t -- macros/typedefs only, no symbols */
#include <stdint.h>
#include <mach-o/loader.h>

/* Segment/section names are char[16], not necessarily NUL-terminated (see
 * this file's own top comment) -- so `field` is read for up to 16 bytes
 * regardless of where a NUL would be, and `want` (an ordinary caller-supplied
 * C string, e.g. "__TEXT") is never read past its own NUL: as soon as both
 * sides agree on a NUL byte at the same position, the fields match and
 * nothing past that position is read from either side, exactly like
 * `strncmp(field, want, 16) == 0` -- but inlined, with no call through the
 * dynamic linker (see the top comment for why that call was the problem). */
static inline int mlive_name_eq(const char *field, const char *want) {
    for (int i = 0; i < 16; i++) {
        char f = field[i], w = want[i];
        if (f != w) return 0;
        if (f == '\0') return 1;
    }
    return 1;
}

/* Is `mh` a 64-bit Mach-O header this file can walk at all? The only check
 * this file can make on the header pointer itself before it starts
 * trusting `ncmds`/`sizeofcmds` -- see the top comment for the full
 * contract, and why a right-shaped-but-lying header is a caller error this
 * check cannot catch. NULL is handled so every other function below can
 * call this once and be done, rather than every caller needing its own NULL
 * check first. */
static inline int mlive_valid(const struct mach_header_64 *mh) {
    return mh != NULL && mh->magic == MH_MAGIC_64;
}

/* The callback signature for mlive_each_lc, mirroring mi_lc_fn in image.h:
 * return 0 to keep walking, non-zero to stop early. There is deliberately
 * only this one iterator, for the same reason image.h gives only one --
 * see its comment -- and mlive_find_segment/mlive_find_section below are
 * built on top of it rather than re-walking by hand. */
typedef int (*mlive_lc_fn)(const struct load_command *lc, void *ctx);

/* Visit each load command in order, refusing to trust any command whose
 * cmdsize doesn't check out against sizeofcmds (see the top comment's
 * "WHAT THIS FILE VALIDATES" section for exactly which four checks and
 * why they're safe to make here). Returns:
 *   1  every command was visited and validated;
 *   0  a callback's non-zero return stopped the walk early -- an ordinary,
 *      expected outcome (mlive_find_segment uses this to stop once it has
 *      found its match), not an error;
 *  -1  `mh` failed mlive_valid, OR the walk hit a load command whose
 *      cmdsize does not check out -- the image cannot be trusted past that
 *      point. Distinguished from 0 so a caller that cares (unlike
 *      mlive_find_segment/mlive_find_section, which only ask "found it or
 *      not") can tell "stopped on purpose" from "this header is not what it
 *      claims to be". */
static inline int mlive_each_lc(const struct mach_header_64 *mh, mlive_lc_fn cb, void *ctx) {
    if (!mlive_valid(mh)) return -1;
    const uint8_t *base = (const uint8_t *)mh + sizeof(struct mach_header_64);
    /* size_t, not uint32_t, deliberately -- matching image.c's mi_validate.
     * sizeofcmds and cmdsize are each at most UINT32_MAX, so off + cmdsize
     * cannot overflow a (>=64-bit here) size_t; it very much CAN wrap a
     * 32-bit accumulator, which would let an oversized cmdsize slip past
     * the very bound check meant to catch it. */
    size_t off = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (off + sizeof(struct load_command) > (size_t)mh->sizeofcmds) return -1;
        const struct load_command *lc =
            (const struct load_command *)(const void *)(base + off);
        if (lc->cmdsize < sizeof(struct load_command)) return -1;
        /* 64-bit Mach-O requires every load command to be a multiple of 8
         * bytes, same reasoning as image.c's mi_validate: a caller
         * downstream of this walk indexes into later commands assuming
         * natural alignment. */
        if (lc->cmdsize % 8 != 0) return -1;
        if (off + lc->cmdsize > (size_t)mh->sizeofcmds) return -1;

        if (lc->cmd == LC_SEGMENT_64) {
            if (lc->cmdsize < sizeof(struct segment_command_64)) return -1;
            const struct segment_command_64 *sg =
                (const struct segment_command_64 *)(const void *)lc;
            /* The check a bare cmdsize/sizeofcmds bound misses (same
             * comment as image.c's mi_validate): a segment's cmdsize must
             * actually cover its trailing section_64 array, since nsects is
             * what mlive_find_section trusts. Widen to uint64_t first so
             * this bound check itself can never overflow. */
            uint64_t want = (uint64_t)sizeof(struct segment_command_64) +
                            (uint64_t)sg->nsects * sizeof(struct section_64);
            if (lc->cmdsize != want) return -1;
        }

        if (cb(lc, ctx)) return 0;   /* callback asked to stop -- not an error */
        off += lc->cmdsize;
    }
    return 1;
}

/* Context + callback pair mlive_find_segment builds on mlive_each_lc with,
 * rather than walking load commands a second time by hand. */
typedef struct { const char *name; const struct segment_command_64 *found; } mlive__seg_ctx;

static inline int mlive__match_segment(const struct load_command *lc, void *ctx_) {
    mlive__seg_ctx *ctx = (mlive__seg_ctx *)ctx_;
    if (lc->cmd == LC_SEGMENT_64) {
        const struct segment_command_64 *sg = (const struct segment_command_64 *)(const void *)lc;
        if (mlive_name_eq(sg->segname, ctx->name)) { ctx->found = sg; return 1; }
    }
    return 0;
}

/* Find LC_SEGMENT_64 `name` (e.g. "__TEXT") in the image at `mh`, or NULL.
 * Read-only, unlike mi_find_segment: there is no rewriting use case for a
 * live image here (avxemu inspects, it does not patch, a running process's
 * own mapped segments through this path), so the returned pointer is const
 * -- a caller that genuinely needs to write through it (none does today)
 * would have to cast the const away itself, visibly. */
static inline const struct segment_command_64 *mlive_find_segment(
        const struct mach_header_64 *mh, const char *name) {
    mlive__seg_ctx ctx = { name, NULL };
    mlive_each_lc(mh, mlive__match_segment, &ctx);
    return ctx.found;
}

/* Find section `sect` within segment `seg` in the image at `mh`, or NULL.
 * `sg->nsects` is trustworthy here because mlive_each_lc already proved (as
 * part of finding `sg` at all) that this segment's cmdsize covers exactly
 * `nsects` trailing section_64 entries -- the check the top comment calls
 * out as the one a bare cmdsize/sizeofcmds bound misses. */
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
