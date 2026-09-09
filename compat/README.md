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
`patch_macho`, `change_dylib` and `add_version_min` **by those names**.

**Today** it builds them from
[`Wowfunhappy/Mavericks-Porting-Resources`](https://github.com/Wowfunhappy/Mavericks-Porting-Resources),
not from this repo — this repo doesn't ship real installs yet, so renaming
these binaries right now would break nothing live (see `PROVENANCE.md`,
"Extracted from a branch, not from master"). Keeping the names matching is
about the adoption path, not present-day breakage: PRs #11/#12 upstream this
repo's fixes to that repo, and if/when Wowfunhappy merges them, or
`mavericksforever.com` points `install.sh` at this repo instead (see
`docs/PROPOSAL.md`, "one repo, first-party"), `install.sh`'s existing
`patch_macho`/`change_dylib`/`add_version_min` invocations have to already
resolve to the same names here. Renaming them now doesn't break a live
install today, but it would quietly break that adoption path later, or
force a coordinated rename with Wowfunhappy that has no reason to exist.

Retiring these entry points — folding what's left of them into `macho9` and
dropping the standalone binaries — is deliberately **out of scope** for the
convergence plan. It is a later generation's work, and it cannot happen until
`install.sh` moves first (see
[`docs/superpowers/plans/2026-09-09-finish-the-convergence.md`](../docs/superpowers/plans/2026-09-09-finish-the-convergence.md),
"What is NOT in this plan").

Only the source files live here now (Task 5, "empty the root"). The installed
binary names, and everything about how each tool behaves, are unchanged.
