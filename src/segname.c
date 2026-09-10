/*
 * mseg_ -- see segname.h. This is compat/rename_segment.c's former
 * rs_rename_lc, unchanged in what it does to a load command; only the driver
 * (open, walk, write back, decide an exit code) stayed behind in that tool.
 */
#include <string.h>
#include <stdint.h>
#include <mach-o/loader.h>

#include "segname.h"

int mseg_name_fits(const char *name) {
    return strlen(name) <= MSEG_NAME_MAX;
}

int mseg_rename_lc(struct load_command *lc, const char *oldname, const char *newname) {
    if (lc->cmd != LC_SEGMENT_64) return 0;
    struct segment_command_64 *seg = (struct segment_command_64 *)lc;
    if (strncmp(seg->segname, oldname, MSEG_NAME_MAX) != 0) return 0;

    /* memset-then-strncpy, not strncpy alone: a name shorter than the one it
     * replaces must not leave the old name's tail bytes behind, and a name of
     * exactly MSEG_NAME_MAX bytes must fill the field with no terminator.
     * strncpy gives both -- it pads with NULs and truncates at the field
     * width -- and the memset makes that explicit rather than incidental. */
    memset(seg->segname, 0, MSEG_NAME_MAX);
    strncpy(seg->segname, newname, MSEG_NAME_MAX);
    /* Each section repeats its segment's name; getsectiondata matches on the
     * section's copy, so it has to change too. */
    struct section_64 *sects = (struct section_64 *)(seg + 1);
    for (uint32_t s = 0; s < seg->nsects; s++) {
        memset(sects[s].segname, 0, MSEG_NAME_MAX);
        strncpy(sects[s].segname, newname, MSEG_NAME_MAX);
    }
    return 1;
}
