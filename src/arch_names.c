#include "arch_names.h"
#include "mach_compat.h"

#include <mach/machine.h>
#include <stdio.h>
#include <string.h>

static const struct { const char *name; uint32_t cputype, cpusubtype; } MA_TABLE[] = {
    { "x86_64",  (uint32_t)CPU_TYPE_X86_64, (uint32_t)CPU_SUBTYPE_X86_64_ALL },
    { "x86_64h", (uint32_t)CPU_TYPE_X86_64, (uint32_t)CPU_SUBTYPE_X86_64_H   },
    { "arm64",   (uint32_t)CPU_TYPE_ARM64,  (uint32_t)CPU_SUBTYPE_ARM64_ALL  },
    { "arm64e",  (uint32_t)CPU_TYPE_ARM64,  (uint32_t)CPU_SUBTYPE_ARM64E     },
    { "i386",    (uint32_t)CPU_TYPE_I386,   (uint32_t)CPU_SUBTYPE_I386_ALL   },
};
static const int MA_N = (int)(sizeof MA_TABLE / sizeof MA_TABLE[0]);

int ma_row(int r, const char **name, uint32_t *cputype, uint32_t *cpusubtype) {
    if (r < 0 || r >= MA_N) return 0;
    *name = MA_TABLE[r].name;
    *cputype = MA_TABLE[r].cputype;
    *cpusubtype = MA_TABLE[r].cpusubtype;
    return 1;
}

int ma_lookup(const char *name) {
    for (int r = 0; r < MA_N; r++)
        if (strcmp(name, MA_TABLE[r].name) == 0) return r;
    return -1;
}

int ma_index(uint32_t cputype, uint32_t cpusubtype) {
    uint32_t sub = cpusubtype & ~(uint32_t)CPU_SUBTYPE_MASK;
    for (int r = 0; r < MA_N; r++)
        if (MA_TABLE[r].cputype == cputype && MA_TABLE[r].cpusubtype == sub) return r;
    return -1;
}

void ma_describe(uint32_t cputype, uint32_t cpusubtype, char out[32]) {
    int r = ma_index(cputype, cpusubtype);
    if (r >= 0) snprintf(out, 32, "%s", MA_TABLE[r].name);
    else        snprintf(out, 32, "cputype 0x%x", cputype);
}

void ma_list(char *out, size_t outsz) {
    size_t o = 0;
    if (outsz) out[0] = '\0';
    for (int r = 0; r < MA_N && o < outsz; r++) {
        int w = snprintf(out + o, outsz - o, "%s%s", r ? ", " : "", MA_TABLE[r].name);
        if (w < 0) break;
        o += (size_t)w;
    }
}
