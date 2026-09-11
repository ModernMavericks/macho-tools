/* See tests/makefat.c for what these fixture helpers are and why. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
static uint8_t *readfile(const char *path, size_t *outsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); exit(2); }
    struct stat st; fstat(fd, &st);
    uint8_t *buf = malloc((size_t)st.st_size);
    if (read(fd, buf, (size_t)st.st_size) != (ssize_t)st.st_size) { perror("read"); exit(2); }
    close(fd);
    *outsz = (size_t)st.st_size;
    return buf;
}
static uint32_t sw32(uint32_t v) {
    return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v & 0xff0000) >> 8) | ((v >> 24) & 0xff);
}
static void locate_arch(const uint8_t *buf, size_t sz, int idx, uint32_t *off, uint32_t *size,
                         uint32_t *align) {
    uint32_t magic = *(const uint32_t *)buf;
    if (magic != FAT_MAGIC && magic != FAT_CIGAM) { fprintf(stderr, "not a fat file\n"); exit(2); }
    int swap = (magic == FAT_CIGAM);
    const struct fat_header *fh = (const struct fat_header *)buf;
    uint32_t narch = swap ? sw32(fh->nfat_arch) : fh->nfat_arch;
    if ((uint32_t)idx >= narch) { fprintf(stderr, "arch %d out of range (narch=%u)\n", idx, narch); exit(2); }
    const struct fat_arch *ar = (const struct fat_arch *)(buf + sizeof(struct fat_header));
    uint32_t o = swap ? sw32((uint32_t)ar[idx].offset) : (uint32_t)ar[idx].offset;
    uint32_t s = swap ? sw32((uint32_t)ar[idx].size)   : (uint32_t)ar[idx].size;
    uint32_t a = swap ? sw32(ar[idx].align) : ar[idx].align;
    if ((size_t)o + s > sz) { fprintf(stderr, "arch %d out of bounds\n", idx); exit(2); }
    *off = o; *size = s; *align = a;
}
static void dump_dylibs(const uint8_t *p, size_t sz) {
    if (sz < sizeof(struct mach_header_64)) { fprintf(stderr, "slice too small\n"); exit(2); }
    const struct mach_header_64 *hdr = (const struct mach_header_64 *)p;
    if (hdr->magic != MH_MAGIC_64) { fprintf(stderr, "slice not 64-bit Mach-O (magic=0x%x)\n", hdr->magic); exit(2); }
    const uint8_t *lcp = p + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)lcp;
        if (lc->cmd == LC_LOAD_DYLIB || lc->cmd == LC_ID_DYLIB ||
            lc->cmd == LC_LOAD_WEAK_DYLIB || lc->cmd == LC_REEXPORT_DYLIB) {
            const struct dylib_command *dc = (const struct dylib_command *)lcp;
            printf("%s\n", (const char *)lcp + dc->dylib.name.offset);
        }
        lcp += lc->cmdsize;
    }
}
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: fatcheck <mode> <file> [args...]\n"); return 2; }
    const char *mode = argv[1];
    size_t sz; uint8_t *buf = readfile(argv[2], &sz);
    if (strcmp(mode, "archinfo") == 0) {
        uint32_t magic = *(uint32_t *)buf;
        if (magic != FAT_MAGIC && magic != FAT_CIGAM) { fprintf(stderr, "not a fat file\n"); return 2; }
        int swap = (magic == FAT_CIGAM);
        const struct fat_header *fh = (const struct fat_header *)buf;
        uint32_t narch = swap ? sw32(fh->nfat_arch) : fh->nfat_arch;
        printf("narch=%u\n", narch);
        for (uint32_t i = 0; i < narch; i++) {
            uint32_t o, s, a; locate_arch(buf, sz, (int)i, &o, &s, &a);
            printf("%u %u %u %u\n", i, o, s, a);
        }
        return 0;
    } else if (strcmp(mode, "dump") == 0) {
        if (argc != 5) { fprintf(stderr, "usage: fatcheck dump <file> <idx> <outfile>\n"); return 2; }
        int idx = atoi(argv[3]);
        uint32_t o, s, a; locate_arch(buf, sz, idx, &o, &s, &a);
        int ofd = open(argv[4], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ofd < 0) { perror("open out"); return 2; }
        if (write(ofd, buf + o, s) != (ssize_t)s) { perror("write"); return 2; }
        close(ofd);
        return 0;
    } else if (strcmp(mode, "dylibs") == 0) {
        if (argc != 4) { fprintf(stderr, "usage: fatcheck dylibs <file> <idx>\n"); return 2; }
        int idx = atoi(argv[3]);
        uint32_t o, s, a; locate_arch(buf, sz, idx, &o, &s, &a);
        dump_dylibs(buf + o, s);
        return 0;
    }
    fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
}
