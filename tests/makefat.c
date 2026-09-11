/* makefat/fatcheck: build and inspect a fat (universal) Mach-O without
 * depending on system lipo, whose accepted architecture list is not this
 * suite's to pin -- a hand-crafted fat container is something we control
 * completely, on either host. Reads/writes the on-disk convention every real
 * fat file uses (big-endian fat_header/fat_arch, i.e. FAT_CIGAM as observed
 * from a little-endian x86_64/arm64 host), by construction, not detection.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mach-o/fat.h>
static uint8_t *readfile(const char *path, size_t *outsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); exit(2); }
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
int main(int argc, char **argv) {
    if (argc != 10) { fprintf(stderr, "usage: %s out s0 ct0 cs0 al0 s1 ct1 cs1 al1\n", argv[0]); return 2; }
    size_t sz0, sz1;
    uint8_t *b0 = readfile(argv[2], &sz0);
    uint32_t ct0 = (uint32_t)strtoul(argv[3], NULL, 0);
    uint32_t cs0 = (uint32_t)strtoul(argv[4], NULL, 0);
    uint32_t al0 = (uint32_t)strtoul(argv[5], NULL, 0);
    uint8_t *b1 = readfile(argv[6], &sz1);
    uint32_t ct1 = (uint32_t)strtoul(argv[7], NULL, 0);
    uint32_t cs1 = (uint32_t)strtoul(argv[8], NULL, 0);
    uint32_t al1 = (uint32_t)strtoul(argv[9], NULL, 0);
    uint32_t hdrlen = (uint32_t)(sizeof(struct fat_header) + 2 * sizeof(struct fat_arch));
    uint32_t a0mask = (1u << al0) - 1;
    uint32_t off0 = (hdrlen + a0mask) & ~a0mask;
    uint32_t a1mask = (1u << al1) - 1;
    uint32_t off1 = (uint32_t)((off0 + sz0 + a1mask) & ~(uint64_t)a1mask);
    uint32_t total = (uint32_t)(off1 + sz1);
    uint8_t *out = calloc(1, total);
    struct fat_header *fh = (struct fat_header *)out;
    fh->magic = sw32(FAT_MAGIC);
    fh->nfat_arch = sw32(2);
    struct fat_arch *ar = (struct fat_arch *)(out + sizeof(struct fat_header));
    ar[0].cputype = (cpu_type_t)sw32(ct0);
    ar[0].cpusubtype = (cpu_subtype_t)sw32(cs0);
    ar[0].offset = sw32(off0);
    ar[0].size = sw32((uint32_t)sz0);
    ar[0].align = sw32(al0);
    ar[1].cputype = (cpu_type_t)sw32(ct1);
    ar[1].cpusubtype = (cpu_subtype_t)sw32(cs1);
    ar[1].offset = sw32(off1);
    ar[1].size = sw32((uint32_t)sz1);
    ar[1].align = sw32(al1);
    memcpy(out + off0, b0, sz0);
    memcpy(out + off1, b1, sz1);
    int ofd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) { perror("open out"); return 2; }
    if (write(ofd, out, total) != (ssize_t)total) { perror("write"); return 2; }
    close(ofd);
    return 0;
}
