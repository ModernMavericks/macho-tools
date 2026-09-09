/* linkedit.c — see linkedit.h for the contract. Verbatim move out of
 * macho_grow.h; not yet renamed. */

#include "linkedit.h"

#include "mach_compat.h"

void mg_bump(uint32_t *off, uint32_t insert, uint32_t grow) {
    if (*off >= insert) *off += grow;
}

void mg_bump_all(uint8_t *buf, const struct mach_header_64 *hdr,
                  uint32_t insert, uint32_t grow) {
    uint8_t *lcp = buf + sizeof(*hdr);
    for (uint32_t i = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)lcp;
        switch (lc->cmd) {
        case LC_SYMTAB: {
            struct symtab_command *c = (struct symtab_command *)lcp;
            mg_bump(&c->symoff, insert, grow);
            mg_bump(&c->stroff, insert, grow);
            break;
        }
        case LC_DYSYMTAB: {
            struct dysymtab_command *c = (struct dysymtab_command *)lcp;
            mg_bump(&c->tocoff, insert, grow);
            mg_bump(&c->modtaboff, insert, grow);
            mg_bump(&c->extrefsymoff, insert, grow);
            mg_bump(&c->indirectsymoff, insert, grow);
            mg_bump(&c->extreloff, insert, grow);
            mg_bump(&c->locreloff, insert, grow);
            break;
        }
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY: {
            struct dyld_info_command *c = (struct dyld_info_command *)lcp;
            mg_bump(&c->rebase_off, insert, grow);
            mg_bump(&c->bind_off, insert, grow);
            mg_bump(&c->weak_bind_off, insert, grow);
            mg_bump(&c->lazy_bind_off, insert, grow);
            mg_bump(&c->export_off, insert, grow);
            break;
        }
        case LC_FUNCTION_STARTS:
        case LC_DATA_IN_CODE:
        case LC_CODE_SIGNATURE:
        case LC_SEGMENT_SPLIT_INFO:
        case LC_DYLIB_CODE_SIGN_DRS:
        case LC_LINKER_OPTIMIZATION_HINT:
        case LC_DYLD_EXPORTS_TRIE:
        case LC_DYLD_CHAINED_FIXUPS: {
            struct linkedit_data_command *c = (struct linkedit_data_command *)lcp;
            mg_bump(&c->dataoff, insert, grow);
            break;
        }
        default:
            break;  /* not a field this table knows about (segments and
                      * LC_MAIN stay macho_grow.h's own job; see linkedit.h) */
        }
        lcp += lc->cmdsize;
    }
}
