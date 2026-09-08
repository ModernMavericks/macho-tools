# Provenance

This repo was extracted from
[Wowfunhappy/Mavericks-Porting-Resources](https://github.com/Wowfunhappy/Mavericks-Porting-Resources),
keeping the original commits rather than squashing them, so authorship and dates
survive: the tools' `Initial commit` (2026-04-19) and everything else of
Wowfunhappy's is his, under his name, at its original date.

Extracted with `git filter-branch --prune-empty --index-filter` over these paths:

```
patch_macho.c   change_dylib.c        change_dylib_test.sh
macho_grow.h    macho_grow_test.c
fix_macho.c     add_version_min.c
rename_segment.c  retag_swift_classes.c
```

Nothing was re-authored. Later work by Amitai Schleier (and Claude, credited in
trailers) sits on top as ordinary commits.

## Extracted from a branch, not from master

The source was `macho-grow-verify-invariant` rather than `master`, so the history
arrives current: it already contains the prove-it-or-refuse work — the refusal
for structures we could not re-base, re-basers for the export trie,
`LC_DATA_IN_CODE` and `__TEXT,__unwind_info`, the load-command classifier, and
`mg_verify` / `mg_plausible`. Extracting from `master` would have started this
repo nine commits behind and required re-applying them.

That work is also open upstream as PRs #11 and #12. They are deliberately left
open: `mavericksforever.com/claude/install.sh` still builds `patch_macho`,
`change_dylib` and `add_version_min` from `Mavericks-Porting-Resources`, so
until Wowfunhappy adopts this repo his artifacts keep the defect #11 fixes.
Merging there or consuming this repo is his call.

## Licensing — settled 2026-09-08

An earlier extraction of these tools recorded this as an open question, since
`Mavericks-Porting-Resources` carries no LICENSE file and its contents were
therefore all-rights-reserved by default. That is now resolved: in
[issue #4](https://github.com/Wowfunhappy/Mavericks-Porting-Resources/issues/4)
Wowfunhappy stated that anything original in that repository is public domain /
CC0 / WTFPL, that code from other projects keeps its own licence, and that he
will grant written consent for another licence on request.

See `LICENSE`, which records that and the one third-party acknowledgement
(LIEF / llvm-objcopy, for technique and a factual field enumeration, not code).

## Earlier extraction

A first extraction lives at `~/Documents/code/trees/mavericks-machotools`
(2026-08-14, 7 commits, local only, never pushed). It predates the naming
convention — the family prefixes checkouts, not repos, so the local directory is
`mavericks-macho-tools` and the remote is `macho-tools` — and it is missing
`change_dylib_test.sh`, `rename_segment.c` and `retag_swift_classes.c`. Its two
branches, `change-dylib-insert-renumber` and `macho-grow-init-offsets`, have both
since merged upstream as PRs #6 and #5.

The two share the root commit `6cb8675` but diverge above it, because they
filtered different path sets from different branches. This repo supersedes it;
the older one can be deleted once nothing is left to salvage from it.
