/*
 * macho_grow.h — make room in a Mach-O header so the load commands can expand.
 *
 * The problem: tools like change_dylib (and patch_macho's LC_DYLD_INFO_ONLY
 * insertion) write load commands in place, bounded by the file offset of the
 * first section's data. When the linker leaves little padding there (recent
 * Bun/JSC builds leave as few as 16 bytes), a longer dylib path or an extra
 * load command no longer fits.
 *
 * The fix, studied from LIEF (src/MachO/Binary.cpp `shift`) and llvm-objcopy
 * (MachOLayoutBuilder): make room by inserting page-aligned space after the
 * load commands. LIEF/llvm push every later segment to a HIGHER vm address and
 * then fix up everything that depended on those addresses — section-symbol
 * n_values, LC_MAIN, function-start deltas, relocations, rebase/bind/chained
 * targets. That is a lot of machinery — and we can't just run those tools on
 * 10.9 anyway: LIEF needs a modern-macOS C++ runtime and llvm-objcopy a
 * cross-built toolchain, while install_name_tool / optool / insert_dylib refuse
 * to grow the header at all. This header compiles with the stock 10.9 clang and
 * has no dependencies.
 *
 * We take a simpler, equivalent route available to any PIE executable with a
 * __PAGEZERO: instead of raising data, we LOWER the image base. We donate the
 * inserted bytes from __PAGEZERO and drop __TEXT's vmaddr by the same amount,
 * growing __TEXT's vm/file size. Net effect: every section and segment keeps
 * its ORIGINAL vm address, so no pointer, rebase, bind, n_value, or entry
 * address ever changes. The only fields that move are file offsets — which we
 * shift uniformly. (Borrowed from LIEF: the exhaustive list of offset fields.)
 *
 * Precondition: a MH_PIE executable with a __PAGEZERO at least `grow` bytes
 * large. (Always true for the Claude Code executable: 0x1_0000_0000 pagezero.)
 * Dylibs without a __PAGEZERO can't lower the base; mg_grow_header reports that
 * and leaves the buffer untouched so the caller can fall back / error cleanly.
 */
#ifndef MACHO9_GROW_H
#define MACHO9_GROW_H


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mach-o/loader.h>

/* ULEB128 decode / minlen / fixed-width encode. */
#include "uleb.h"

/* Validated open/wrap/iterate over a Mach-O buffer. */
#include "image.h"

/* Export-trie rebuild, for when an in-place re-encode (mg_trie_node, below)
 * can't absorb an address's widened ULEB. */
#include "trie.h"

/* Load-command/section-type constants newer than the 10.9 SDK headers. */
#include "mach_compat.h"

/* The __LINKEDIT offset-bump table: ml_bump and ml_bump_all, covering
 * LC_SYMTAB/LC_DYSYMTAB/LC_DYLD_INFO[_ONLY] and the linkedit_data_command
 * family. See src/linkedit.h for the exact field list. */
#include "linkedit.h"


#define MG_EXPORT_KIND_MASK        0x03
#define MG_EXPORT_REEXPORT         0x08
#define MG_EXPORT_STUB_AND_RESOLVER 0x10

#define MG_PAGE 0x1000UL

/* Lowest section file offset — this bounds the header pad. `fsize` is the
 * buffer's real size, wrapped through mi_wrap so this walk cannot stride past
 * it -- the bug class this whole extraction exists to prevent.
 *
 * Returns UINT32_MAX, with a message on stderr, if the buffer fails to wrap
 * (bad magic, or load commands that don't fit): refuse rather than guess. A
 * fixed fallback here would be a real hazard, not a theoretical one --
 * change_dylib.c's memset(buf + 32, 0, first_sect_off - 32) turns a wrong
 * guess directly into an out-of-bounds write. Every caller must check for
 * UINT32_MAX. This differs from the UINT32_MAX -> 4096 default a few lines
 * down, which is a validated image that simply has no sections -- a real,
 * if unusual, answer rather than a guess about an image we couldn't read. */
uint32_t mg_first_sect_off(const uint8_t *buf, size_t fsize);



/* Re-encode the leading (base-relative) LC_FUNCTION_STARTS delta after lowering
 * the image base by `grow`: delta[0] += grow, keeping the leading delta's byte
 * width so blob size is unchanged and the trailing deltas are untouched.
 * Returns: 1 patched in place; 0 the widened delta needs more bytes than the
 * original leading encoding (caller must refuse — LINKEDIT resize unsupported);
 * -1 malformed blob (empty / bad leading ULEB). */
int mg_reencode_funcstarts_base(uint8_t *blob, uint32_t size, uint32_t grow);


/* Decode the whole function-starts blob into absolute addresses given the image
 * base. Stops at a 0 delta (terminator/padding) or end. Returns count (<= max),
 * or -1 on a malformed ULEB. (Used by tests to assert the grow moved nothing.) */
int mg_funcstarts_decode(const uint8_t *blob, uint32_t size,
                                uint64_t base, uint64_t *out, int max);




/* Add `grow` to every entry of every S_INIT_FUNC_OFFSETS section, or with
 * patch=0 just verify the pass would be sound. Without this, dyld4-era static
 * constructors are called at (base - grow) + offset and jump into whatever
 * precedes them. Returns 0 ok, -1 malformed/unsafe. */
int mg_init_offsets_pass(uint8_t *buf, size_t fsize, uint32_t grow, int patch);


/* Walk the export trie looking for an exported address we would have to
 * re-encode. Addresses there are ULEB offsets from the image base; bumping one
 * can widen its encoding and force __LINKEDIT to be rebuilt, which this header
 * does not do. __mh_execute_header is exported at offset 0 and stays correct --
 * it names the header, which moved down with the base -- so a trie whose
 * addresses are all zero is safe to leave alone.
 * Returns 0 safe, 1 needs re-encoding, -1 malformed. */
int mg_trie_scan(const uint8_t *trie, uint32_t size, uint32_t off, int depth);



/* Not every base-relative address names a function. Initializers and
 * compact-unwind entries do; a data export, a jump-table range, an LSDA blob and
 * a personality GOT slot do not. Only MG_K_FUNC entries can be checked against
 * LC_FUNCTION_STARTS. */
#define MG_K_ANY  0
#define MG_K_FUNC 1

typedef struct { uint64_t *addr; uint32_t n; } mg_snapshot;

#define MG_SNAP_MAX 65536

int mg_collect(const uint8_t *buf, size_t fsize, uint64_t *out, uint8_t *kinds,
                      uint32_t max, uint32_t *n_out);


int mg_snapshot_take(const uint8_t *buf, size_t fsize, mg_snapshot *s);


void mg_snapshot_free(mg_snapshot *s);


/* 0 if every base-relative structure resolves exactly where it did before the
 * grow; -1 (with a message naming the first mismatch) otherwise. */
int mg_verify(const uint8_t *buf, size_t fsize, const mg_snapshot *before);


/* ---- __TEXT,__unwind_info -------------------------------------------------
 * Compact unwind stores several different things as 32-bit words, and only some
 * are measured from the image base. Getting that distinction wrong is silent:
 * the tables still parse, and only an actual unwind notices.
 *
 * MUST gain `grow` (offsets from the image base):
 *   - personality array entries (they address the routine's GOT slot)
 *   - first-level index functionOffset, INCLUDING the trailing sentinel
 *   - LSDA index entries: both functionOffset and lsdaOffset
 *   - regular (kind 2) second-level page entry functionOffset
 * MUST NOT be touched:
 *   - compressed (kind 3) second-level entries. Their low 24 bits are a delta
 *     from their own page's first-level functionOffset, which the bump above
 *     already moved, so they are correct untouched and corrupt if bumped.
 *   - every *SectionOffset field: those are offsets within this section.
 *   - common encodings: encodings, not addresses.
 *
 * One walker, three uses -- audit (patch=0), apply (patch=1), collect for verify
 * (out != NULL). Deliberately one function: the __init_offsets double-apply
 * happened because two functions encoded the same knowledge and both ran. */
int mg_uw_bump(uint8_t *p, uint32_t grow, int patch);


int mg_unwind_walk(uint8_t *buf, size_t fsize, uint32_t grow, int patch,
                          uint64_t base, uint64_t *out, uint8_t *kinds,
                          uint32_t *n, uint32_t max);


/* ---- LC_DATA_IN_CODE ------------------------------------------------------
 * A flat array of data_in_code_entry { uint32 offset; uint16 length; uint16 kind }.
 * ONLY `offset` is measured from the image base. `length` and `kind` are not
 * offsets at all, so a walker that bumps whole words instead of the first field
 * of each entry corrupts every range while still "changing by grow".
 * One walker, three uses, as for compact unwind. */
int mg_dice_walk(uint8_t *buf, size_t fsize, uint32_t grow, int patch,
                        uint64_t base, uint64_t *out, uint8_t *kinds,
                        uint32_t *n, uint32_t max);


/* ---- export trie ----------------------------------------------------------
 * Each exported address is a ULEB offset FROM THE IMAGE BASE, so lowering the
 * base means every one must gain `grow`. The reason this is safe to do in place:
 * adding a page never widens the encoding on a real binary. Measured across all
 * 670 entries of Claude Code 2.1.263 at 4K, 8K and 16K grows, zero needed a
 * wider ULEB and zero needed redundant padding. So each address is re-encoded at
 * its ORIGINAL byte width, the trie keeps its size, and no __LINKEDIT offset
 * moves. If one ever would widen, we refuse -- that is the case the old guard
 * was written for, and it is still handled, just no longer assumed.
 *
 * Address 0 stays 0. That is __mh_execute_header, which names the header itself;
 * the header moved down with the base, so 0 remains correct. It is therefore
 * neither bumped nor collected -- its resolved address is base+0, which SHOULD
 * change, and collecting it would make verify fail on a correct grow.
 *
 * `seen` guards a shared subtree from being bumped twice -- the same hazard as
 * the __init_offsets double-apply. */
int mg_trie_node(uint8_t *trie, uint32_t size, uint32_t off, int depth,
                        uint32_t grow, int patch, uint64_t base,
                        uint64_t *out, uint8_t *kinds, uint32_t *n, uint32_t max,
                        uint8_t *seen);


/* Locate the export trie's load command -- LC_DYLD_INFO, LC_DYLD_INFO_ONLY, or
 * LC_DYLD_EXPORTS_TRIE, whichever this image carries -- and return its BYTE
 * OFFSET from buf (not a pointer: a caller that goes on to realloc buf, as
 * the widen-append path does, needs an offset it can re-derive a pointer
 * from afterward, not a pointer the realloc may have invalidated). Returns 1
 * with *lc_off and *cmd set, or 0 if this image has no such load command (not an
 * error -- just nothing to walk).
 *
 * The single source of truth for "which load command carries the export
 * trie": mg_find_trie (below) and mg_grow_header's widen-append path both
 * call this rather than each re-scanning load commands on their own, so the
 * two can never disagree about which one it is. (They once could: an
 * earlier version had the append path re-scan without breaking on the first
 * match, landing on the LAST export-trie-shaped load command while this
 * function -- and mg_find_trie -- always meant the FIRST. A file carrying
 * both LC_DYLD_INFO_ONLY and LC_DYLD_EXPORTS_TRIE would have repointed the
 * wrong one. It failed safe -- mg_verify would see pre-shift addresses
 * through the untouched first LC and refuse -- but "two places
 * independently deciding the same thing" is exactly the bug shape this
 * whole toolkit plan exists to eliminate, so it is not left as a coincidence
 * that happens to agree today.) */
int mg_find_trie_lc(const uint8_t *buf, size_t fsize, long *lc_off, uint32_t *cmd);


/* Locate the export trie's (off, size), whichever load command carries it --
 * LC_DYLD_INFO[_ONLY]'s export_off/export_size, or LC_DYLD_EXPORTS_TRIE's
 * dataoff/datasize. Returns 1 with *off and *size set, or 0 if this image has
 * no export-trie load command at all (not an error -- just nothing to walk). */
int mg_find_trie(const uint8_t *buf, size_t fsize, uint32_t *off, uint32_t *size);


int mg_trie_walk(uint8_t *buf, size_t fsize, uint32_t grow, int patch,
                        uint64_t base, uint64_t *out, uint8_t *kinds,
                        uint32_t *n, uint32_t max);



int mg_classify(const uint8_t *buf, size_t fsize);


/* ---- plausibility: verification with no "before" to compare against --------
 * mg_verify is stronger, but it needs a snapshot taken before the transform.
 * The wrapper cannot have one: it checks the end state of a pipeline whose
 * earlier stages ran in other processes. This works from the finished file.
 *
 * The useful check is not "is this address inside __text" -- __text is 63 MB on
 * Claude Code, so a one-page error stays comfortably inside it. It is that
 * initializers and compact-unwind entries name FUNCTIONS, so their targets must
 * appear in LC_FUNCTION_STARTS. Measured on 2.1.263: 13/13 first-level, 198/198
 * LSDA and 9/9 initializers land exactly on one of 71,974 known starts.
 *
 * Without LC_FUNCTION_STARTS there is nothing to check against, so this passes
 * rather than refusing -- a weaker guarantee, honestly reported by returning 0. */
int mg_addr_known(const uint64_t *sorted, int n, uint64_t a);


int mg_plausible(const uint8_t *buf, size_t fsize);


/*
 * Grow the header pad by at least `grow_req` bytes (rounded up to a page).
 * pbuf is realloc'd, pfsize updated. Returns 0 on success, -1 if the
 * precondition (PIE-style __PAGEZERO large enough) isn't met — in which case
 * the buffer and size are left unchanged.
 */
int mg_grow_header(uint8_t **pbuf, size_t *pfsize, uint32_t grow_req);

#endif /* MACHO9_GROW_H */
