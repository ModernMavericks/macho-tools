#ifndef MACHOTOOL_DECLASSIFY_H
#define MACHOTOOL_DECLASSIFY_H
/*
 * md_ -- lowering a modern Mach-O's CHAINED FIXUPS to the LC_DYLD_INFO_ONLY
 * rebase/bind opcode streams dyld has understood since 10.6, so a binary a
 * 2021-and-later toolchain produced can be loaded on 10.9 at all.
 *
 * This is compat/patch_macho.c's whole conversion, lifted out of that tool's
 * main() so it is a library function rather than a program. cli/macho9.c's
 * `declassify` verb and src/edit.c's `fixups set classic` statement are its
 * only C front-ends (the latter through md_declassify_buf, below, the same
 * conversion on an image already in memory); the old grammar,
 * `patch_macho IN OUT`, reaches this same code through compat/patch_macho.sh,
 * the /bin/sh wrapper that replaced compat/patch_macho.c. That wrapper is
 * what still reproduces the old tool's observables -- exit 1 for everything
 * that goes wrong, and no "Wrote ..." line on the pass-through path.
 *
 * Everything the conversion itself PRINTS lives down here, once, exactly as
 * patch_macho has always printed it, for the same reason rewrite.h keeps
 * change_dylib's diagnostics down there: two copies of a message drift, and
 * tests/characterize.sh and tests/chained-fixups.sh both run the conversion
 * through patch_macho.
 *
 * WHAT IT DOES, in the order the file is touched:
 *   - collects every LC_SEGMENT_64 (a chained-fixups entry names its segment
 *     by index, so the table has to be built before anything can be resolved),
 *     and finds LC_DYLD_EXPORTS_TRIE / LC_DYLD_CHAINED_FIXUPS /
 *     LC_DYLD_INFO_ONLY / LC_BUILD_VERSION;
 *   - walks every fixup chain, writing each slot's final value back (a rebase
 *     target, or 0 for a bind that dyld will fill in) and emitting the classic
 *     REBASE_/BIND_ opcodes that say what it just undid;
 *   - strips LC_DYLD_EXPORTS_TRIE, LC_DYLD_CHAINED_FIXUPS and every
 *     LC_BUILD_VERSION (10.9's dyld understands none of them);
 *   - appends the two opcode streams past the end of the file, adds a 48-byte
 *     LC_DYLD_INFO_ONLY pointing at them (and at the export trie, still in
 *     place), and EXTENDS __LINKEDIT to cover them -- dyld only reads file
 *     ranges some segment declares. That last step is one of this repo's two
 *     sanctioned exceptions to "never move a byte of file data": nothing is
 *     moved, but the image does grow past __LINKEDIT's old end.
 *
 * IDEMPOTENT: a binary that already has LC_DYLD_INFO_ONLY and no chained
 * fixups is passed through unchanged (MDCL_PASSTHROUGH) rather than refused,
 * so a driver script -- mavericksforever.com/claude/install.sh is one -- can
 * run it over a binary that may already have been patched.
 *
 * THIN ONLY: the conversion reads one 64-bit thin Mach-O. A fat container
 * gets MDCL_NOT_MACHO, like everything else mi_open_slack declines on
 * CONTENT grounds (too short, wrong magic, load commands failing
 * validation); an IN mi_open_slack cannot even open, read, or allocate
 * for gets MDCL_ERROR instead -- that is an operational failure, not a
 * judgement about a fat container or any other content.
 */
#include <stdint.h>
#include <stddef.h>

/* Returns from md_declassify. A caller must test these BY NAME, never with a
 * bare `< 0` or `!= 0`: 0 and 1 are both successes that differ only in what
 * was done, and the two negatives differ in whether anything has been printed
 * yet -- which is the whole difference between a front-end's message and a
 * duplicate of one. */
#define MDCL_CONVERTED     0   /* chained fixups lowered; *out_len may exceed
                                * the input's size (the appended streams) */
#define MDCL_PASSTHROUGH   1   /* already LC_DYLD_INFO_ONLY and fixup-free;
                                * *out_buf is the input, byte for byte */
#define MDCL_NOT_MACHO   (-1)  /* not a readable 64-bit thin Mach-O; NOTHING
                                * printed, so a front-end that cares must say
                                * so itself, in its own vocabulary */
#define MDCL_REFUSED     (-2)  /* examined and declined on purpose (see LIMITS
                                * below for the full list); the reason is
                                * already on stderr */
#define MDCL_ERROR       (-3)  /* an operational failure: IN could not even be
                                * opened, read, or allocated for
                                * (mi_open_slack's own MI_IO_ERROR), or an
                                * allocation this conversion could not make
                                * -- not a judgement about the input; already
                                * reported. A caller that distinguishes
                                * refusal from failure (the `declassify` verb
                                * does) must NOT report this as a refusal */

/* The most load commands one conversion strips: the size of its removal
 * table (see LIMITS, below, and src/declassify.c's md_collect_ctx for why 16),
 * and so of md_report's list of them. */
#define MDCL_MAX_STRIP 16

/* LIMITS, and what happens at each -- every one of them is a refusal, never a
 * truncated or corrupted output. The conversion works inside two fixed
 * budgets, both of them deliberate: it appends into slack allocated with the
 * file rather than reallocating, and it emits into fixed opcode buffers.
 *
 *   32 LC_SEGMENT_64 commands, and 16 strippable ones
 *     (LC_DYLD_EXPORTS_TRIE/LC_DYLD_CHAINED_FIXUPS/LC_BUILD_VERSION together)
 *     -- more of either is MDCL_REFUSED.
 *   1MB of rebase opcodes and 1MB of bind opcodes. A rebase costs about 5
 *     bytes (SET_SEGMENT_AND_OFFSET_ULEB + DO_REBASE_IMM_TIMES), so the
 *     rebase budget is roughly 200k fixups; a bind costs far more --
 *     SET_DYLIB_ORDINAL(_IMM or _ULEB), SET_SYMBOL_TRAILING_FLAGS_IMM, the
 *     symbol name plus its NUL, SET_SEGMENT_AND_OFFSET_ULEB, and DO_BIND --
 *     roughly 27 bytes for a typical symbol name, so the bind budget is
 *     reached around 40k fixups, not 200k. A binary with more fixups than
 *     its budget allows is MDCL_REFUSED, not a binary whose remaining
 *     pointers silently go unrebased. Which budget is reached first is not
 *     fixed -- it depends on the image's rebase:bind ratio, and because a
 *     bind costs roughly 5x what a rebase does, the bind budget is the one
 *     reached first only when binds outnumber rebases by more than about
 *     1:5, which is the opposite of the ratio the one real overflow seen so
 *     far has: src/declassify.c's opcode-buffer comment measures it as
 *     rebase-heavy, on the Node binary install.sh fetches and runs this
 *     conversion over.
 *   2MB of slack past the end of the file (MDCL_SLACK, below), which both
 *     finished streams plus their 8-byte alignment must fit inside --
 *     MDCL_REFUSED otherwise.
 *   48 bytes of header pad for the new LC_DYLD_INFO_ONLY, and a __LINKEDIT
 *     segment to extend -- MDCL_REFUSED without either. The pad ends at the
 *     first section's file data, so an image with no section data, or whose
 *     first section's offset lies past its end, is MDCL_REFUSED too, rather
 *     than given a guessed bound.
 *   An unknown chained-fixups pointer format is MDCL_REFUSED. A fixup that
 *     points outside the file, or a bind naming an ordinal the import table
 *     does not have, abandons THAT CHAIN with a message and keeps going --
 *     the one place this conversion continues rather than refusing, unchanged
 *     from patch_macho.
 *
 * A caller therefore never has to bound its input itself, and never has to
 * wonder whether a zero return means the whole file was converted. */

/*
 * Read the Mach-O at `path` and produce the declassified image in memory.
 *
 * On MDCL_CONVERTED or MDCL_PASSTHROUGH, *out_buf is a malloc'd buffer the
 * CALLER must free() and *out_len is exactly how many of its bytes are the
 * output file -- write them and nothing else. Both front-ends write the same
 * bytes because they are handed the same buffer; that is what makes `macho9
 * declassify` and `patch_macho` byte-identical by construction rather than by
 * agreement (tests/cli_test.sh and tests/chained-fixups.sh assert it anyway).
 *
 * On ANY negative return nothing is allocated and nothing is written; the
 * file on disk is never touched by this function in any case, since it only
 * ever reads.
 */
int md_declassify(const char *path, uint8_t **out_buf, size_t *out_len);

/* The headroom the conversion appends into, past the end of the image: the
 * LIMITS section's 2MB of slack. md_declassify reads the file with this much
 * allocated beyond it; a caller of md_declassify_buf must provide it the
 * same way. */
#define MDCL_SLACK (2*1024*1024)

/* What one conversion did, for a caller that reports it (src/edit.c's
 * verbose log). Every field is a figure the conversion already has in hand
 * as it works -- the counters behind its "Processed N rebases, M binds"
 * line, the lengths of the two streams it emits, the commands it strips, and
 * __LINKEDIT's size before and after -- copied out, never recounted. */
typedef struct {
    int      rebases;          /* fixups lowered to REBASE_ opcodes */
    int      binds;            /* fixups lowered to BIND_ opcodes */
    size_t   rebase_bytes;     /* the rebase stream's length */
    size_t   bind_bytes;       /* the bind stream's length */
    size_t   appended;         /* bytes past the input's end: both streams
                                * and their 8-byte alignment */
    uint32_t stripped[MDCL_MAX_STRIP]; /* the commands removed, as LC_*
                                        * values, in load-command order */
    int      n_stripped;
    uint64_t linkedit_before;  /* __LINKEDIT's filesize as found */
    uint64_t linkedit_after;   /* ... and as left: larger when the conversion
                                * extended it over the streams, else equal */
} md_report;

/*
 * md_declassify's conversion, without the file: the same work on an image
 * already in memory. buf[0..fsize) is the image and buf[fsize..cap) is
 * writable room past it -- MDCL_SLACK of it, as md_declassify reads with; a
 * caller that gives less is refused when the streams do not fit, never
 * written past. md_declassify is a read plus this; src/edit.c calls it for
 * `fixups set classic` against the image it writes once, itself, after the
 * last statement.
 *
 * Returns the same codes as md_declassify, tested by name the same way, and
 * prints the same messages -- except that it has no file to fail to read, so
 * MDCL_ERROR here only ever means an opcode-buffer allocation failed. On
 * MDCL_CONVERTED *out_len is the converted image's length (at most `cap`); on
 * MDCL_PASSTHROUGH it is `fsize`, the image unchanged. On a negative return
 * *out_len is untouched and the buffer's contents are unspecified -- the
 * chain walk rewrites fixup slots in place before a later refusal can fire --
 * so a caller must discard it, never write it.
 *
 * It never allocates or frees `buf`: the caller owns it throughout.
 *
 * `rep`, if non-NULL, receives what the conversion did (md_report) on
 * MDCL_CONVERTED, and is left alone on every other return -- a pass-through
 * did nothing to report, and a refusal's figures describe no output.
 * md_declassify passes NULL.
 */
int md_declassify_buf(uint8_t *buf, size_t fsize, size_t cap, size_t *out_len,
                      md_report *rep);

#endif /* MACHOTOOL_DECLASSIFY_H */
