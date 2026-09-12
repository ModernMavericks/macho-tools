#ifndef MACHOTOOL_ARCH_NAMES_H
#define MACHOTOOL_ARCH_NAMES_H
/*
 * ma_ -- lipo's architecture names, and the cputype/cpusubtype each means.
 *
 * One table, so the `arch` directive, edit's messages, and anything else
 * that names a slice agree on what a name means. A subtype is compared with
 * its high byte masked off (CPU_SUBTYPE_MASK): that byte carries capability
 * bits (CPU_SUBTYPE_LIB64 on x86_64, pointer-auth ABI bits on arm64e) that
 * vary between otherwise identical slices.
 */
#include <stddef.h>
#include <stdint.h>

/* Row `r` of the table: sets *name, *cputype, *cpusubtype and returns 1, or
 * returns 0 when `r` is past the end. Rows are stable: the `arch` directive
 * records a set of names as a bitmask over them (src/script.h). */
int ma_row(int r, const char **name, uint32_t *cputype, uint32_t *cpusubtype);

/* The row a name means, or -1 for a name not in the table. */
int ma_lookup(const char *name);

/* The row a cputype/cpusubtype pair means, or -1. */
int ma_index(uint32_t cputype, uint32_t cpusubtype);

/* The name for a cputype/cpusubtype pair, or "cputype 0x…" for one the
 * table does not know. */
void ma_describe(uint32_t cputype, uint32_t cpusubtype, char out[32]);

/* Every name, comma-separated, for messages that list what was accepted. */
void ma_list(char *out, size_t outsz);

#endif /* MACHOTOOL_ARCH_NAMES_H */
