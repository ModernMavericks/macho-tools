# `macho9 rewrite` — recipes, and two vocabularies

**Status:** design, agreed 2026-09-10. Supersedes the `port` verb sketched in
`docs/PROPOSAL.md`, including its name.

**Depends on:** Task 0 of
`docs/superpowers/plans/2026-09-10-report-what-macho9-did.md`. Verification is
currently blind on every dylib and bundle, so a mandatory verify gate would be a
mandatory rubber stamp until that lands. See "Verification" below.

## The problem, measured

`mavericksforever.com/claude/install.sh` patches a ~200MB Claude Code binary by
running three tools in sequence — `patch_macho`, then `add_version_min`, then
`change_dylib` — so **the binary is written three times**, and a failure in the
third leaves a half-converted file where the first two succeeded.

One of those invocations mixes two operation families in a single command:

```sh
change_dylib "$T" -strip-lc uuid -strip-lc codesig \
    -change "/usr/lib/libSystem.B.dylib"  "@loader_path/../S.dylib" \
    -change "/usr/lib/libicucore.A.dylib" "@loader_path/../I.dylib" \
    -change "/usr/lib/libc++.1.dylib"     "@loader_path/../c++.1.dylib"
```

The stripping is what makes room for the rewriting. Under a
subcommand-per-family CLI that becomes two invocations, `lc` then `dylib`, and
`docs/PROPOSAL.md` predicted the cost. The compat-tool retirement then measured
it: two rows of the 1227-row behaviour sweep came back where the C tool refused
**atomically** while the translated sequence refused **after already writing**.

The shipped mitigation is shell: `compat/macho9-compat.sh` copies the target
beside itself, runs the sequence against the copy, and installs on full success.
That restores atomicity but not the single write, and it costs a third copy —
peak footprint ~3x the file where the C tool needed 2x.

## What this design does

Adds one verb, `macho9 rewrite`, which takes a **recipe**: a flat sequence of
statements applied to one in-memory image, verified, and written once.

```
macho9 rewrite FILE RECIPE                 # rewrite FILE in place
macho9 rewrite FILE RECIPE --output OUT    # write elsewhere; FILE untouched
macho9 rewrite FILE -                      # recipe on stdin
```

`install.sh`'s three writes collapse to one. The mixed-family split disappears,
and with it the temp-copy dance and its extra 200MB.

## Two vocabularies

Statements are **physical** or **logical**, and the language marks which. The
distinction is not stylistic: it is whether a statement has consequences beyond
the bytes it names.

Space consequences — pad exhaustion, growth — apply to *any* statement, so they
are handled orthogonally by the `allow-grow` directive. That leaves **reference
invalidation** as the only thing separating the two levels, and the
ordinal-carrying command set is exactly four (`mo_is_ordinal_lc`,
`src/ordinals.c`: `LC_LOAD_DYLIB`, `LC_LOAD_WEAK_DYLIB`, `LC_REEXPORT_DYLIB`,
`LC_LOAD_UPWARD_DYLIB`). So the logical vocabulary is small enough to name
exhaustively rather than gesture at.

**Physical** — touches only what it names:

```
load-command  delete    KIND        uuid | codesig | source-version
                                    | build-version | code-sign-drs
segment       rename    OLD NEW
version-min   set       10.9
retag-swift
dylib         replace   OLD NEW     position and ordinal kept
dylib         append    PATH        hands out a new index only
dylib         reexport  PATH        LC_LOAD_DYLIB -> LC_REEXPORT_DYLIB, same position
rpath         replace   OLD NEW
rpath         delete    PATH
rpath         append    PATH
rpath         insert    PATH
```

**Logical** — does dependent work not visible in the line:

```
dylib   delete   PATH    renumbers survivors; refuses if any symbol still binds
dylib   insert   PATH    renumbers every dylib after it
fixups  lower            rebuilds __LINKEDIT's bind and rebase streams
```

Three statements, and the code already said which three.
`docs/PROPOSAL.md`'s own note: *"Appending is safe because it only hands out new
indices, but INSERTING or DELETING shifts every later one."* `LC_RPATH` carries
no ordinal, so every rpath operation is physical.

`load-command delete`'s five-kind vocabulary is not a convenience list. It was
selected as the set with no non-local consequence, and `src/lc_kinds.c` says so:
*"purely informational, or invalidated the moment the binary is rewritten …
None of them carries a library ordinal, so stripping never disturbs
change_dylib's ordinal renumbering."* That table is the physical layer,
isolated before anyone named it.

**Why the levels are marked rather than composed.** Logical statements are not
macros over physical ones and the language does not pretend otherwise. Ordinal
renumbering is data-dependent — which ordinals shift depends on which symbols
currently bind to which dylib — so it cannot be written as a static expansion.
A language that offered `--explain` and then could not explain its three most
consequential statements would claim more than it delivers.

## Directives

Properties of the run, not steps in it, so they read as declarations:

```
allow-grow          permission to enlarge the header pad by lowering the image
                    base if the new load commands do not fit. Opt-in and
                    failing by default. MH_EXECUTE + MH_PIE only (src/grow.c:846-853)
fatal-warnings      an operation that matched nothing is an error, not a report
```

`allow-grow` keeps its name to match `ld`'s permission convention
(`--allow-multiple-definition`, `--allow-shlib-undefined`). It stays distinct
from the standalone `grow FILE N` verb, which is an instruction rather than a
permission.

`fatal-warnings` matches `ld` and `gas`, and GCC's `-Werror`. An operation that
matched nothing genuinely is a warning; this promotes it.

**Directives must precede every operation.** They describe the run, so a reader
should not have to reach line 40 to learn that growth was permitted, and the
parser should not have to decide what a directive appearing after the statement
it would have governed means. A directive below an operation is a parse error.
Repeating a directive is not — it is idempotent and harmless.

## File format

One statement per line. Fields are whitespace-separated, with single- and
double-quote grouping and backslash escapes — shell word rules. `#` begins a
comment except inside quotes. Blank lines are ignored.

This matches `objcopy --redefine-syms=FILE`, binutils' own recipe-file
convention, in comment character and blank-line handling. Shell quoting is not
an arbitrary pick either: `compat/translate.sh`'s `mt_quote` already emits
exactly this, and it has been through a hostile-argument battery — paths
containing `$( )`, backticks, semicolons, globs, spaces and leading dashes — so
the generator and the parser cannot drift.

```
# port-claude.m9
allow-grow
fixups        lower
version-min   set      10.9
load-command  delete   uuid
load-command  delete   codesig
dylib         replace  /usr/lib/libSystem.B.dylib  @loader_path/../S.dylib
dylib         replace  /usr/lib/libicucore.A.dylib @loader_path/../I.dylib
dylib         replace  /usr/lib/libc++.1.dylib     @loader_path/../c++.1.dylib
```

## Execution model

1. **Parse the whole recipe.** Any syntax or vocabulary error is reported before
   the file is opened for writing.
2. **Read the image once.**
3. **Apply each statement in order** against the in-memory buffer.
4. **Verify** the finished image.
5. **Write once** — atomically to `FILE`, or to `--output`.

Sequential rather than collapsing the recipe into one operation set, because
`fixups lower` cannot batch with anything (later statements must see the lowered
image), and because "run in sequence" is what a reader will assume. The cost is
rebuilding the load-command table once per statement; on a 200MB binary that
table is a few KB and the expensive part is I/O, which happens once either way.

**Any statement failing means nothing is written.** That is the property the
whole design exists to buy.

`rewrite` uses the verbs' existing exit codes, unchanged: `0` on success, `2`
where it examined the file and declined on purpose (a refused statement, a
failed verify, or an unmatched operation under `fatal-warnings`), `1` for an
operational failure (a syscall, a malloc, an unparseable recipe). A recipe that
asked for nothing the image needed still succeeds, and says so, exactly as a
single verb does today.

## Verification

Mandatory, after the last statement and before the write. **No escape hatch.**

This is viable only because of a finding from the spike that preceded this
design: `mg_plausible` refuses every dylib and bundle at a precondition that
mistakes a legitimate image base of 0 for "no segment maps the header", so the
check it exists to perform never runs. Until that is fixed, a mandatory gate
would be a mandatory rubber stamp. Task 0 of
`docs/superpowers/plans/2026-09-10-report-what-macho9-did.md` fixes it.

No opt-out, deliberately: a `MACHO_NO_VERIFY` escape was removed from a shipped
wrapper during the compat-tool retirement, on the grounds that a
disabled-safety-check shaped hole in a shipped artifact is worse than the false
positives it hides. Reintroducing one at a higher level would undo that.

## The recipe is the plan

`docs/PROPOSAL.md` says `port` *"can plan across families"*, implying the tool
derives the ordering. **This design does not reorder statements.** They run in
the order written.

If `load-command delete uuid` frees the pad a later `dylib replace` needs, the
caller wrote them in that order deliberately. `compat/translate.sh` already
knows to emit `lc` before `dylib`, so the planning lives in the generator that
has the context for it, and the tool keeps a property that is one sentence to
state and one test to check.

## Naming

| was | is | why |
|---|---|---|
| `port` | `rewrite` | "port" collides with Mach ports, in a Mach-O tool. `rewrite` is this codebase's own word — `src/rewrite.c`, prefix `mr_`, *"rewriting a Mach-O's dylib load commands and LC_RPATHs in place"* — and separates cleanly from `info` and `verify`, the verbs that do not rewrite |
| `lc` | `load-command` | matches `otool -l`'s own term and the spec's stated principle that "the family is a subcommand, the operation is a flag, and both are always explicit". Also retires the `-strip-lc` spelling, which invited confusion with binutils' `strip` (symbols and debug info) |
| `declassify` | `fixups lower` | gives it a family, where it was the one family-less verb. "Lower" is the compiler term for translating to a more primitive representation, which is exactly what it does: chained fixups (macOS 12+) down to `LC_DYLD_INFO_ONLY` (10.6+) |
| `--for 10.9` | dropped | `version-min set 10.9` says it plainly, and `--for` was the only justification "port" had left |

`docs/PROPOSAL.md`'s "Why these names" section explains every naming choice it
made except `declassify` and `port` — the two this design replaces.

## binutils alignment

**Matched:** `objcopy`'s `infile [outfile]` shape, where omitting the output
modifies in place via temp-and-rename. `--redefine-syms=FILE`'s recipe-file
conventions. `rename`, from `--rename-section`. `--fatal-warnings`, from `ld`
and `gas`. `--allow-*`, from `ld`.

**Deliberately not matched:** `--remove-section` and `--strip-all` — Apple says
*delete* and *load command*, and Mach-O people reading `otool -l` are the
audience. `--wildcard` — patterns that match one more command than the author
expected are the silent-success class this toolkit exists to eliminate.

**Reserved:** `arch`. If a recipe ever targets one slice of a fat container, the
vocabulary to match is `lipo`'s (`-arch`, `-thin`, `-extract`, `-remove`), not
binutils' `--target`/`-I`/`-O`, which is a different concept. Do not spend the
word elsewhere.

**Knowingly divergent:** `objcopy` frames address manipulation mechanically
(`--adjust-vma`, `--change-addresses`). `allow-grow` and the `grow` verb frame
it by intent — enlarge the header pad — with lowering the image base as the
means. The pad is what a caller cares about here.

## Known wart

Exit codes are `0` ok, `1` failed, `2` refused. binutils uses 0 and 1 only, and
`grep` — the tool most shell authors have in mind — uses `2` for *error* where
this uses it for *deliberate refusal*. It is documented in `--capabilities` and
relied on by the compat wrappers, so changing it now would be a compatibility
break. It will surprise someone; this is where it is written down.

## Out of scope

- Conditionals, variables, includes. The operations are already safe no-ops on
  inapplicable input — `fixups lower` passes an already-lowered binary through
  unchanged, and a `replace` that matches nothing does nothing — so a predicate
  buys almost nothing for a real parser's cost.
- Per-slice targeting of fat containers. See "Reserved" above.
- Symbol-table surgery (`objcopy --redefine-sym`, `--localize-symbol`). The
  toolkit touches `nlist` only as a consequence of ordinal renumbering and
  exposes no symbol verbs.
- `--explain` / expansion of logical statements into physical ones. See "Why the
  levels are marked rather than composed".

## Consumers

`compat/translate.sh` emits a `rewrite` recipe whenever a translated invocation
needs more than one `macho9` command, and the plain verb otherwise. That targets
exactly the case that measured harm without churning single-command paths.

`compat/patch_macho.sh` can become a `rewrite --output` call, since `--output`
subsumes its `IN OUT` shape.

Once both hold, `compat/macho9-compat.sh`'s temp-copy-and-install dance has no
remaining caller and can go, taking the extra 200MB with it.
