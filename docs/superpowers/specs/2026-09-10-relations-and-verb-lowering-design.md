# Relations as data, and verbs that lower to scripts

**Status:** design, agreed 2026-09-10.

**Sequenced after** `docs/superpowers/specs/2026-09-10-edit-scripts-design.md` and
its plan. `MS_TABLE`, `ms_script` and `me_run` all come from that work; this
design has nothing to attach to until they exist.

## Why these two things are one design

The edit-scripts design listed them separately — "Relations — one place to record
what points at what" and "What this does to the CLI". They are the same move made
against two different kinds of knowledge:

> **One declaration, several consumers, and no way for the consumers to drift
> apart.** Applied to *what points at what*, it is the relation table. Applied to
> *the operation vocabulary*, it is verb lowering.

Doing them together is not bundling. The vocabulary table is where an operation
declares which relations it disturbs, so the relation half needs the merged table
the verb half produces. Split, the first half would have to invent a temporary
home for that column and the second would move it.

## This mechanism already exists here, twice

Worth stating before designing anything, because it changes the work from
"introduce a pattern" to "finish applying one".

**`src/linkedit.h`** holds one list of load commands whose entire `__LINKEDIT`
footprint is plain file-offset fields. `src/grow.c`'s `mg_classify_cb` and
`src/linkedit.c`'s `ml_bump_lc` both build their `case` labels from it, so a load
command cannot be added to grow's accept side without being added here, and
adding it here is what teaches `ml_bump_lc` to bump it. The coupling is enforced
as a **link error**, not merely a test — commit `247d09d`.

**`cli/machotool.c`'s `DYLIB_OPS`** (`:159-166`) carries, per operation, the flag
spelling, the arity, a capability name, and separate `dylib`/`rpath` columns
(`-reexport` is `DOP_REEXPORT` for dylib and `DOP_NONE` for rpath). The argument
parser and the `--capabilities` output are both generated from it.

So the question this design answers is not "should we do this" but "why is it
done in two places and not the other three".

## A correction to the edit-scripts spec

That document says `mg_verify`, `mg_snapshot_take` and `mg_plausible` "each
hand-roll their own version of 'is this still consistent', each with its own
hand-rolled applicability condition." **The first half is false.** All three call
`mg_collect` (`src/grow.c:197`), which is already the single walk over the
base-relative structures; `mg_snapshot_take` stores what it returns, `mg_verify`
compares two of them, and `mg_plausible` checks them against
`LC_FUNCTION_STARTS`. Those are three different invariants over one shared
collection, which is the right shape.

The second half is true and is what this design addresses: the **applicability
conditions** are hand-written and scattered, and one of them was the base-of-zero
bug that made `mg_plausible` refuse every dylib.

## Decision 1: derivation only

Relations declare **what is pointed at** and **when they are live**. Nothing
else. Repair code does not move, and no relation declares a check or a repair
function.

| relation | referent | repaired today by |
|---|---|---|
| library ordinal | the ordinal-carrying load-command subsequence | `mo_map_build`/`mo_map_apply` (`src/ordinals.c`) |
| base-relative values | the image base | `src/grow.c`'s re-base pass |
| file-offset fields | `__LINKEDIT`'s blobs | `src/linkedit.h`'s list, `src/grow.c`'s bump |
| initializer and unwind targets | `LC_FUNCTION_STARTS` | `mg_plausible` — checked, never repaired |
| `sizeofcmds` | the header pad | `mr_build_lcs`, `mg_grow_header` |

The two heavier options were considered and declined. Declaring a `check` and a
`repair` per relation would rewrite working, load-bearing code in `src/grow.c` to
buy uniformity this design does not need. A general engine that executes
consequences from a declared graph is the larger of the edit-script spec's "two
generalizations", and nothing yet demands it.

## Decision 2: what each operation disturbs

"Disturbs" means **changes the referent such that references to it go stale** —
not merely "writes bytes near it". The distinction is the whole content of the
column, so it is stated before the table rather than left to be inferred.

Every row below was checked against `src/rewrite.h`'s documented semantics rather
than reasoned from the operation's name. Two rows came out the opposite of the
first draft, which is why the check is recorded here.

| operation | disturbs | why |
|---|---|---|
| `segment rename` | nothing | name characters; no offsets, no commands |
| `swift-abi set legacy` | nothing | one tag bit per class record |
| `dylib reexport` | nothing | an **in-place promotion** of `LC_LOAD_DYLIB` to `LC_REEXPORT_DYLIB` (`rewrite.h:62`). Both are ordinal-carrying, so the subsequence's membership, order and length are all unchanged |
| `dylib append` | header pad | a new `LC_LOAD_DYLIB` "placed last" (`rewrite.h:85`); it takes the highest ordinal, so **no existing ordinal moves** |
| `dylib replace` | header pad, only if the new path is longer | keeps the command's position and its ordinal |
| `rpath` — all four | header pad | `LC_RPATH` carries no ordinal; only the command count changes |
| `load-command delete` | header pad | frees pad |
| `version-min set` | header pad | appends a command |
| `dylib insert` | ordinal subsequence, header pad | "placed first"; inserted dylibs "become ordinals 1..n" (`rewrite.h:76-87`), shifting every existing one |
| `dylib delete` | ordinal subsequence, header pad | removes a member, renumbering every survivor after it |
| `fixups set classic` | `__LINKEDIT`'s blobs, the image base | rebuilds the bind and rebase streams wholesale |

The header-pad column is not noise: "disturbs the header pad" is exactly the
condition under which an edit may not fit and `allow-grow` becomes relevant. It
is the cheapest relation to satisfy and the most commonly disturbed, which is why
it earns a column rather than a special case.

Two facts are then computed rather than maintained:

- **which operations carry follow-up work** — exactly those disturbing some
  relation's referent;
- **which checks apply to a run** — exactly those relations live in this image
  whose referent this run disturbs.

## Decision 3: `mr_is_rename_only` is deleted, and the deletion must be proved

`mr_is_rename_only` (`src/rewrite.c:641`) scopes `mg_plausible` out of rename-only
operations. It exists because the gate refuses real images for ordinary edits, and
it is written as "everything else is empty" — a conjunction over `mr_ops`' fields
with a **documented blind spot**: a new member of four bytes or fewer placed in
one of the struct's seven interior padding holes moves neither `sizeof` nor the
guarded offset, compiles clean, and is invisible to it. The layout tripwire at
`:638` catches the layout change, not the meaning change.

What that predicate actually asks is *"does this operation disturb anything
anyone points at?"* — which is what §2 computes. So it becomes derived, and the
hand-written version goes.

### The derivation narrows applicability, deliberately, and the narrowing is enumerated

`mr_is_rename_only` skips the gate for one shape. The derived rule skips it
whenever the run did not disturb the initializer-and-unwind relation — which, per
§2, only `fixups set classic` and a header grow do. So the new rule **skips checks
the old rule ran**, and a differential test demanding the two "agree everywhere"
would be incoherent.

The justification is `mg_plausible`'s own, generalized. Its comment
(`src/rewrite.c:872-880`) explains the rename exception as *"the gate cannot catch
anything a rename did; it can only re-decide a property the INPUT already had,
and refuse."* That is an argument about offsets, and it holds identically for
every operation below: none moves a section offset or rewrites
`LC_FUNCTION_STARTS`.

**Expected differences — the complete list. Any difference not on it is a stop.**

| op-set shape | old | new | why the skip is right |
|---|---|---|---|
| `swift-abi set legacy` alone | runs | skips | one tag bit per class record; no offset moves |
| `dylib reexport` alone | runs | skips | in-place promotion; count, order and ordinals all unchanged |
| `load-command delete` alone | runs | skips | frees header pad; no section offset moves |
| `version-min set` alone | runs | skips | appends a command; no section offset moves |
| any `dylib` or `rpath` op alone, without growth | runs | skips | changes the command region and possibly ordinals; no section offset moves |
| any combination of the above, without growth | runs | skips | the union disturbs no referent the gate checks |
| `fixups set classic` | runs | **runs** | rewrites `__LINKEDIT` and the image base |
| anything that grew the header | runs | **runs** | see below |
| rename-only | skips | skips | unchanged |

**Applicability is evaluated against what the run did, not only what it
declared.** A `dylib append` that overflows the pad and triggers a grow has
disturbed the base-relative relation, whichever statement asked for it. Declaring
this up front matters: computing applicability from the statement list alone would
skip the gate on exactly the runs that most need it.

**What this costs, stated plainly.** `mg_plausible` is defence against bugs in the
*rewriter*, not only against the operation's declared intent, so narrowing its
reach narrows that defence. The case the comment names as the reason it exists —
`patch_macho`'s chained-fixups conversion, ~94,900 rebases with no self-check of
its own — is preserved, because that conversion disturbs the relation. What is
given up is the chance of the gate incidentally catching a bug in an operation
that moves no offsets. That trade is the point of the design, not a side effect,
and it is written here so a reviewer weighs it rather than discovers it.

### One existing assertion this invalidates, and what replaces it

`tests/cli_test.sh:2109-2122` runs `lc -delete uuid` against `mkimplausible`'s
fixture and asserts it **is refused**, under the comment *"An ordinary operation
on it still meets the gate and is refused … so the skip below is narrow, not a
hole"* and the label *"an operation that CAN move an offset still meets the
gate"*. Its failure message is `"lc -delete uuid was NOT refused, so the gate is
gone"`.

The derived rule skips the gate for that operation, so this test fails — and
fails saying the gate is gone, which would read as a genuine regression.

Two things follow, and the plan must carry both:

**Its premise is already slightly false, and that is worth recording.** `lc
-delete` does not move a section offset: it frees header pad and `mr_build_lcs`
repacks the command region, leaving section file offsets where they were. The
label "an operation that CAN move an offset" overstates what the operation does —
the same class of claim this repo treats as a defect. The test has been passing
because the gate ran, not because the premise held.

**Do not delete it to make the suite green.** Its purpose — proving the skip is
narrow rather than a hole — is exactly the property this design most needs
asserted, since the skip is now much wider. Rewrite it against an operation that
genuinely disturbs the relation: `fixups set classic`, or any run that grows the
header. Same fixture, same "was NOT refused, so the gate is gone" failure message,
an operation for which the claim is true.

**The test that gates the deletion.** Both predicates run side by side across
every operation-set shape the suite exercises. A difference on the table above is
expected and asserted; a difference anywhere else fails. Only once that holds does
the old predicate come out. A comment claiming the two are equivalent is exactly
the kind of claim this repo treats as a defect when nothing would fail if it were
false.

## Decision 4: verbs build scripts

`DYLIB_OPS` and `MS_TABLE` merge into one table carrying, per operation: script
kind and operation, verb flag spelling, arity, which modes accept it, capability
name, and the referents it disturbs. One declaration feeds the verb parser, the
script parser, `--capabilities`, and §2's derivation.

Each `cmd_*` parses its `argv` into an `ms_script` **in memory** and calls
`me_run`. Not by generating script text: round-tripping `argv` through quoting
would put a quoting bug on the compat wrappers' path, where today it could only
reach `edit`.

**The verbs' own `printf` calls do not move.** The six wrappers' stdout must stay
byte-identical to the C tools they replaced; `tests/known-callers.sh` is the gate,
and it stays green because the output statements stay exactly where they are. What
changes is the path by which the edit gets applied, not the path by which it is
described.

## The objection this will draw

Putting an applicability predicate in front of `me_run`'s verification reads like
reintroducing the `MACHO_NO_VERIFY` escape that was deliberately removed from a
shipped wrapper during the compat retirement.

It is not the same thing, and the difference is the whole reason the removal
happened. **An escape hatch is caller-controlled**: a flag or an environment
variable, settable by whoever is invoking the tool, and therefore settable by
someone who wants a refusal to go away. **This is determined by the image and the
operations**, computed from declarations, with no input that a caller can supply
to switch it off. The check that does not run is the check that has nothing to
check — which is already why `mg_plausible` returns 0 when an image carries no
`LC_FUNCTION_STARTS`.

## Testing

- **The differential test of Decision 3**, above. It is the one that gates the
  deletion.
- **A tripwire on the merged table**, in the spirit of `linkedit.h`'s link error:
  adding an operation without declaring what it disturbs must fail to build, not
  silently declare "nothing". "Nothing" is a real and common answer, so it has to
  be spelled, not defaulted.
- `tests/known-callers.sh` and `tests/wrapper_test.sh` unchanged and green —
  the evidence that wrapper stdout did not move.
- `tests/characterize.sh` reproducing
  `ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792` — the
  evidence that no emitted byte changed.

## Out of scope

- **Declaring checks or repairs per relation**, and the general consequence
  engine. Decision 1 names both and declines them.
- **Removing any verb.** The verbs stay as sugar; the repo owner settled that
  redundancy is a feature when it keeps a transformation in one pass.
- **The module prefixes.** `mi_`, `mr_`, `mg_`, `mo_`, `mseg_`, `mswift_`, `wa_`
  and the new `ms_`/`me_` are opaque to a reader who has not learned them — the
  repo owner said so while approving this design. It is a real readability cost
  and it is not this design's to fix; the rename design
  (`2026-09-10-machotool-rename-and-target-design.md`) currently says prefixes
  stay, and reversing that is a decision for that document.
- **`tests/compat-matrix.tsv`.** A dated artifact whose other side no longer
  exists at HEAD; nothing here regenerates it.
