#ifndef MACHO9_DECLASSIFY_H
#define MACHO9_DECLASSIFY_H
/*
 * md_ -- lowering a modern Mach-O's CHAINED FIXUPS to the LC_DYLD_INFO_ONLY
 * rebase/bind opcode streams dyld has understood since 10.6, so a binary a
 * 2021-and-later toolchain produced can be loaded on 10.9 at all.
 *
 * This is compat/patch_macho.c's whole conversion, lifted out of that tool's
 * main() so it is a library function rather than a program. Two front-ends
 * call it now and must keep behaving identically: compat/patch_macho.c (the
 * old grammar, `patch_macho IN OUT`, its own open/write of OUT at 0755, its
 * "Wrote ..." line only on the converting path, exit 1 for everything that
 * goes wrong) and cli/macho9.c's `declassify` verb. Only what genuinely
 * differs between the two -- argument parsing, the write path, the exit code
 * -- stays in each front-end. Everything the conversion itself PRINTS lives
 * down here, once, exactly as patch_macho has always printed it, for the same
 * reason rewrite.h keeps change_dylib's diagnostics down there: two copies of
 * a message drift, and tests/characterize.sh and tests/chained-fixups.sh both
 * run the conversion through patch_macho.
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
#define MDCL_REFUSED     (-2)  /* examined and declined on purpose (no chained
                                * fixups to convert, more segments or strippable
                                * commands than the tables hold, an unknown
                                * pointer format, no room for the 48-byte
                                * LC_DYLD_INFO_ONLY, no __LINKEDIT); the reason
                                * is already on stderr */

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
 * On either negative return nothing is allocated and nothing is written; the
 * file on disk is never touched by this function in any case, since it only
 * ever reads.
 */
int md_declassify(const char *path, uint8_t **out_buf, size_t *out_len);

#endif /* MACHO9_DECLASSIFY_H */
