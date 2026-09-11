# `edit` on fat (universal) files

**Status:** design, agreed 2026-09-11. Queue item 11.

## The problem

`macho9 edit` refuses every fat file (`src/edit.c`, `me_refuse_input`). Modern
macOS applications ship universal -- x86_64 plus arm64 -- so the population the
edit-scripts spec names ("arbitrary modern macOS applications") is largely fat.

It is not a regression: `edit` is new, and every verb behaves as it did. But it
is lopsided. `dylib`, `rpath`, `lc` and `segment` already edit a fat file slice
by slice through `mr_process_fat` (`src/rewrite.c:997`), so a script of only
those statements works through the verbs and is refused by `edit`. And it
becomes a regression the day `compat/translate.sh` starts emitting edit scripts
(the edit-scripts spec, "Consumers"): a `change_dylib fat -strip-lc uuid
-change A B` that works today would be refused.

One fact shapes the design. `fixups set classic` understands two chained
pointer formats, `DYLD_CHAINED_PTR_64` and `_64_OFFSET`
(`src/declassify.c:355`), and refuses "Unknown ptr format" otherwise. On a
universal app a statement that succeeds on the x86_64 slice can be refused on
the arm64 slice. So which slices a script touches has to be something the
script can say.

## Decisions, as agreed

- **Which slices:** every 64-bit slice by default, as the verbs do, plus an
  optional `arch NAME` directive that limits the script to the named slices.
- **The container stays:** `edit` never drops a slice. Slices a script does not
  touch pass through byte-identical. Thinning stays lipo's job; the reserved
  vocabulary (`-thin`, `-remove`) can become a statement later without breaking
  any script.
- **One split-and-reassemble,** shared by the verbs and `edit`, not a second
  copy of the layout rule.

## Design

### One split-and-reassemble, in `src/fat.c`

```c
/* One selected slice, split out as its own buffer. The callback may grow it:
 * reallocate *pbuf and update *psize. */
typedef int (*mfat_slice_fn)(uint8_t **pbuf, size_t *psize,
                             const mfat_arch *a, uint32_t index, void *ctx);

/* Split a 32-bit-header fat file, call `fn` for each slice `select` accepts,
 * and reassemble. Every slice keeps its offset until an earlier one grows;
 * later slices then shift, each keeping its own alignment. Slices `select`
 * rejects, and 32-bit slices, pass through byte-identical.
 * Returns 0 (with *modified set if anything changed), or the first non-zero
 * code a callback returned. On any non-zero return the input is untouched
 * and every split buffer freed. */
int mfat_rewrite(uint8_t **pbuf, size_t *psize,
                 int (*select)(const mfat_arch *a, void *ctx),
                 mfat_slice_fn fn, void *ctx, int *modified);
```

- **Moved out of `mr_process_fat`, not copied:** the slice loop, the per-slice
  copies, and the layout -- including the refusal when a grown slice on a
  non-ascending arch table would overlap another, and sizing the output from
  the furthest slice end rather than the last one processed.
- **Left in `mr_process_fat`:** only what is rewrite-specific -- the thin
  rewrite as its callback, and the hit arrays it accumulates across slices,
  through `ctx`.
- **Labels belong to the callback.** The verb path's per-slice stdout --
  `arch N (cputype 0x…): …`, "not a 64-bit Mach-O; leaving this slice
  unchanged", "Nothing to change." -- is on `change_dylib`'s fat path, whose
  stdout is a byte-identical contract. The verb callback keeps building those
  labels; `edit`'s builds its own.
- **A slice failing refuses the whole file,** as today ("a partial rewrite
  would leave its slices inconsistent"), and writes nothing.
- **`FAT_MAGIC_64` stays refused** by `mfat_parse`, as everywhere today.

### One table of arch names

`src/arch_names.{c,h}` maps lipo's names -- `x86_64`, `x86_64h`, `arm64`,
`arm64e`, `i386` -- to cputype/cpusubtype and back. An unknown cputype prints as
`cputype 0x…`. Constants the 10.9 SDK lacks (arm64's cputype or subtypes) go in
`src/mach_compat.h`, beside the load-command constants already there. The table
serves the `arch` directive and `edit`'s messages, and could later give
`macho9 info` arch names.

### The `arch` directive

- A directive like the other two: precedes every operation, repeatable
  (a repeat of the same name is harmless), exactly one operand.
- The name is checked against the arch table when the script is parsed:
  `arch amd64` is a parse error (2), before the file is opened.
- `edit` only. The single-operation verbs keep editing every 64-bit slice.

**Which slices a run touches:**

| input | no `arch` | with `arch` directives |
|---|---|---|
| thin | the image, as today | runs only if the image's arch is named; else refused (1), naming the image's arch |
| fat | every 64-bit slice; 32-bit slices pass through | exactly the named slices. A name the file lacks is refused (1), listing the file's slices. A name that is a 32-bit slice is refused (1): statements cannot apply to it |

One script then works on thin and fat inputs alike. A script that says `arch
x86_64` cannot be quietly applied to a slice nobody named. And when a vendor
drops a slice in a later release, the script is refused rather than silently
editing nothing -- the version-churn case the edit-scripts spec calls normal.

### Execution

- Per selected slice, in the order the fat header lists them: every
  statement, in order, then that slice's own final verification (`mg_plausible`, never subject to
  `MACHO_NO_VERIFY`). This is `me_run`'s thin loop, run as `mfat_rewrite`'s
  callback. Slice by slice, not statement by statement: slices are
  independent images, so the result is the same, with one reassembly.
- The reassembled container is re-parsed with `mfat_parse` before it is
  written.
- Anything failing in any slice refuses the whole run; nothing is written. The
  refusal names the slice: `macho9 edit: refused at statement 3 of 7 (line 9)
  in slice arm64; FILE left unmodified`.

**`fatal-warnings` across slices: a statement is matched if it matched in any
selected slice.** This is what `mr_process_fat` already does for the verbs,
and slices usually share their load commands. Requiring a match in every slice
would refuse universal apps whose arm64 slice legitimately links something
different. The miss verdict is therefore taken after every slice has run,
rather than right after the statement. On a thin file that is unobservable,
and nothing is written either way.

### Reporting

Without `--verbose`, nothing is added: the refusal and dry-run lines stay the
only unconditional output. `--dry-run` reports the whole container's size.

Under `--verbose`, on a fat file, **every slice gets a line**, in the order
the fat header lists them:

| slice | log |
|---|---|
| edited | `slice x86_64:`, then its statement and follow-up lines, then `slice x86_64: verified` |
| not selected by `arch` | `slice arm64: not selected by arch; passed through unchanged` |
| 32-bit | `slice i386: 32-bit; passed through unchanged` |

A passed-through slice can still move: when an earlier slice grew, a later
untouched one shifts to keep its alignment. Its line then ends `(moved from
offset 0x8000 to 0x9000)` -- its bytes are identical, but where it lives is
not, and that is the kind of consequence the log exists to show. Thin-file
output is unchanged.

The "fat file refused" message goes away. `FAT_MAGIC_64` keeps its own
refusal. The README and `src/edit.h` document the directive, the table above,
and the rule for `fatal-warnings`.

## Testing

**Fat fixtures are hand-built, not made with lipo.** This follows the
convention of `tests/change_dylib_test.sh`'s `makefat` and
`tests/wrapper_test.sh`'s `fm_mkfat`: the big-endian `fat_header`/`fat_arch`
written by construction, so both hosts build the same container and no test
depends on which architectures a host's lipo accepts or its SDK can link. (A
modern SDK cannot build i386; 10.9's cannot build arm64.) Slices are thin
fixtures relabelled with the cputype each test needs, in both the `fat_arch`
entry and the slice's own header, plus a minimal 32-bit header for the
pass-through case.

**The arch table:** every name maps to cputype/cpusubtype and back; an unknown
name is rejected; an unknown cputype prints as `cputype 0x…`.

**`mfat_rewrite`, hand-built images in the C tests:**

- nothing selected: the input is untouched and `modified` is clear;
- slice 0 grown: slice 1 shifts to its alignment and its bytes are unchanged;
- a non-ascending arch table with a grown slice is still refused, as today;
- a callback failing on the second slice: the input is untouched, every split
  buffer freed, the callback's code returned.

**The verbs' fat output does not change.** The existing fat tests and the
characterization digest
(`ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792`) pass, plus
an old-versus-new comparison of `macho9 dylib` and `change_dylib` on the fat
fixtures: stdout, exit code and resulting bytes identical.

**`edit` on fat files, in `tests/cli_test.sh`:**

- no `arch`: every 64-bit slice edited; the 32-bit slice byte-identical;
  `macho9 verify` passes;
- `arch x86_64`: on a fat file only that slice is edited; on a thin x86_64 file
  the run proceeds; on a thin file of another arch it is refused (1);
- `arch arm64` on a file without that slice: refused (1), the message lists the
  file's slices, the file unchanged; `arch i386`: refused (1);
- `arch amd64`: parse error (2);
- under `fatal-warnings`, a statement matching in one slice but not another is
  not a miss; one matching nowhere is;
- a statement refused in the second slice: the file is byte-identical, and the
  refusal names that slice;
- `--verbose` shows a line for every slice, passed-through ones included, and
  the moved offset where it happens; `--dry-run` writes nothing.

**Mutations, each shown to fail a test and reverted:** ignoring `arch`
selection; deciding `fatal-warnings` per slice instead of across them; writing
before the second slice has been processed.

## Out of scope

- Dropping slices: lipo's job, until a statement is wanted.
- `FAT_MAGIC_64` containers.
- `arch` selection for the single-operation verbs.
- Arch names in `macho9 info` (the table makes it a small follow-up).

## Relation to other queued work

- **Item 10 (`allow-grow` everywhere):** growth now happens per slice. A grown
  slice is what makes later slices move, which `mfat_rewrite`'s layout rule
  already handles. Landing item 10 first means `version-min set` can grow a
  slice here too; landing this first changes nothing in item 10's design.
- **Item 9 (skip the write when nothing changed):** applies to the whole
  container unchanged -- compare the reassembled bytes against FILE.
- **Item 5 (relations and verb lowering):** `mr_process_fat` becomes a thin
  caller of `mfat_rewrite`, which is one fewer place for verb lowering to
  untangle.
