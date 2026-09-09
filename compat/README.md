# compat/

The six original entry points, kept for compatibility.

`macho9` (`cli/macho9.c`) is the toolkit's own multi-verb CLI, built on
`src/`. These six single-purpose tools predate it and mostly duplicated their
own open/fstat/malloc/read/`ncmds`-walk preamble before Task 1 of the
[toolkit convergence
plan](../docs/superpowers/plans/2026-09-08-macho9-toolkit.md) converted them
onto `src/image.h`. They now share the same toolkit library `macho9` does
(`macho9core`), but they still exist as separate binaries, under their
original names, because `mavericksforever.com/claude/install.sh` builds
`patch_macho`, `change_dylib` and `add_version_min` **by those names** — it is
a real, external, unversioned consumer of this repo, and breaking those names
breaks real installs.

Retiring these entry points — folding what's left of them into `macho9` and
dropping the standalone binaries — is deliberately **out of scope** for the
convergence plan. It is a later generation's work, and it cannot happen until
`install.sh` moves first (see
[`docs/superpowers/plans/2026-09-09-finish-the-convergence.md`](../docs/superpowers/plans/2026-09-09-finish-the-convergence.md),
"What is NOT in this plan").

Only the source files live here now (Task 5, "empty the root"). The installed
binary names, and everything about how each tool behaves, are unchanged.
