#ifndef MACHO9_LC_KINDS_H
#define MACHO9_LC_KINDS_H

#include <stdint.h>
#include <stddef.h>

/* The KIND vocabulary change_dylib's -strip-lc and macho9's `lc -delete`
 * both accept -- ONE table, in the shared library, so it has exactly one
 * place to edit. Before this, the same name->LC mapping was hand-copied in
 * three places (change_dylib.c's strippable[], cli/macho9.c's LC_KINDS[],
 * and a hardcoded "kinds=..." string inside macho9's --capabilities), which
 * defeats the one thing --capabilities exists for: a wrapper that trusts
 * the probe can be lied to just by editing one of the three and not the
 * others. Now both tools -- and --capabilities' advertised list -- read
 * this table, so they cannot diverge. */
struct lc_kind { const char *name; uint32_t cmd; };

extern const struct lc_kind LC_STRIP_KINDS[];
extern const size_t LC_STRIP_KINDS_COUNT;

#endif
