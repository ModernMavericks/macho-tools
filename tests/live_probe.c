/*
 * tests/live_probe.c — the constraint-pinning fixture for src/live.h.
 *
 * This file #includes ONLY src/live.h -- nothing else. tests/live_test.c
 * compiles this to a standalone object with a plain `cc -c` and runs `nm`
 * over the result, asserting no reference to malloc/free/calloc/realloc or
 * any stdio symbol appears -- see live.h's own top comment for why that
 * matters: avxemu's SIGILL handler is async-signal-safe and cannot link
 * anything that might allocate or lock. Reading `nm`'s output here is
 * inspecting THIS test's own freshly-built object for symbol presence, not
 * parsing a tool's human-readable text as an oracle for Mach-O structure
 * (tests/README.md's lesson on that is about *.macho files, not *.o symbol
 * tables) -- exactly what live_test.c's own header comment says is fine.
 *
 * This is a persistent fixture, not a string generated at test time, so a
 * reviewer can see exactly what got compiled and linked. Every public
 * function in live.h is called from mlp_probe below, and mlp_probe itself
 * has external linkage (not `static`) so nothing here can be discarded as
 * unreferenced before its body -- and any calls INSIDE that body -- ever
 * reaches the object file's symbol table.
 */
#include "live.h"

int mlp_probe(const struct mach_header_64 *mh, intptr_t slide,
              const char *segname, const char *sectname);

static int mlp_stop_at_first(const struct load_command *lc, void *ctx) {
    (void)lc;
    (void)ctx;
    return 1; /* stop immediately -- just needs to be a valid callback */
}

int mlp_probe(const struct mach_header_64 *mh, intptr_t slide,
              const char *segname, const char *sectname) {
    int valid = mlive_valid(mh);
    const struct segment_command_64 *sg = mlive_find_segment(mh, segname);
    const struct section_64 *se = mlive_find_section(mh, segname, sectname);
    int walked = mlive_each_lc(mh, mlp_stop_at_first, NULL);
    uintptr_t addr = sg ? mlive_addr(sg->vmaddr, slide) : 0;

    return valid + (sg != NULL) + (se != NULL) + walked + (int)(addr != 0);
}
