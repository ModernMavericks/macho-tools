#ifndef MACHO9_DECLASSIFY_H
#define MACHO9_DECLASSIFY_H
/*
 * md_ -- lowering a modern Mach-O's CHAINED FIXUPS to the LC_DYLD_INFO_ONLY
 * rebase/bind opcode streams dyld has understood since 10.6, so a binary a
 * 2021-and-later toolchain produced can be loaded on 10.9 at all.
 *
 * This is compat/patch_macho.c's whole conversion, lifted out of that tool's
 * main() so it is a library function rather than a program. cli/macho9.c's
 * `declassify` verb is the only C front-end left; the old grammar,
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
 * gets MDCL_NOT_MACHO, like anything else mi_open_slack will not open.
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
#define MDCL_ERROR       (-3)  /* an operational failure -- an allocation this
                                * conversion could not make -- not a judgement
                                * about the input; already reported. A caller
                                * that distinguishes refusal from failure (the
                                * `declassify` verb does) must NOT report this
                                * as a refusal */

/* LIMITS, and what happens at each -- every one of them is a refusal, never a
 * truncated or corrupted output. The conversion works inside two fixed
 * budgets, both of them deliberate: it appends into slack allocated with the
 * file rather than reallocating, and it emits into fixed opcode buffers.
 *
 *   32 LC_SEGMENT_64 commands, and 16 strippable ones
 *     (LC_DYLD_EXPORTS_TRIE/LC_DYLD_CHAINED_FIXUPS/LC_BUILD_VERSION together)
 *     -- more of either is MDCL_REFUSED.
 *   1MB of rebase opcodes and 1MB of bind opcodes, about 200k fixups each
 *     (roughly 5 bytes per fixup). A binary with more fixups than that is
 *     MDCL_REFUSED, not a binary whose remaining pointers silently go
 *     unrebased. This is the limit a very large modern binary reaches first.
 *   2MB of slack past the end of the file, which both finished streams plus
 *     their 8-byte alignment must fit inside -- MDCL_REFUSED otherwise.
 *   48 bytes of header pad for the new LC_DYLD_INFO_ONLY, and a __LINKEDIT
 *     segment to extend -- MDCL_REFUSED without either.
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

#endif /* MACHO9_DECLASSIFY_H */
