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

/* mi_open/mi_open_slack's two failure reasons -- distinguishable so a caller
 * that wants to tell "the file couldn't even be opened or read" apart from
 * "it opened fine and just isn't a valid 64-bit Mach-O" can (cli/macho9.c's
 * EX_REFUSED/EX_FAIL split, and src/rewrite.c's/src/version_min.c's
 * MR_REFUSED/MR_FAIL, both need exactly this distinction and used to have no
 * way to get it from these two functions). Both are negative so 0 stays
 * success. -1 and -2 are NOT reserved values -- they equal, among others,
 * declassify.h's MDCL_NOT_MACHO/MDCL_REFUSED, swift_retag.h's MSWIFT_ERROR/
 * MSWIFT_NOT_MACHO, and src/rewrite.c's own private MR_ERROR/MR_SKIP -- but
 * that never matters, because every caller of any of these tests its
 * result BY NAME, never by comparing the raw number, so which small
 * negative integer any one module happens to pick is not a namespace two
 * modules could actually collide in. MI_IO_ERROR covers open, fstat, the
 * size-overflow guard on `slack`, malloc and read; MI_NOT_MACHO covers
 * every case mi_validate rejects (too short, wrong magic, load commands
 * failing validation) -- "too short" is grouped with the latter, not the
 * former: it is a decision about what the file's own size says, not a
 * syscall failing. mi_wrap, which never touches a syscall, only ever
 * returns MI_NOT_MACHO. A caller
 * that only checks `!= 0` (most of them) is entirely unaffected by adding
 * this distinction -- both values are still nonzero. */
#define MI_IO_ERROR   (-1)
#define MI_NOT_MACHO  (-2)

/* Read `path` whole, validate it is a 64-bit Mach-O whose load commands fit
 * inside the file, and populate *out. Returns 0 on success, MI_IO_ERROR or
 * MI_NOT_MACHO otherwise (see those constants above). On failure *out is
 * untouched and nothing is allocated. */
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
 * end, and each LC_SEGMENT_64's cmdsize actually covering the section_64
 * array its own nsects claims) and returns MI_NOT_MACHO on failure (never
 * MI_IO_ERROR: there is no I/O here to fail), leaving *out untouched. `cap`
 * is set to `size`. This is how a synthetic, in-memory Mach-O
 * (macho_grow_test.c builds several) gets the same validated view mi_open
 * gives a file -- without a file to read. mi_close on a wrapped image never
 * frees `buf`: the caller still owns it. */
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

/* Visit each load command in order. The callback receives `lc` as const, but
 * that is only a hint, not enforcement -- the underlying memory is whatever
 * the caller's buffer is (mutable, for an owned or wrapped image opened
 * O_RDWR). A callback MAY write through a cast-away-const `lc` to edit a
 * command's own fixed-size fields, or fields several hops away through the
 * buffer -- a segment's segname (rename_segment.c's rs_rename_lc), a class
 * record's tag bits (retag_swift_classes.c's retag()), a fixup pointer's raw
 * bits (patch_macho.c's pm_collect_lc leaves these alone but a caller could).
 * The one invariant a callback must NEVER violate, no matter what else it
 * edits: THE COMMAND-CHAIN SHAPE ITSELF must stay exactly what it was when
 * the walk started -- concretely, never assign to `lc->cmd`, never assign to
 * `lc->cmdsize`, and never assign to `im->hdr->ncmds`. mi_each_lc's own loop
 * reads `lc->cmdsize` via this same `lc` right after the callback returns to
 * compute the next command's address, and it uses `im->hdr->ncmds` (read
 * once, before the loop starts) to know when to stop. Changing either
 * desyncs that stride from the buffer's real shape -- reading a stale
 * cmdsize as the next command's header, walking past the real end, or
 * stopping short -- silently, since there is no bounds check inside the loop
 * verifying the walk still lines up with what mi_validate proved at open
 * time. This is a prohibition on those three fields only, not a blanket ban
 * on writing through `lc` -- everything else in the command, or reachable
 * from it, is fair game.
 *
 * The callback returns int: 0 to continue, non-zero to stop the walk early
 * (a refusal partway through, e.g. change_dylib.c's build_lcs on a malformed
 * dylib/rpath name offset -- see its own comment for why it must not keep
 * calling the callback, and hence writing into the caller's output buffer,
 * once it has decided to refuse). mi_each_lc itself returns 1 if it visited
 * every command (the callback never asked to stop) or 0 if a callback's
 * non-zero return cut the walk short -- so the caller can propagate the
 * refusal instead of trusting whatever the callback wrote before stopping.
 *
 * There is deliberately only ONE walking loop behind this signature (not a
 * separate void-callback variant kept alongside it) -- this codebase's
 * recurring bug class is two places independently deciding one thing, and a
 * second iterator with its own copy of the bounds/stride logic would be
 * exactly that, for the walk itself. A callback with nothing to abort for
 * just always returns 0.
 *
 * No MUTATING variant exists here either, on purpose. The one caller that
 * ever needed that shape was compat/fix_macho.c's -strip_build_version walk,
 * which shrank the chain mid-iteration (memmove'd a later command down over
 * the one being dropped, shrank ncmds/sizeofcmds, and revisited the same
 * cursor instead of advancing). That is a genuinely different contract from
 * "stop early" -- the caller would own recomputing bounds and deciding
 * whether to advance after every call, i.e. exactly the stride logic this
 * module exists to centralize, pushed back out to every such caller. THAT
 * CALLER IS GONE: fix_macho is a /bin/sh wrapper now and the deletion runs
 * through src/rewrite.c, which rebuilds the load-command table wholesale
 * rather than mutating it in place. So today there are ZERO callers wanting
 * a mutating walk, which makes not having one easier still. Revisit only if
 * one shows up. */
typedef int (*mi_lc_fn)(const struct load_command *lc, void *ctx);
int mi_each_lc(const mi_image *im, mi_lc_fn cb, void *ctx);

/* Find LC_SEGMENT_64 `name` (e.g. "__TEXT"), or NULL. Segment and section names
 * are 16 bytes and need not be NUL-terminated, which is the trap these wrap. */
struct segment_command_64 *mi_find_segment(const mi_image *im, const char *name);

/* Find section `sect` within segment `seg`, or NULL. */
struct section_64 *mi_find_section(const mi_image *im, const char *seg, const char *sect);

/* The image base: the vmaddr of the segment whose file range covers offset 0 --
 * the one the header itself lives in. That is what every base-relative fixup in
 * macho_grow.h means by "base". Usually __TEXT, but derived rather than assumed.
 * Returns 0 if no segment maps the header -- which is indistinguishable from
 * a base that legitimately IS 0 (every dylib and bundle). A caller that uses
 * the base as a PRECONDITION must call mi_image_base below instead; two that
 * did not were refusing every dylib on the machine. */
uint64_t mi_text_base(const mi_image *im);

/* The image's base vmaddr -- the vmaddr of the segment that maps the header,
 * which is __TEXT in every image this toolkit handles.
 *
 * Separate from mi_text_base because that function returns 0 BOTH for "no
 * segment maps the header" and for "the base is 0", and a dylib's base
 * legitimately IS 0: dylibs are linked at zero and slid at load time. Callers
 * that use the base as a precondition need to tell those apart, and the two
 * that did not were refusing every dylib on the machine.
 *
 * Returns 0 with *out set (which may be 0), or -1 if no segment maps the
 * header. */
int mi_image_base(const mi_image *im, uint64_t *out);

#endif /* MACHO9_IMAGE_H */
