# `allow-grow` everywhere it is expected

**Status:** design, agreed 2026-09-11. Queue item 10.

## The problem

Growing the header pad is opt-in, and today only `dylib` and `rpath` can do it.
`mr_process_thin` checks whether the rebuilt load commands fit, and grows the
pad through `mg_grow_header` when `allow_grow` permits (`src/rewrite.c:778-816`).
Nothing else can:

- **`version-min set`** appends a 16-byte `LC_VERSION_MIN_MACOSX`. When the pad
  is short it refuses "no room for LC_VERSION_MIN_MACOSX"
  (`src/version_min.c:161-165`) whether or not `allow-grow` was given. The spec's
  own Zoom example (`2026-09-10-edit-scripts-design.md`, "Framework
  substitution") combines `allow-grow` with `version-min set`, and on a binary
  with a tight pad it is refused by a directive the author wrote. `macho9
  minos` has no `--allow-grow` at all.
- **The refusal names the wrong remedy.** mr_process_thin's no-room message
  says "pass -grow to enlarge it". `-grow` is `change_dylib`'s flag, not
  `macho9`'s (`--allow-grow`) and not `edit`'s (the `allow-grow` directive).
- **Grow's own refusals point at a document that does not exist.** The
  dylib/bundle and non-PIE messages in `mg_grow_header` (`src/grow.c`) cite
  `HEADER_PAD_GROWTH.md`, which is not in this repository. The chained-fixups
  refusal says "run patch_macho first", a tool an edit script's author need
  never have heard of.

## Scope, as agreed

Every operation that can run out of header pad honours `allow-grow`, **on the
images growth already supports**: 64-bit `MH_EXECUTE` with `MH_PIE`. Growth
lowers the image base into `__PAGEZERO`; dylibs and bundles have none and keep
refusing, with a message that says so plainly.

**`fixups set classic` is excluded, for two reasons checked against the code.**

- **It cannot grow.** Growth refuses any image still carrying
  `LC_DYLD_CHAINED_FIXUPS` (`mg_classify`, `src/grow.c:582`): chained pointers
  encode offsets from the image base, which growth moves. The conversion runs
  only on a chained image, so it cannot grow mid-conversion.
- **It does not need to.** Before adding its 48-byte `LC_DYLD_INFO_ONLY` it
  removes `LC_DYLD_CHAINED_FIXUPS` (16), `LC_DYLD_EXPORTS_TRIE` (16) and
  `LC_BUILD_VERSION` (24) (`src/declassify.c`, the removal loop before "Add
  LC_DYLD_INFO_ONLY"). Its "No room for LC_DYLD_INFO_ONLY" refusal is
  reachable only on an image missing most of those, which a modern chained
  binary does not.

**The consequence is an ordering rule.** On a chained image nothing can grow
until `fixups set classic` has converted it. An edit script that needs growth
must put that statement first, which the spec's examples already do. The
refusal a misordered script gets must say so (below).

## Design

### One place decides whether there is room

A new function in `src/grow.c`, declared in `src/grow.h`:

```c
/* Ensure the load-command region can extend to `need_end` bytes from the
 * start of the image (sizeof(mach_header_64) + the new sizeofcmds).
 *
 * Returns 0 with the image untouched if it already fits, or 0 after growing
 * it -- *pbuf/*pfsize updated, and every pointer the caller held into the
 * buffer stale. Returns -1 with the image untouched when it does not fit and
 * either growth was not permitted or this image cannot be grown; the reason
 * has been printed to stderr, prefixed with `label`. */
int mg_ensure_pad(uint8_t **pbuf, size_t *pfsize, uint32_t need_end,
                  int allow_grow, const char *label);
```

- **mr_process_thin calls it** in place of its inline block. The stdout lines
  that block prints on the grow path -- "load commands need N more bytes than
  the M-byte pad; growing header..." and "grew header pad: first sect now at
  N (M bytes available)" -- move into `mg_ensure_pad` with their text
  unchanged. They are on `change_dylib -grow`'s path, whose stdout is a
  byte-identical contract.
- **version-min calls it** (below). There is no third copy of the rule.
- **Growability stays where it is.** `mg_grow_header` already validates every
  precondition -- 64-bit, `MH_EXECUTE`, `MH_PIE`, and `mg_classify`'s audit of
  base-relative load commands, which is where chained fixups are refused --
  before it mutates anything (`src/grow.c:881-925`, `:1010`). `mg_ensure_pad`
  relies on that rather than duplicating it.
- **A refusal is still MR_REFUSED (1).** An allocation failure inside growth
  stays folded into refused, as disclosed at `src/rewrite.c`'s MR_ERROR
  mapping. This design does not change that.

### The messages say the true remedy

| message | was | becomes |
|---|---|---|
| no room, growth not permitted | "…don't fit in header pad (N avail); pass -grow to enlarge it" | "…don't fit in header pad (N avail); growing the header needs allow-grow" |
| chained fixups (`mg_classify`) | "LC_DYLD_CHAINED_FIXUPS is not supported here; run patch_macho first to convert it to LC_DYLD_INFO_ONLY" | "…; convert them first (`fixups set classic` in an edit script, or `macho9 declassify`)" |
| dylib/bundle, non-PIE (`mg_grow_header`) | cite `HEADER_PAD_GROWTH.md` | say this tool cannot grow them, and why, with no dangling reference |

"allow-grow" is the one name the `edit` directive and `macho9`'s
`--allow-grow` share; `change_dylib`'s documentation already says its `-grow`
means `--allow-grow`. A per-front-end hint was considered and rejected: it
needs a new `mr_ops` field threaded through every caller to vary one line.

These are stderr lines. `tests/compat-matrix.tsv` records the old wording, but
it is a frozen, dated measurement, not a live gate; it is not edited.

### version-min grows

**The in-memory entry point takes a buffer it may reallocate:**

```c
int mv_add_version_min_image(uint8_t **pbuf, size_t *psize,
                             int allow_grow, int *out_added);
```

Same shape as `mr_apply_image`. The existing three-way "no room" test
(`src/version_min.c:149-166`) keeps its two cases growth cannot cure -- no
section with file data at all, and a load-command region that would run past
the buffer -- as refusals. Only the room-before-the-first-section case calls
`mg_ensure_pad(pbuf, psize, lc_end + sizeof(struct version_min_command),
allow_grow, …)`, then re-derives its header pointer and continues.

**The file path takes the permission too:**
`int mv_add_version_min(const char *path, int allow_grow)`. It keeps writing
back through its already-open descriptor with its race guard. A grown image is
larger than the file it was read from; writing it from offset 0 extends the
file, so the write must cover the new size, nothing more. A failed in-place
write can leave a partial file, exactly as it can today.

### Front-ends

| front-end | change |
|---|---|
| `edit` | `version-min set` honours the `allow-grow` directive: `src/edit.c`'s MS_VERSION_MIN arm passes `s->allow_grow` |
| `macho9 minos FILE 10.9` | gains `--allow-grow`, after the version, as `dylib`/`rpath` take their flags; the default is unchanged |
| `--capabilities` | `verb minos versions=10.9 flags=allow-grow` |
| `compat/add_version_min.sh` | unchanged: the historical tool never grew, so the wrapper never passes the flag |

### Documentation

`src/edit.h`, the README's `edit` section and `cmd_minos`'s usage say today
that allow-grow reaches `dylib` and `rpath` only. They change to say exactly:

- `allow-grow` covers `dylib`, `rpath` and `version-min set`;
- it does not cover `fixups set classic`, and why;
- on a chained image nothing grows before `fixups set classic`, so put it first;
- growth works only on a PIE executable (unchanged).

## Testing

**`mg_ensure_pad`, hand-built images in `tests/grow_test.c`:**

- fits: returns 0; the buffer is neither reallocated nor changed;
- short, not permitted: returns -1; the bytes are unchanged;
- short, permitted, PIE executable: returns 0; the first section's file offset
  moved out by a page; `mg_plausible` passes;
- `MH_DYLIB`, a non-PIE executable, and a PIE executable carrying
  `LC_DYLD_CHAINED_FIXUPS`: each returns -1 with the bytes unchanged, and the
  chained case's message names `fixups set classic`. This check lives here and
  not in `cli_test`: cli_test's chained fixture (`mkchained`) is `MH_DYLIB`, so
  the dylib refusal fires before the chained one ever could.

**A tight fixture, in `tests/cli_test.sh`.** A normal fixture has far more
than 16 bytes of pad. Read the pad from `macho9 info`, then `dylib append` a
path sized so fewer than 16 bytes remain -- computed, not hard-coded, because
CI's linker leaves a different pad (the existing allow-grow test does the
same). On it:

- `edit`: `version-min set` without the directive refuses (1), hash and inode
  unchanged; with `allow-grow` it succeeds, `LC_VERSION_MIN_MACOSX` is
  present, `macho9 verify` passes;
- `macho9 minos`: refuses (1) without `--allow-grow`, and the message says
  "allow-grow"; succeeds with it; `--capabilities` shows `flags=allow-grow`;
- `compat/add_version_min`: still refuses -- pinning that the historical
  behaviour did not change.

**The refactor changes no output.** The existing suites and the
characterization digest (`ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792`)
must pass, plus an old-versus-new comparison of `change_dylib -grow` and
`macho9 dylib --allow-grow` over the existing fixtures: stdout, exit code and
resulting bytes identical.

**Mutations, each shown to fail a test and reverted:** dropping `allow_grow`
from `edit`'s version-min arm; making `mg_ensure_pad` ignore `allow_grow`;
removing the chained-fixups case from `mg_classify`.

## Out of scope

- Growing dylibs and bundles. They have no `__PAGEZERO`; a different mechanism
  that shifts segments and fixes every rebased pointer would be its own design.
- `fixups set classic` growth, for the reasons above.
- Splitting growth's allocation failures from its refusals: that widens
  `src/grow.c`'s contracts, which item 5 restructures.
- Fat files: queue item 11, designed next.

## For item 5

Its relations design (`2026-09-10-relations-and-verb-lowering-design.md`)
lists `fixups set classic` as disturbing `__LINKEDIT`'s blobs and the image
base. It also changes the load-command region -- it removes up to three
commands and adds one -- so it belongs in the header-pad column too.
`mg_ensure_pad` is the enforcement point that column describes.
