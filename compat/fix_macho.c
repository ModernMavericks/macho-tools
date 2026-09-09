/*
 * fix_macho - modify dylib paths and strip LC_BUILD_VERSION in Mach-O files.
 * Works with both thin and fat (universal) binaries.
 * Usage: fix_macho <file> [operations...]
 *   -change <old> <new>    Change a dylib path
 *   -strip_build_version   Remove LC_BUILD_VERSION commands
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include "fat.h"
#include "image.h"
#include "ordinals.h"
#include "mach_compat.h"

struct change_entry {
    const char *old_path;
    const char *new_path;
};

struct rename_seg_entry {
    const char *old_name;
    const char *new_name;
};

static int process_macho(uint8_t *buf, size_t size, struct change_entry *changes,
                         int nchanges, int strip_bv,
                         struct rename_seg_entry *renames, int nrenames) {
    /* `size` used to be accepted and never read: only the magic was checked,
     * with no bound on sizeofcmds or any individual cmdsize, and
     * dylib.name.offset was trusted outright. On a malformed or truncated
     * slice that let the -strip_build_version memmove/memset below (and the
     * -change path's name write) run past the end of `buf`. mi_wrap runs the
     * same load-command validation every other tool in this codebase relies
     * on (magic, sizeofcmds/cmdsize bounds, LC_SEGMENT_64's nsects actually
     * fitting its cmdsize) against this exact `size` -- so every offset this
     * function trusts afterward has already been proven to fit. */
    mi_image im;
    if (mi_wrap(buf, size, &im) != 0) {
        fprintf(stderr, "  Not a valid 64-bit Mach-O (bad magic, load commands don't fit "
                        "the %zu-byte slice, or a malformed LC_SEGMENT_64)\n", size);
        return -1;
    }
    struct mach_header_64 *hdr = im.hdr;

    /* Left as a hand-rolled walk, deliberately, even with a stop-capable
     * mi_each_lc now available (src/image.h): the -strip_build_version
     * branch below MUTATES the chain mid-walk -- memmove's a later command's
     * bytes down over the one being dropped, memset's the vacated tail,
     * shrinks hdr->ncmds and hdr->sizeofcmds, and then `continue`s WITHOUT
     * advancing lcp, so the command that just slid into this position gets
     * re-examined at the same cursor rather than skipped. That is exactly
     * the shape mi_each_lc's contract (see image.h) forbids a callback from
     * producing: it walks by re-reading `im->hdr->ncmds` once before the
     * loop and unconditionally striding `p += lc->cmdsize` after every call,
     * so a callback that shrinks ncmds or leaves the cursor where it was
     * desyncs that stride from the buffer's real shape.
     *
     * A mutating variant COULD be built, but its contract would be a
     * different, harder one -- not "may stop the walk" but "must recompute
     * ncmds and lcend after every call and know whether to advance the
     * cursor or retry it," pushing the stride logic this module exists to
     * centralize back onto every caller of that variant. This file has
     * exactly one walk that needs it. Building a second iterator shape for
     * one caller reproduces, for the walk itself, the "two places
     * independently deciding one thing" bug class this whole conversion
     * effort exists to retire -- so this walk stays hand-rolled instead.
     * Revisit only if a second mutating caller shows up and the shared shape
     * becomes clear from two real examples rather than guessed from one. */
    uint8_t *lcp = buf + sizeof(struct mach_header_64);
    uint8_t *lcend = lcp + hdr->sizeofcmds;
    int modified = 0;

    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;

        if (strip_bv && lc->cmd == LC_BUILD_VERSION) {
            /* Remove by converting to padding (set to zero-filled) */
            uint32_t sz = lc->cmdsize;
            size_t tail = lcend - (lcp + sz);
            memmove(lcp, lcp + sz, tail);
            memset(lcend - sz, 0, sz);
            lcend -= sz;
            hdr->ncmds--;
            hdr->sizeofcmds -= sz;
            modified = 1;
            printf("  Removed LC_BUILD_VERSION (%u bytes)\n", sz);
            continue; /* Don't advance lcp, re-check at same position */
        }

        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *seg = (struct segment_command_64 *)lcp;
            for (int r = 0; r < nrenames; r++) {
                if (strncmp(seg->segname, renames[r].old_name, 16) == 0) {
                    printf("  Renamed segment '%s' -> '%s'\n", renames[r].old_name, renames[r].new_name);
                    memset(seg->segname, 0, 16);
                    strncpy(seg->segname, renames[r].new_name, 16);
                    /* Also rename sections within this segment */
                    struct section_64 *sect = (struct section_64 *)(lcp + sizeof(struct segment_command_64));
                    for (uint32_t s = 0; s < seg->nsects; s++) {
                        memset(sect[s].segname, 0, 16);
                        strncpy(sect[s].segname, renames[r].new_name, 16);
                    }
                    modified = 1;
                    break;
                }
            }
        }

        /* mo_is_ordinal_lc() here (rather than a locally re-listed set) is
         * what keeps this "which dylib LCs -change can rewrite" set in sync
         * with change_dylib.c's identical predicate at process_lc (compat/
         * change_dylib.c) -- they used to disagree about
         * LC_LOAD_UPWARD_DYLIB: fix_macho hand-listed {LOAD, WEAK, ID,
         * REEXPORT} and silently left an upward dylib's path unrewritten
         * (reporting "No changes needed", exit 0) where change_dylib
         * rewrote it -- two answers to the same question. LC_ID_DYLIB is
         * added back in for the same reason change_dylib.c adds it: it
         * names the image itself, so it must be recognized as dylib-shaped
         * for `dc`/`name` below to be valid, even though nothing in
         * `changes` is ever meant to match it. */
        if (mo_is_ordinal_lc(lc->cmd) || lc->cmd == LC_ID_DYLIB) {
            struct dylib_command *dc = (struct dylib_command *)lcp;
            char *name = (char *)mo_lc_str_at((const struct load_command *)lcp, dc->dylib.name.offset);
            if (!name) {
                fprintf(stderr, "  ERROR: malformed dylib load command (name offset %u "
                                "exceeds cmdsize %u)\n", dc->dylib.name.offset, dc->cmdsize);
                return -1;
            }

            for (int c = 0; c < nchanges; c++) {
                if (strcmp(name, changes[c].old_path) == 0) {
                    size_t new_len = strlen(changes[c].new_path);
                    /* Check there's room in the existing command */
                    size_t name_off = dc->dylib.name.offset;
                    size_t available = dc->cmdsize - name_off;
                    if (new_len + 1 > available) {
                        fprintf(stderr, "  ERROR: new path '%s' too long (%zu > %zu)\n",
                                changes[c].new_path, new_len + 1, available);
                        return -1;
                    }
                    memset(name, 0, available);
                    memcpy(name, changes[c].new_path, new_len);
                    printf("  Changed: %s -> %s\n", changes[c].old_path, changes[c].new_path);
                    modified = 1;
                    break;
                }
            }
        }

        lcp += lc->cmdsize;
    }
    return modified;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <file> [-change old new] [-strip_build_version]\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];

    /* Parse operations */
    struct change_entry changes[32];
    int nchanges = 0;
    int strip_bv = 0;
    struct rename_seg_entry renames[16];
    int nrenames = 0;

    for (int i = 2; i < argc; ) {
        if (strcmp(argv[i], "-change") == 0 && i + 2 < argc) {
            changes[nchanges].old_path = argv[i+1];
            changes[nchanges].new_path = argv[i+2];
            nchanges++;
            i += 3;
        } else if (strcmp(argv[i], "-strip_build_version") == 0) {
            strip_bv = 1;
            i++;
        } else if (strcmp(argv[i], "-rename_seg") == 0 && i + 2 < argc) {
            /* segname is char[16]; compat/rename_segment.c:81 refuses a
             * newname longer than that outright. Before this check,
             * process_macho's strncpy(seg->segname, ..., 16) below silently
             * truncated instead -- same operation, two disagreeing
             * implementations. Refuse here too, before any file I/O, same
             * as rename_segment does. */
            if (strlen(argv[i+2]) > 16) {
                fprintf(stderr, "new segment name longer than 16 bytes: %s\n", argv[i+2]);
                return 1;
            }
            renames[nrenames].old_name = argv[i+1];
            renames[nrenames].new_name = argv[i+2];
            nrenames++;
            i += 3;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    /* Read file */
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st;
    fstat(fd, &st);
    size_t fsize = st.st_size;

    uint8_t *buf = malloc(fsize);
    if (read(fd, buf, fsize) != (ssize_t)fsize) { perror("read"); return 1; }

    int modified = 0;
    uint32_t magic = *(uint32_t *)buf;

    if (magic == FAT_CIGAM || magic == FAT_MAGIC) {
        /* Fat binary - process each slice. mfat_parse (src/fat.c) is the ONE
         * place both this tool and change_dylib validate a fat file's arch
         * table now -- this used to trust `offset`/`asize` from the file
         * outright and index buf+offset with them unchecked, an
         * out-of-bounds READ on a malformed or hostile fat file. A failed
         * parse now refuses the whole file instead of reading past the end
         * of `buf`. mfat_parse also refuses two declared slices that overlap
         * EACH OTHER, not just ones that run past the file or into the
         * header -- a fat file whose own arch table already aliases two
         * slices is malformed input, on the read side, regardless of what a
         * tool does with it. */
        uint32_t narch; int swap;
        if (mfat_parse(buf, fsize, &narch, &swap) != 0) {
            fprintf(stderr, "Malformed fat file (bad magic, arch table past the end, "
                            "a slice overlapping the header, or two slices overlapping "
                            "each other)\n");
            close(fd);
            free(buf);
            return 1;
        }

        for (uint32_t i = 0; i < narch; i++) {
            mfat_arch a;
            mfat_get(buf, swap, i, &a);
            printf("Processing arch %u at offset %u:\n", i, a.offset);
            int r = process_macho(buf + a.offset, a.size, changes, nchanges, strip_bv, renames, nrenames);
            if (r > 0) modified = 1;
            else if (r < 0) { fprintf(stderr, "  Skipping arch %u\n", i); }
        }
    } else if (magic == MH_MAGIC_64) {
        printf("Processing thin Mach-O:\n");
        int r = process_macho(buf, fsize, changes, nchanges, strip_bv, renames, nrenames);
        if (r > 0) modified = 1;
        else if (r < 0) {
            /* Unlike the fat loop above (multiple slices, where one this tool
             * can't handle is tolerable as long as the others still get
             * processed), a thin file has exactly one slice: if
             * process_macho refused it, there is nothing left to do but
             * report failure. Before this fix the return value was simply
             * dropped here, so a real refusal (e.g. process_macho's own
             * "malformed dylib load command" or "new path too long" errors,
             * already printed to stderr) still exited 0 with "No changes
             * needed" -- a refusal that looked like success to any caller
             * checking only the exit code. */
            close(fd);
            free(buf);
            return 1;
        }
    } else if (magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64) {
        /* Genuinely a Mach-O (a 64-bit fat container, fat_arch_64 -- wide
         * offsets, used for arm64e/watchOS-style slices); this tool just
         * doesn't speak that variant. Say so, rather than the generic "not a
         * Mach-O file" below, which reads as "this isn't Mach-O at all". */
        fprintf(stderr, "64-bit fat Mach-O (fat_arch_64); not supported -- only the "
                        "32-bit-offset fat_arch container is (magic=0x%x)\n", magic);
        close(fd);
        return 1;
    } else {
        fprintf(stderr, "Not a Mach-O file (magic=0x%x)\n", magic);
        close(fd);
        return 1;
    }

    if (modified) {
        lseek(fd, 0, SEEK_SET);
        if (write(fd, buf, fsize) != (ssize_t)fsize) { perror("write"); close(fd); return 1; }
        printf("File updated: %s\n", path);
    } else {
        /* NOTE for the compat-retirement plan: rename_segment.c:125 exits 2
         * ("nothing to do") when its rename matched no segment; this exits
         * 0 here for the identical "nothing this run's operations touched"
         * outcome, covering -change/-strip_build_version/-rename_seg alike.
         * Left alone deliberately per this wave's scope -- not the LC set
         * mismatch this wave fixed, just a second, separate exit-code
         * divergence between the two tools worth resolving when fix_macho
         * is retired behind macho9. */
        printf("No changes needed: %s\n", path);
    }

    close(fd);
    free(buf);
    return 0;
}
