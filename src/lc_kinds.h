#ifndef MACHOTOOL_LC_KINDS_H
#define MACHOTOOL_LC_KINDS_H

#include <stdint.h>
#include <stddef.h>

/* The KIND vocabulary change_dylib's -strip-lc and machotool's `lc -delete`
 * both accept -- ONE table, in the shared library, so it has exactly one
 * place to edit. Before this, the same name->LC mapping was hand-copied in
 * three places (change_dylib.c's strippable[], cli/machotool.c's LC_KINDS[],
 * and a hardcoded "kinds=..." string inside machotool's --capabilities), which
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
 * Every front-end that accepts a KIND -- cli/machotool.c's `lc -delete`,
 * src/script.c's `load-command delete`, and src/edit.c's lowering of it --
 * asks this, so none of them carries its own copy of the lookup; each still
 * words its own refusal. */
int lc_kind_by_name(const char *name, uint32_t *cmd);

/* A load command's own LC_* name ("LC_LOAD_DYLIB", "LC_BUILD_VERSION", ...),
 * for every kind this toolkit names back to a user -- a different
 * vocabulary from the KIND names above, which cover only what `lc -delete`
 * can strip. Returns NULL for a kind not in its list, so each caller decides
 * how to show one it does not know. cli/machotool.c's `info` dump and
 * src/edit.c's verbose report both ask this; it is the one list. */
const char *lc_cmd_name(uint32_t cmd);

#endif
