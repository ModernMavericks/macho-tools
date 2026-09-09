/*
 * Convert a Mach-O dylib from chained fixups format (macOS 12+)
 * to traditional LC_DYLD_INFO_ONLY format (macOS 10.6+).
 *
 * The conversion itself is src/declassify.c (md_declassify), shared with
 * cli/macho9.c's `declassify` verb so the two front-ends cannot disagree
 * about what declassifying a binary means. What is left here is this tool's
 * own grammar (`patch_macho IN OUT`), its output file (created 0755, written
 * with a plain open+write, no atomic replace), its messages, and its exit
 * code -- 1 for everything that goes wrong, which is what it has always
 * returned and what install.sh's callers see. macho9's verb makes a finer
 * distinction; see cli/macho9.c's cmd_declassify for the list of deliberate
 * divergences.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "declassify.h"

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "Usage: %s input output\n", argv[0]); return 1; }

    uint8_t *buf = NULL;
    size_t out_size = 0;
    int rc = md_declassify(argv[1], &buf, &out_size);

    /* Tested by name, not `< 0`: MDCL_NOT_MACHO is the one code md_declassify
     * deliberately says nothing about, so that each front-end can name the
     * file in its own words -- this message is exactly what this tool has
     * always printed. Every other refusal has already printed its own reason
     * and needs no second line here. */
    if (rc == MDCL_NOT_MACHO) {
        fprintf(stderr, "%s: not a readable 64-bit Mach-O\n", argv[1]);
        return 1;
    }
    /* Anything that is not one of the two successes is a failure this tool
     * reports as 1 -- MDCL_REFUSED and MDCL_ERROR today, and any code
     * declassify.h grows later. macho9's verb tells those two apart; this one
     * never has and does not start now. Spelled as "not a success" rather than "== MDCL_REFUSED" so a
     * future code cannot fall through to writing an output file from a NULL
     * buffer; the exit code stays the flat 1 this tool has always used. */
    if (rc != MDCL_CONVERTED && rc != MDCL_PASSTHROUGH) return 1;

    /* The pass-through and converting paths write the same way but report
     * differently, as they always have: a pass-through says only "Already
     * patched ... passing through." (md_declassify's own line) and checks its
     * write; a conversion ends with the "Wrote ..." line. Kept exactly, rather
     * than tidied into one path, because this tool's observable behaviour is
     * what the compat guarantee is about. */
    if (rc == MDCL_PASSTHROUGH) {
        int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0755);
        if (fd < 0) { perror("create output"); return 1; }
        if (write(fd, buf, out_size) != (ssize_t)out_size) { perror("write"); close(fd); return 1; }
        close(fd);
        free(buf);
        return 0;
    }

    int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) { perror("create output"); return 1; }
    write(fd, buf, out_size);
    close(fd);
    printf("Wrote %s (%zu bytes)\n", argv[2], out_size);

    free(buf);
    return 0;
}
