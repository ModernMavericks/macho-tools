/*
 * Convert a Mach-O dylib from chained fixups format (macOS 12+)
 * to traditional LC_DYLD_INFO_ONLY format (macOS 10.6+).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include "image.h"
#include "mach_compat.h"

/* Chained fixups structures (not in 10.9 headers) */
struct cf_header {
    uint32_t fixups_version;
    uint32_t starts_offset;
    uint32_t imports_offset;
    uint32_t symbols_offset;
    uint32_t imports_count;
    uint32_t imports_format;
    uint32_t symbols_format;
};

struct cf_starts_image {
    uint32_t seg_count;
    uint32_t seg_info_offset[];
};

struct cf_starts_seg {
    uint32_t size;
    uint16_t page_size;
    uint16_t pointer_format;
    uint64_t segment_offset;
    uint32_t max_valid_pointer;
    uint16_t page_count;
    uint16_t page_start[];
};

struct cf_import {
    uint32_t bits; /* lib_ordinal:8, weak_import:1, name_offset:23 */
};

#define CF_PTR_64        2
#define CF_PTR_64_OFFSET 6
#define CF_START_NONE    0xFFFF

/* Dynamic buffer for opcodes */
struct opbuf {
    uint8_t *data;
    size_t len, cap;
};

static void ob_init(struct opbuf *b) { b->data = malloc(1024*1024); b->len = 0; b->cap = 1024*1024; }
static void ob_byte(struct opbuf *b, uint8_t v) { b->data[b->len++] = v; }
static void ob_uleb(struct opbuf *b, uint64_t v) {
    do { uint8_t byte = v & 0x7F; v >>= 7; if (v) byte |= 0x80; ob_byte(b, byte); } while (v);
}
static void ob_str(struct opbuf *b, const char *s) {
    while (*s) ob_byte(b, *s++);
    ob_byte(b, 0);
}

/* mi_each_lc's collecting-walk context for the pass below: gathers every
 * LC_SEGMENT_64 (needed afterward to translate a chained-fixups segment
 * index into a file offset), locates LC_DYLD_EXPORTS_TRIE/LC_DYLD_CHAINED_
 * FIXUPS/LC_DYLD_INFO_ONLY/LC_BUILD_VERSION, and records which commands the
 * rest of this tool will strip. Read-only w.r.t. the chain itself (never
 * touches lc->cmd, lc->cmdsize, or ncmds) except for the early stops below --
 * both of the fixed-size arrays here (segs[32], to_remove[16]) refuse rather
 * than overflow when a malformed or pathological input would fill them past
 * capacity, which is exactly what the stop-capable mi_each_lc exists for.
 *
 * to_remove[16], not [4]: an ORDINARY modern binary carries at most one each
 * of LC_DYLD_EXPORTS_TRIE/LC_DYLD_CHAINED_FIXUPS/LC_BUILD_VERSION (3 total),
 * but a ZIPPERED (Mac Catalyst) binary carries TWO LC_BUILD_VERSION commands
 * -- one per platform -- for 1+1+2 = 4. patch_macho's entire purpose is
 * converting modern-toolchain binaries for 10.9, so a zippered binary is
 * squarely in its real input class, not a pathological one; a cap of exactly
 * 4 would refuse it with zero margin. 16 keeps the refusal for what it is
 * actually for -- a genuinely absurd file -- without sitting one command away
 * from a legitimate one. */
struct pm_collect_ctx {
    struct segment_command_64 *segs[32];
    int nsegs;
    uint32_t exports_off, exports_size;
    uint32_t fixups_off, fixups_size;
    int has_dyld_info_only;
    struct { uint8_t *pos; uint32_t size; } to_remove[16];
    int n_remove;
};

/* Push one command onto ctx->to_remove, refusing rather than overflowing the
 * fixed-size array if there is no room. All three LC_DYLD_EXPORTS_TRIE/
 * LC_DYLD_CHAINED_FIXUPS/LC_BUILD_VERSION call sites below route through
 * here rather than each writing `ctx->to_remove[ctx->n_remove++] = ...`
 * with its own copy of the bound check -- three independent copies drifting
 * apart over time is exactly the "two places independently decide one
 * thing" bug class this conversion exists to retire, and it is also the
 * shape of bug that put this fix here in the first place: the nsegs >= 32
 * bound just above was applied to ONE fixed-size array in this struct;
 * to_remove[], ten lines away in the same struct, was moved here without
 * the matching treatment. n_remove is declared immediately after to_remove[]
 * -- when the array was still sized [4], an unbounded 5th push wrote
 * element [4], one past the array, directly over n_remove itself (the low
 * bits of a heap pointer, since the write reads as a pointer-sized `pos`
 * first), and every following write then walked off the struct into
 * main()'s locals. A hand-built fixture with 5+ LC_BUILD_VERSION commands
 * reproduced this as "pointer being freed was not allocated" (n_remove
 * corrupted to something that still looked small) or SIGSEGV (corrupted to
 * something that didn't); see tests/leaf-tool-crashes.sh (which now targets
 * the current [16] boundary, not the original [4] one this incident found
 * it at). Returns 1 (stop the walk) on overflow, 0 on success. */
static int pm_remove_push(struct pm_collect_ctx *ctx, uint8_t *pos, uint32_t size) {
    int cap = (int)(sizeof ctx->to_remove / sizeof ctx->to_remove[0]);
    if (ctx->n_remove >= cap) {
        fprintf(stderr, "ERROR: more than %d load commands to strip (LC_DYLD_EXPORTS_TRIE/"
                        "LC_DYLD_CHAINED_FIXUPS/LC_BUILD_VERSION); refusing rather than "
                        "overflowing the removal table\n", cap);
        return 1;
    }
    ctx->to_remove[ctx->n_remove++] = (typeof(ctx->to_remove[0])){pos, size};
    return 0;
}

static int pm_collect_lc(const struct load_command *lc_, void *ctx_) {
    struct pm_collect_ctx *ctx = ctx_;
    /* Cast away const as rename_segment.c's rs_rename_lc and retag_swift_
     * classes.c's find_section_lc do -- see image.h's contract comment.
     * segs[] keeps a MUTABLE pointer because the fixups-translation pass
     * further down writes through segs[si] (a segment's file data, not its
     * load command). */
    struct load_command *lc = (struct load_command *)lc_;
    if (lc->cmd == LC_SEGMENT_64) {
        struct segment_command_64 *seg = (struct segment_command_64 *)lc;
        /* Refuse rather than guess (the global rule -grow's own comment
         * states): silently dropping a 33rd segment here would leave si
         * (this segment's index into segs[]) referring to the WRONG segment
         * for every chained-fixups entry from here on, an unnoticed
         * reordering of which fixups apply to which segment. No 10.9-era
         * binary plausibly has this many segments; refusing costs nothing
         * real. Returning 1 stops mi_each_lc immediately -- the caller must
         * not trust ctx beyond this point, same as build_lcs's early exits
         * in change_dylib.c. */
        if (ctx->nsegs >= 32) {
            fprintf(stderr, "ERROR: more than 32 LC_SEGMENT_64 commands; refusing "
                            "rather than silently dropping one from the "
                            "chained-fixups translation\n");
            return 1;
        }
        ctx->segs[ctx->nsegs++] = seg;
    } else if (lc->cmd == LC_DYLD_EXPORTS_TRIE) {
        uint32_t *d = (uint32_t *)lc;
        ctx->exports_off = d[2]; ctx->exports_size = d[3];
        if (pm_remove_push(ctx, (uint8_t *)lc, lc->cmdsize)) return 1;
        printf("Exports trie: off=%u size=%u\n", ctx->exports_off, ctx->exports_size);
    } else if (lc->cmd == LC_DYLD_CHAINED_FIXUPS) {
        uint32_t *d = (uint32_t *)lc;
        ctx->fixups_off = d[2]; ctx->fixups_size = d[3];
        if (pm_remove_push(ctx, (uint8_t *)lc, lc->cmdsize)) return 1;
        printf("Chained fixups: off=%u size=%u\n", ctx->fixups_off, ctx->fixups_size);
    } else if (lc->cmd == LC_DYLD_INFO_ONLY) {
        ctx->has_dyld_info_only = 1;
    } else if (lc->cmd == LC_BUILD_VERSION) {
        if (pm_remove_push(ctx, (uint8_t *)lc, lc->cmdsize)) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "Usage: %s input output\n", argv[0]); return 1; }

    /* Read file. The 2MB of slack is this tool's own requirement: it appends the
     * rebuilt rebase/bind streams into the tail of the buffer rather than
     * reallocating, so the headroom has to be there from the start. That is why
     * mi_open alone could not serve this caller. */
    mi_image im;
    if (mi_open_slack(argv[1], 2*1024*1024, &im) != 0) {
        fprintf(stderr, "%s: not a readable 64-bit Mach-O\n", argv[1]);
        return 1;
    }
    size_t fsize = im.size;
    struct mach_header_64 *hdr = im.hdr;

    /* __TEXT's vmaddr: the pre-slide base. Was a strcmp inside the walk below;
     * segname is a char[16] that need not be NUL-terminated, so strcmp could run
     * off the end of a 16-character name. mi_find_segment compares against the
     * field width instead. */
    struct segment_command_64 *text_seg = mi_find_segment(&im, "__TEXT");
    uint64_t image_base_vmaddr = text_seg ? text_seg->vmaddr : 0;

    /* Collect segments and find special load commands. Walked via mi_each_lc
     * while `im` still owns the buffer -- mi_release happens right after,
     * once the walk (and its one early-stop refusal) is done. */
    struct pm_collect_ctx cctx;
    memset(&cctx, 0, sizeof cctx);
    if (!mi_each_lc(&im, pm_collect_lc, &cctx)) {
        /* pm_collect_lc already printed why; im still owns buf here. */
        mi_close(&im);
        return 1;
    }
    struct segment_command_64 **segs = cctx.segs;
    int nsegs = cctx.nsegs;
    uint32_t exports_off = cctx.exports_off, exports_size = cctx.exports_size;
    /* fixups_size is read inside pm_collect_lc's own diagnostic printf and
     * nowhere after -- no local copy here, else it would be a genuinely new
     * "set but not used" warning this conversion introduced. */
    uint32_t fixups_off = cctx.fixups_off;
    int has_dyld_info_only = cctx.has_dyld_info_only;
    typeof(cctx.to_remove) to_remove;
    memcpy(to_remove, cctx.to_remove, sizeof to_remove);
    int n_remove = cctx.n_remove;

    /* mi_release, not the image, owns the buffer from here: the rest of this
     * tool indexes the buffer directly and eventually free()s it (twice below,
     * on the two exit paths), which would leave an mi_image dangling -- and
     * mi_close would double-free -- if it still thought it owned the memory.
     * The hand-off is explicit, as in change_dylib.c. */
    uint8_t *buf = mi_release(&im);

    /* Idempotency: a binary that already has LC_DYLD_INFO_ONLY and no chained
     * fixups has been through this tool before (or never needed patching),
     * and re-running the conversion would error out on the missing fixups.
     * Pass the input through unchanged so driver scripts can invoke this
     * tool safely on an already-converted binary. */
    if (!fixups_off && has_dyld_info_only) {
        printf("Already patched (LC_DYLD_INFO_ONLY present, no chained fixups) — passing through.\n");
        int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0755);
        if (fd < 0) { perror("create output"); return 1; }
        if (write(fd, buf, fsize) != (ssize_t)fsize) { perror("write"); close(fd); return 1; }
        close(fd);
        free(buf);
        return 0;
    }
    if (!fixups_off) { fprintf(stderr, "No chained fixups found\n"); free(buf); return 1; }
    printf("Found %d segments\n", nsegs);

    /* Parse chained fixups */
    struct cf_header *cfh = (struct cf_header *)(buf + fixups_off);
    struct cf_starts_image *csi = (struct cf_starts_image *)(buf + fixups_off + cfh->starts_offset);
    struct cf_import *imports = (struct cf_import *)(buf + fixups_off + cfh->imports_offset);
    char *sympool = (char *)(buf + fixups_off + cfh->symbols_offset);

    printf("Fixups v%u: %u imports, %u segs in starts\n",
           cfh->fixups_version, cfh->imports_count, csi->seg_count);

    /* Generate rebase and bind opcodes */
    struct opbuf rebase, bind;
    ob_init(&rebase); ob_init(&bind);
    ob_byte(&rebase, REBASE_OPCODE_SET_TYPE_IMM | REBASE_TYPE_POINTER);
    ob_byte(&bind, BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER);

    int total_rebases = 0, total_binds = 0;

    for (uint32_t si = 0; si < csi->seg_count && si < (uint32_t)nsegs; si++) {
        if (csi->seg_info_offset[si] == 0) continue;
        struct cf_starts_seg *ss = (struct cf_starts_seg *)((uint8_t *)csi + csi->seg_info_offset[si]);
        printf("  Seg %u (%s): fmt=%u pages=%u segoff=0x%llx\n",
               si, segs[si]->segname, ss->pointer_format, ss->page_count, ss->segment_offset);

        for (uint16_t pg = 0; pg < ss->page_count; pg++) {
            uint16_t ps = ss->page_start[pg];
            if (ps == CF_START_NONE) continue;

            uint64_t off_in_seg = (uint64_t)pg * ss->page_size + ps;

            while (1) {
                uint64_t file_pos = segs[si]->fileoff + off_in_seg;
                if (file_pos + 8 > fsize) {
                    fprintf(stderr, "Fixup out of bounds at seg %u off 0x%llx\n", si, off_in_seg);
                    break;
                }
                uint64_t *ptr = (uint64_t *)(buf + file_pos);
                uint64_t raw = *ptr;
                int is_bind = (raw >> 63) & 1;
                uint32_t next;

                if (ss->pointer_format == CF_PTR_64 || ss->pointer_format == CF_PTR_64_OFFSET) {
                    /* Both formats: next is 12 bits at position 51, stride 4 */
                    next = ((raw >> 51) & 0xFFF) * 4;
                } else {
                    fprintf(stderr, "Unknown ptr format %u\n", ss->pointer_format);
                    return 1;
                }

                if (is_bind) {
                    uint32_t ordinal = raw & 0xFFFFFF;
                    if (ordinal >= cfh->imports_count) {
                        fprintf(stderr, "Bad ordinal %u\n", ordinal);
                        break;
                    }
                    uint32_t bits = imports[ordinal].bits;
                    int lib_ord = bits & 0xFF;
                    /* Handle signed 8-bit lib ordinal */
                    if (lib_ord > 127) lib_ord -= 256;
                    int weak = (bits >> 8) & 1;
                    uint32_t name_off = bits >> 9;
                    char *sym = sympool + name_off;

                    /* Emit bind opcodes. dyld in macOS 10.9 only understands
                     * special ordinals 0 (self), -1 (main exec), -2 (flat).
                     * -3 (weak lookup) is rejected with "bad special ordinal",
                     * so remap to flat lookup + weak-import flag. */
                    int effective_weak = weak;
                    int effective_ord = lib_ord;
                    if (lib_ord == -3) {
                        effective_ord = -2;
                        effective_weak = 1;
                    }
                    if (effective_ord < 0) {
                        ob_byte(&bind, BIND_OPCODE_SET_DYLIB_SPECIAL_IMM | (effective_ord & 0x0F));
                    } else if (effective_ord < 16) {
                        ob_byte(&bind, BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | effective_ord);
                    } else {
                        ob_byte(&bind, BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB);
                        ob_uleb(&bind, (uint64_t)effective_ord);
                    }
                    ob_byte(&bind, BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM | (effective_weak ? BIND_SYMBOL_FLAGS_WEAK_IMPORT : 0));
                    ob_str(&bind, sym);
                    ob_byte(&bind, BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | si);
                    ob_uleb(&bind, off_in_seg);
                    ob_byte(&bind, BIND_OPCODE_DO_BIND);

                    *ptr = 0; /* dyld will fill in */
                    total_binds++;
                } else {
                    /* Rebase. CF_PTR_64 stores a full VM address (with 8 high
                     * bits packed in the chain). CF_PTR_64_OFFSET stores an
                     * offset from image base — we have to add image_base so the
                     * classic REBASE (which adds slide, not slide+image_base)
                     * ends up at the right place. */
                    uint64_t target;
                    if (ss->pointer_format == CF_PTR_64) {
                        target = raw & 0x7FFFFFFFFFF; /* bits [42:0] */
                        uint8_t high8 = (raw >> 43) & 0xFF;
                        target |= (uint64_t)high8 << 56;
                    } else { /* CF_PTR_64_OFFSET */
                        target = (raw & 0xFFFFFFFFF) + image_base_vmaddr;
                    }

                    ob_byte(&rebase, REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | si);
                    ob_uleb(&rebase, off_in_seg);
                    ob_byte(&rebase, REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1);

                    *ptr = target;
                    total_rebases++;
                }

                if (next == 0) break;
                off_in_seg += next;
            }
        }
    }

    ob_byte(&rebase, REBASE_OPCODE_DONE);
    ob_byte(&bind, BIND_OPCODE_DONE);
    printf("Processed %d rebases, %d binds\n", total_rebases, total_binds);

    /* Remove old commands from the header (process in reverse order to maintain positions) */
    /* Sort by position descending */
    for (int i = 0; i < n_remove - 1; i++) {
        for (int j = i + 1; j < n_remove; j++) {
            if (to_remove[j].pos > to_remove[i].pos) {
                typeof(to_remove[0]) tmp = to_remove[i];
                to_remove[i] = to_remove[j];
                to_remove[j] = tmp;
            }
        }
    }

    uint8_t *lcmds_end = buf + sizeof(struct mach_header_64) + hdr->sizeofcmds;
    for (int i = 0; i < n_remove; i++) {
        uint8_t *pos = to_remove[i].pos;
        uint32_t sz = to_remove[i].size;
        size_t tail = lcmds_end - (pos + sz);
        memmove(pos, pos + sz, tail);
        lcmds_end -= sz;
        hdr->ncmds--;
        hdr->sizeofcmds -= sz;
        printf("Removed cmd at %ld (size %u)\n", pos - buf, sz);
    }

    /* Add LC_DYLD_INFO_ONLY */
    /* Append data at end of file (aligned) */
    size_t new_end = (fsize + 7) & ~7UL;
    uint32_t rebase_foff = (uint32_t)new_end;
    memcpy(buf + new_end, rebase.data, rebase.len);
    new_end += rebase.len;
    new_end = (new_end + 7) & ~7UL;
    uint32_t bind_foff = (uint32_t)new_end;
    memcpy(buf + new_end, bind.data, bind.len);
    new_end += bind.len;

    /* Check space for new load command.
     * __TEXT segment starts at fileoff=0 (includes header), but actual section
     * data starts much later. Find first section offset. */
    uint32_t first_sect_off = 0;
    for (int i = 0; i < nsegs; i++) {
        struct section_64 *sect = (struct section_64 *)((uint8_t *)segs[i] + sizeof(struct segment_command_64));
        for (uint32_t j = 0; j < segs[i]->nsects; j++) {
            if (sect[j].offset > 0 && (first_sect_off == 0 || sect[j].offset < first_sect_off))
                first_sect_off = sect[j].offset;
        }
    }
    if (first_sect_off == 0) first_sect_off = 4096; /* fallback */
    uint8_t *first_data = buf + first_sect_off;
    if (lcmds_end + 48 > first_data) {
        fprintf(stderr, "ERROR: No room for LC_DYLD_INFO_ONLY (need 48 bytes, have %ld)\n",
                first_data - lcmds_end);
        return 1;
    }

    struct dyld_info_command *di = (struct dyld_info_command *)lcmds_end;
    memset(di, 0, 48);
    di->cmd = LC_DYLD_INFO_ONLY;
    di->cmdsize = 48;
    di->rebase_off = rebase_foff;
    di->rebase_size = (uint32_t)rebase.len;
    di->bind_off = bind_foff;
    di->bind_size = (uint32_t)bind.len;
    di->export_off = exports_off;
    di->export_size = exports_size;
    hdr->ncmds++;
    hdr->sizeofcmds += 48;

    printf("Added LC_DYLD_INFO_ONLY: rebase=%u+%zu bind=%u+%zu exports=%u+%u\n",
           rebase_foff, rebase.len, bind_foff, bind.len, exports_off, exports_size);

    /* Extend __LINKEDIT segment to cover the appended opcode data — dyld only
     * accesses file ranges declared by some segment, and our new data lives
     * past the old __LINKEDIT end. */
    struct segment_command_64 *linkedit = NULL;
    for (int i = 0; i < nsegs; i++) {
        if (strcmp(segs[i]->segname, "__LINKEDIT") == 0) { linkedit = segs[i]; break; }
    }
    if (!linkedit) { fprintf(stderr, "No __LINKEDIT segment found\n"); return 1; }
    uint64_t needed_end = new_end;
    uint64_t new_filesize = needed_end - linkedit->fileoff;
    uint64_t new_vmsize = (new_filesize + 0xFFF) & ~0xFFFUL;
    if (new_filesize > linkedit->filesize) {
        printf("Extending __LINKEDIT: filesize %llu -> %llu, vmsize %llu -> %llu\n",
               linkedit->filesize, new_filesize, linkedit->vmsize, new_vmsize);
        linkedit->filesize = new_filesize;
        linkedit->vmsize = new_vmsize;
    }

    /* Write output */
    int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) { perror("create output"); return 1; }
    write(fd, buf, new_end);
    close(fd);
    printf("Wrote %s (%zu bytes)\n", argv[2], new_end);

    free(rebase.data); free(bind.data); free(buf);
    return 0;
}
