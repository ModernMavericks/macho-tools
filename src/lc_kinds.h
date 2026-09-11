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

/* The reverse of the table above: the KIND name a strip_cmds entry (an
 * LC_* value) came from, for diagnostics that need to name a load-command
 * kind back to the user in the vocabulary they typed it in (e.g. "no load
 * command of kind uuid to delete"). Lives here, next to LC_STRIP_KINDS,
 * for the same reason the table itself does -- one place to edit, so a
 * name<->LC_* mapping can't drift into a second, hand-rolled switch
 * somewhere a caller needed it. Returns "unknown" if `cmd` is not one of
 * LC_STRIP_KINDS's entries; defensive only; every strip_cmds value reaching
 * this function was itself produced by looking a name up in this same
 * table. */
const char *lc_kind_name(uint32_t cmd);

/* The forward direction: the LC_* value a KIND name (as typed: "uuid",
 * "codesig", ...) stands for. Returns 0 and sets *cmd if `name` is one of
 * LC_STRIP_KINDS's entries; returns -1 and leaves *cmd alone otherwise.
 * Every front-end that accepts a KIND -- cli/macho9.c's `lc -delete`,
 * src/script.c's `load-command delete`, and src/edit.c's lowering of it --
 * asks this, so none of them carries its own copy of the lookup; each still
 * words its own refusal. */
int lc_kind_by_name(const char *name, uint32_t *cmd);

#endif
