# `macho9` never writes its input

**Status:** design, agreed 2026-09-11. Replaces queue item 9 ("`edit` skips
the write when nothing changed"), which this makes moot.

## The problem

Four write paths in this toolkit can leave a file partly written:

1. **`wa_write_atomic`'s hard-link fallback** (`src/atomic_write.c`). A file
   with more than one hard link cannot be replaced by rename without leaving
   the other links on the old content, so it is truncated and rewritten in
   place, and a failure partway leaves it truncated.
2. **`minos` / `add_version_min`** write back through an open descriptor
   (`src/version_min.c:124`).
3. **`retag-swift` / `retag_swift_classes`**, likewise
   (`src/swift_retag.c:240`).
4. **The compat wrappers' multi-command path** (`mw_run_atomic`,
   `compat/macho9-compat.sh`) installs its result with `cat COPY > FILE`, on
   purpose, to keep the inode, links, xattrs and symlinks.

Behind all four is one design choice: the tools rewrite the binary they were
given. The repo owner's observation settles it -- editing a binary in place was
always a little odd. A tool that never writes its input has no partial-write
problem to manage on the input at all.

## Decisions, as agreed

- **`macho9` never writes its input.** Every rewriting verb takes an output,
  and refuses one that is the input.
- **Grammar:** `macho9 VERB FILE OUT …` -- OUT is always the second
  positional, right after FILE.
- **The compat wrappers provide "in place"** with a temp file and `mv`, and
  refuse a hard-linked FILE rather than silently split it.
- **Multi-command wrapper invocations become one edit script** -- the
  edit-scripts spec's "Consumers" -- which needs fat support (queue item 11)
  first.
- **`--dry-run` is removed.** A real run to a scratch OUT is the same run,
  write included, and the input is never at risk.
- **Queue item 9 is dropped.** With the original never rewritten there is
  nothing to skip.

## Design

### Grammar

```
macho9 dylib        FILE OUT OP...        -replace -delete -append -insert -reexport
macho9 rpath        FILE OUT OP...        -replace -delete -append -insert
macho9 lc           FILE OUT -delete KIND [-delete KIND...]
macho9 segment      FILE OUT OLD NEW
macho9 minos        FILE OUT 10.9 [--allow-grow]
macho9 retag-swift  FILE OUT
macho9 grow         FILE OUT N
macho9 declassify   FILE OUT                (already this shape)
macho9 edit         FILE OUT SCRIPT         (SCRIPT may be '-')
macho9 info FILE    macho9 verify FILE      (read-only; unchanged)
```

Verb-level flags (`--allow-grow`, `--fatal-warnings`, `--verbose`) keep their
current placement rules. A missing OUT is a usage error (2). `edit`'s
`--output` and `--dry-run` are gone, and `--capabilities` drops them.
`--capabilities` gains one line saying every rewriting verb takes `FILE OUT`,
so a wrapper can check rather than assume.

### One writer: `wa_write_new`

`src/atomic_write.{c,h}`:

```c
/* Write `size` bytes of `buf` as the file `out`, never touching `in`.
 * Refuses (returns WA_IS_INPUT, writing nothing) when `out` is `in`: the same
 * path once both are resolved, or -- when `out` exists -- the same device and
 * inode, which catches a symlink to `in` and a hard link to it. Otherwise
 * writes a temp file in `out`'s resolved directory, gives it `in`'s mode, its
 * owner (best-effort: needs privilege) and every extended attribute, fsyncs
 * it, and renames it onto `out` -- so `out` is either what it was or the whole
 * new content, never a partial file, and a symlink at `out` is followed to its
 * target rather than replaced. Returns 0, WA_IS_INPUT, or WA_FAILED with the
 * reason on stderr; on any non-zero return `out` is as it was and no temp file
 * remains. */
int wa_write_new(const char *in, const char *out, const uint8_t *buf, size_t size);
```

It replaces `wa_write_atomic`, whose hard-link fallback -- the only way the
old writer could leave a partial file -- has no reason to exist: `out` is
always a new file.

**Who calls it.** Each file-level operation gains an output and writes
through it: `mr_apply_file(path, out, ops)` (dylib, rpath, lc, segment),
`mv_add_version_min(path, out, allow_grow)` (minos), `mswift_retag_file(path,
out)` (retag-swift), `md_declassify(in, out)`, `cmd_grow`, and `me_run`. The
in-place descriptor writes in `version_min.c` and `swift_retag.c` go, and so do
their race guards: they existed because the tool read one open of `path` and
wrote another, and now it only reads it.

**The input is checked before any work.** A run whose OUT is its FILE is
refused (2, "OUT is FILE") before the file is read, not after a 200MB rewrite.
`wa_write_new` checks again at the write, since a path can change in between.

**Success lines tell the truth.** A single-operation verb that wrote says
`Wrote OUT (N bytes)` -- in particular mr_apply_file's `Updated PATH (N
bytes)` becomes that. (`edit` keeps its own report, whose lines already name
the file they wrote.)
A run that found nothing to do still writes OUT, an identical copy, and says
so, because a command that exits 0 must leave OUT there. Every progress line
labelled with the input path is unchanged.

### The compat wrappers provide "in place"

The wrappers keep their historical interfaces. Five edit FILE in place
(`change_dylib`, `add_version_min`, `fix_macho`, `rename_segment`,
`retag_swift_classes`, the last once per binary it is given), and
`patch_macho IN OUT` does too when IN and OUT are the same file, which its C
tool allowed. One shell function in `compat/macho9-compat.sh` does it for all:

1. **Resolve** FILE through any chain of symlinks to its real target, with
   `readlink` in a loop (10.9's has no `-f`): the `mv` must land on the
   target, not replace the symlink.
2. **Refuse a hard-linked target.** If its link count is above 1: exit 1 (the
   historical "failed"), before anything runs, saying FILE has N hard links,
   replacing it would leave the others on the old content, and to break the
   link first or run `macho9` with an explicit OUT.
3. **Run** the translated `macho9` command with the user's own FILE as input
   -- so every progress line still names FILE -- and
   `.NAME.macho9-compat.PID` beside the target as OUT, so the rename stays on
   one filesystem.
4. **Install** with `mv -f TMP TARGET`: atomic, and `macho9` has already
   given TMP the target's mode, xattrs and owner. If `macho9` or the `mv`
   fails: remove TMP, leave FILE untouched, exit with the wrapper's usual
   mapped code.
5. **Print the historical success line** where `macho9`'s now differs:
   capture `macho9`'s stdout, pass every other line through, and print the C
   tool's line (for example `Updated FILE (N bytes)`) only after the `mv`
   succeeded. Which wrappers need this is established against
   `tests/known-callers.sh` and `tests/wrapper_test.sh`, not assumed.

**Multi-command invocations become one edit script.** `compat/translate.sh`
emits a single `macho9 edit FILE TMP -` with one statement per translated
operation on stdin, emitting several `-insert` flags in reverse, because
script inserts go to the front in turn. That deletes `mw_run_atomic`'s
copy-aside-and-`cat` path and the stdout that named its copy. It needs fat
support (queue item 11) first: `change_dylib` accepts fat files.

On every wrapper path FILE is either wholly old or wholly new. The one new
behaviour a wrapper user can see is the hard-link refusal.

## Testing

**`wa_write_new`**, in a new hermetic `tests/atomic_write_test.c`:

- OUT that is FILE is refused and FILE untouched -- as the same path, as a
  symlink to FILE, and as a hard link to FILE;
- a new OUT gets FILE's mode and an extended attribute set on FILE;
- an existing OUT is replaced: new inode, new content;
- a write that fails partway -- forced by lowering `RLIMIT_FSIZE` with
  `SIGXFSZ` ignored, so no special filesystem is needed -- leaves OUT as it was
  and no temp file behind.

**`macho9`**, in `tests/cli_test.sh`, for every rewriting verb: missing OUT is
2; OUT naming FILE is 2 and FILE unchanged; a successful run leaves FILE's hash
and inode unchanged and writes OUT with "Wrote OUT"; a run with nothing to do
still writes an identical OUT. `edit --dry-run` and `edit --output` are usage
errors; `--capabilities` carries the `FILE OUT` line.

**The wrappers.** `tests/known-callers.sh` and `tests/wrapper_test.sh` stay the
byte-identical stdout gates, unchanged, and `tests/characterize.sh` -- which
drives `patch_macho`, `add_version_min` and `change_dylib` -- must still
reproduce `ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792`.
New: a symlinked FILE updates the target and stays a symlink; a hard-linked
FILE is refused (1) and untouched; a failed run leaves no temp file beside the
target; mode and a `com.apple.quarantine` xattr survive a wrapper run;
`change_dylib f -insert A -insert B` still gives A then B through the
edit-script path; a fat multi-command invocation works.

**Every existing suite** gains OUT on each `macho9` rewriting call. Mechanical,
and wide.

**Mutations, each shown to fail a test:** dropping the OUT-is-FILE check;
dropping the metadata copy; dropping the wrappers' hard-link refusal.

## Relation to other work

- **Sequencing:** after items 10 and 11 (the wrappers' edit-script path needs
  fat support), before item 3's rename. Items 10 and 11's plans stay valid as
  written: this item converts their `edit --output`/`--dry-run` tests along
  with every other call. Item 3's always-verbose task applies on top,
  unchanged.
- **The edit-scripts spec** (`2026-09-10-edit-scripts-design.md`) is amended:
  its `--output` and `--dry-run` passages are superseded, and under "binutils
  alignment" the `objcopy infile [outfile]` in-place shape moves from
  "Matched" to "Knowingly divergent", with this reason.
- **Item 5 (verb lowering)** later moves the verbs' reading and writing into
  one place; this design deliberately leaves each file-level operation doing
  its own, now with an output.

## Out of scope

- Fsyncing the directory after the rename: durability across a crash is
  unchanged from today.
- `tests/compat-matrix.tsv`, a frozen measurement.
- Refusing an OUT that already exists: it is replaced, as `cp` would.
