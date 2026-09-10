# Report What macho9 Did — and Retire fix_macho Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Un-blind the safety gate that has been refusing every dylib for the
wrong reason; make `macho9` say which requested operations matched nothing; and
use that to retire `compat/fix_macho.c`, the last C rewriting tool, so `macho9`
becomes the only Mach-O rewriting binary this repo ships.

**Architecture:** Task 0 is an unrelated safety fix that jumped this queue: a
spike found `mg_plausible` refuses every dylib at a precondition that mistakes a
legitimate image base of 0 for "no segment maps the header", so the check it
exists to perform never runs. Then: the rewriter already reports what it *did* (`Change [...]`,
`Delete [...]`, `Insert [...]`). It says nothing about what it was asked to do
and didn't. Task 1 adds per-operation hit accounting inside `src/rewrite.c` and
reports unmatched operations on **stderr**, so the five existing wrappers'
stdout stays byte-identical. Task 2 adds an opt-in strict mode that turns an
unmatched operation into a non-zero exit. Task 3 replaces `compat/fix_macho.c`
with a `/bin/sh` wrapper — possible only now, because the repo owner has ruled
that `fix_macho`'s divergences from the shared drivers are improvements to
adopt deliberately rather than behaviour to preserve.

**Tech Stack:** C89-compatible C built by stock 10.9 AppleClang 6.0; POSIX
`/bin/sh` for the wrappers; CMake + CTest; shell test suites under `tests/`.

**Spec:** `docs/PROPOSAL.md`. Its "verify" section is the argument for this
work: *"Every defect found in this code has been a silent success — each tool
reported OK and the binary died in the loader, or worse, did not."* An
operation that matches nothing and exits 0 is that shape.

## Why this plan exists — the measured gap

```
$ macho9 dylib FILE -replace /usr/lib/libSystem.B.dylib /tmp/new.dylib \
                    -replace /nope/absent.dylib /also/absent.dylib
  Change [56->56 bytes]: /usr/lib/libSystem.B.dylib -> /tmp/new.dylib
FILE: updated (sizeofcmds=1296, 8528 bytes)
rc=0
```

The second `-replace` matched nothing. Neither the output nor the exit code
says so. Ask for three replacements, get two, no way to tell.

`src/rewrite.c:679` handles the all-miss case (`nothing to change.`), so the
*total* miss is reported. The partial miss is not.

## The decision this plan rests on

The previous plan (`2026-09-09-retire-the-compat-tools.md`) left `fix_macho` as
C because a **wrapper must preserve behaviour**, and `fix_macho`'s behaviour
differs from the shared drivers in ways a wrapper would silently change. The
repo owner has since ruled that those differences are improvements to adopt.
That ruling is what makes Task 3 possible, and the plan the previous one could
not finish now finishes.

The four adopted changes, each named at its site in Task 3:

| what | `fix_macho` today | shared drivers | why adopting is right |
|---|---|---|---|
| a longer replacement path | refuses: `new path '...' too long (320 > 32)` — the check at `compat/fix_macho.c:211-215` is `new_len + 1 > dc->cmdsize - name_off`, i.e. it must fit **the existing command** | resizes the command into existing header padding | an artificial limit: its rewriter never learned to resize a command. `docs/PROPOSAL.md` records no safety reason for it, and the plan's predecessor states the rule — *"`macho9` does NOT have to inherit the old tools' artificial limits"* |
| chained `-rename_seg A B -rename_seg B C` | produces `B`: the rename loop `break`s on the first match per segment, so the second pair never fires | produces `C`: each pass sees the previous pass's output | doing what was asked |
| write-back | `lseek`+`write` straight into the file | `wa_write_atomic` (mkstemp + rename, symlink-safe, hard-link-aware, xattr-preserving) | a crash mid-write no longer leaves a corrupt binary |
| a fat slice it cannot handle | prints `Skipping arch %u` and carries on, exit 0 | refuses the whole file | refuse rather than guess |

**Nothing outside this repo calls `fix_macho`.** The caller survey in the
previous plan's "Known callers" section established this from evidence:
`mavericksforever.com/claude/install.sh` does not name it, and no checkout under
`~/Documents/code/trees/mavericks-*` invokes it. Its only caller is this repo's
own `tests/change_dylib_test.sh`.

## Global Constraints

- **`tests/EXPECTED` is a characterization reference**, never edited. If
  `tests/characterize.sh` stops reproducing
  `ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792`, behaviour
  changed and the task has failed.
- **The five existing wrappers' stdout must stay byte-identical.** New
  reporting goes to **stderr**. `tests/known-callers.sh` and
  `tests/wrapper_test.sh` are the gates.
- **The tools must never move a byte of file data.** Two reviewed exceptions
  stay: `-grow`'s memmove after the load commands, and the export-trie append
  past `__LINKEDIT`'s end.
- **POSIX `/bin/sh` only in the wrappers** — 10.9's shell is old and these run
  on the target. No bashisms; `/bin/sh -n` and `/bin/ksh -n` must pass.
- **Stock 10.9 AppleClang 6.0**; no post-10.9 APIs. The build must be
  warning-free — check by grepping build output for `warning:` as well as
  `error:`.
- **A comment or doc that claims more than the code does is a defect.** The
  predecessor plan spent five review passes making one comment's claim exactly
  true. Write the weaker true thing.

## File Structure

| file | responsibility | task |
|---|---|---|
| `src/image.h`, `src/image.c` | **new** `mi_image_base()` — the base a caller can distinguish from "no header-mapping segment" | 0 |
| `src/grow.c` | two call sites stop treating a legitimate base of 0 as "not found" | 0 |
| `src/rewrite.c` | hit accounting inside `mr_build_lcs`; unmatched report in `mr_apply_file` | 1, 2 |
| `src/rewrite.h` | `mr_ops` gains `strict_unmatched`; the layout tripwire's literals move | 2 |
| `cli/macho9.c` | `--strict` flag on `dylib`/`rpath`/`lc`; `--capabilities` advertises it | 2 |
| `compat/fix_macho.sh` | **new** — the wrapper | 3 |
| `compat/fix_macho.c` | **deleted** | 3 |
| `compat/translate.sh` | `fix_macho` translation; the chained-rename refusal is **removed** | 3 |
| `tests/cli_test.sh` | assertions for the report and for `--strict` | 1, 2 |
| `tests/wrapper_test.sh` | `fix_macho` wrapper assertions | 3 |
| `tests/translate_test.sh` | chained-rename now translates rather than refusing | 3 |
| `tests/change_dylib_test.sh` | its `fix_macho` cases move to the wrapper's behaviour | 3 |
| `CMakeLists.txt` | `fix_macho` moves from `MACHO_TOOLS` to `MACHO_WRAPPERS` | 3 |

---

### Task 0: Un-blind the safety gate on dylibs

**Files:**
- Modify: `src/image.h`, `src/image.c` (add `mi_image_base`)
- Modify: `src/grow.c` (`mg_collect` ~`:203-204`, `mg_plausible` ~`:679-680`)
- Test: `tests/cli_test.sh`, `tests/change_dylib_test.sh`

**Interfaces:**
- Consumes: nothing.
- Produces: `int mi_image_base(const mi_image *im, uint64_t *out);` — returns 0
  and sets `*out` when a segment maps the header (the base may legitimately be
  0), or -1 when none does. `mi_text_base` is **unchanged** and keeps its third
  caller; do not reroute it.

**The diagnosis, so you do not have to re-derive it.** `mg_plausible` refuses
every dylib on this machine — all 26 thin 64-bit dylibs in `/usr/lib` — and the
reason has nothing to do with plausibility. It bails at its precondition
`if (!base) return -1`, where `base` comes from `mi_text_base`. **Dylibs are
linked at image base 0**, and `mi_text_base` uses 0 as its "no segment maps the
header" sentinel. The `LC_FUNCTION_STARTS` heuristic never runs.

The symptom is visible from outside, which is worth knowing when you write the
tests:

```
$ macho9 verify <a thin 64-bit /usr/lib dylib>
<file>: FAILED (see above)          <- nothing above it: no entry was checked
$ macho9 info <same>
<file>: ... filetype=6
  segname=__TEXT vmaddr=0x0 ...
$ macho9 verify /bin/ls
/bin/ls: OK                          <- filetype=2, __TEXT vmaddr=0x100000000
```

Forcing `__TEXT.vmaddr` to `0x100000000` in a scratch copy makes the
**unmodified** gate pass all 20 measured samples with zero misses
(`libc++.1.dylib` alone: 481/481 function-naming entries). The heuristic is
sound; the precondition is wrong.

**This has been hit before and misdiagnosed.** `tests/change_dylib_test.sh`
around `:609-615` works around it with `MACHO_NO_VERIFY=1`, explaining that the
fixture's *"plain `__TEXT` layout doesn't satisfy `mg_plausible`'s
`LC_FUNCTION_STARTS` heuristic … so it's not something the ordinal fix
introduces … opts out of that unrelated gate."* Half right: it does fail
regardless of any rewrite. But the heuristic was never unsatisfied — it never
ran — and the fixture is a dylib. That workaround is your regression test.

**Blast radius.** `mg_verify` and `mg_snapshot_take` carry the same guard, so
the grow path's verification is equally blind on every dylib. And because
`mr_process_thin` gates on `mg_plausible`, `change_dylib` and `macho9
dylib`/`rpath`/`lc` have been **refusing every dylib outright**, for a reason
unrelated to safety. It went unnoticed because the production workload is an
executable.

- [ ] **Step 1: Write the failing tests**

Two, in `tests/cli_test.sh`. Build the dylib fixture with the suite's own
fixture machinery rather than reaching into `/usr/lib` — a test that scans the
host for a suitable binary is a test that skips itself away on the cross
runner, which this repo has been bitten by.

```sh
# A dylib links at image base 0, and mg_plausible used to read that 0 as
# mi_text_base's "no segment maps the header" sentinel and refuse before
# checking anything. The tell was `FAILED (see above)` with nothing above it.
"$BIN/macho9" verify "$T/fixture.dylib" >"$T/dylibverify.out" 2>&1
expect_eq "verify: a dylib gets a real verdict" 0 "$?"
expect_grep "verify: and says OK" "OK" "$T/dylibverify.out"
expect_not_grep "verify: not the contentless failure" \
    "FAILED (see above)" "$T/dylibverify.out"

# The gate refusing every dylib meant no dylib could be rewritten at all.
cp "$T/fixture.dylib" "$T/dylibrw"
"$BIN/macho9" lc "$T/dylibrw" -delete uuid
expect_eq "lc -delete: a dylib is rewritable" 0 "$?"
```

- [ ] **Step 2: Run them and watch them fail**

Run: `sh tests/cli_test.sh <bindir>`
Expected: FAIL — `verify` reports `FAILED (see above)`, and `lc -delete` is
refused with "base-relative offsets that name no known function".

- [ ] **Step 3: Add `mi_image_base`**

```c
/* The image's base vmaddr -- the vmaddr of the segment that maps the header,
 * which is __TEXT in every image this toolkit handles.
 *
 * Separate from mi_text_base because that function returns 0 BOTH for "no
 * segment maps the header" and for "the base is 0", and a dylib's base
 * legitimately IS 0: dylibs are linked at zero and slid at load time. Callers
 * that use the base as a precondition need to tell those apart, and the two
 * that did not were refusing every dylib on the machine.
 *
 * Returns 0 with *out set (which may be 0), or -1 if no segment maps the
 * header. */
int mi_image_base(const mi_image *im, uint64_t *out);
```

- [ ] **Step 4: Use it at exactly two sites**

`src/grow.c`'s `mg_collect` (~`:203-204`) and `mg_plausible` (~`:679-680`).
**Leave `mi_text_base` and its remaining caller alone** — `src/grow.c:893`
uses it where a 0 base is genuinely uninteresting, and rerouting it would widen
this change past what the evidence supports.

- [ ] **Step 5: Run the new tests, then the whole suite**

Run: `cmake --build --preset native-local && ctest --preset native-local`
Expected: the two new assertions PASS; 13/13 overall.

**Watch for a newly-refused input.** Un-blinding `mg_verify` and
`mg_snapshot_take` means the grow path now actually checks dylibs. If something
that used to pass now refuses, that is a **finding to report, not to paper
over** — it may be the gate doing its job for the first time on that path. Say
what refused and why before changing anything.

- [ ] **Step 6: Delete the workaround that was the bug's fingerprint**

Remove `MACHO_NO_VERIFY=1` from `tests/change_dylib_test.sh:615` and rewrite the
comment above it to say what was actually wrong. The case must pass without the
escape hatch — that is the regression test for this whole task.

- [ ] **Step 7: Confirm the digest and the callers**

Run: `sh tests/characterize.sh <bindir> check` → `ad12bdd7...`;
`sh tests/known-callers.sh <bindir>`; `sh tests/wrapper_test.sh <bindir>`.
`tests/fixture.macho` is `filetype=2` at `vmaddr=0x100000000`, so the digest
should not move. If it does, stop and report.

- [ ] **Step 8: Commit**

```bash
git add src/image.h src/image.c src/grow.c tests/cli_test.sh \
        tests/change_dylib_test.sh
git commit -m "fix: mg_plausible refused every dylib on a base-of-zero sentinel"
```

---

### Task 1: Report operations that matched nothing

**Files:**
- Modify: `src/rewrite.c` (`mr_build_lcs`, `mr_process_thin`, `mr_process_fat`, `mr_apply_file`)
- Test: `tests/cli_test.sh`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: no public API change. `mr_apply_file`'s signature and `mr_ops`'
  layout are **unchanged** — this task is deliberately invisible to the
  layout tripwire at `src/rewrite.c`. Task 2 is the one that trips it.

**Design.** `mr_build_lcs` is `static` and already takes a `verbose` int, so
extra parameters are free. Give it three caller-owned counter arrays and
increment the matching index everywhere `matched`/`rmatched`/a strip match
fires. `mr_apply_file` owns the arrays (sized `MR_MAX_OPS`/`MR_MAX_STRIP`,
zeroed once), passes them down through `mr_process_thin`/`mr_process_fat`, and
reports after every slice has run — **not** per slice, because an operation
that matched in one fat slice and not another has matched.

Three operation kinds can miss: `dylib_changes`, `rpath_changes`, `strip_cmds`.
Appends and inserts always act; do not report them.

- [ ] **Step 1: Write the failing test**

Add to `tests/cli_test.sh`, in the `dylib` section:

```sh
# A -replace naming a path the image does not have matched nothing, and the
# tool used to say so nowhere: stdout reported only the replace that DID fire
# and the exit code was 0. Ask for two, get one, no way to tell -- the silent
# partial success docs/PROPOSAL.md's `verify` section exists to rule out.
cp "$T/fixture" "$T/unmatched"
out=$("$BIN/macho9" dylib "$T/unmatched" \
        -replace /usr/lib/libSystem.B.dylib /tmp/new.dylib \
        -replace /nope/absent.dylib /also/absent.dylib 2>"$T/unmatched.err")
rc=$?
expect_eq "unmatched -replace: still succeeds" 0 "$rc"
expect_grep "unmatched -replace: names the op that missed" \
    "/nope/absent.dylib" "$T/unmatched.err"
expect_grep "unmatched -replace: says it matched nothing" \
    "matched nothing" "$T/unmatched.err"
expect_not_grep "unmatched -replace: the report is NOT on stdout" \
    "matched nothing" <<<"$out"
expect_grep "unmatched -replace: the op that DID match still reported" \
    "libSystem.B.dylib -> /tmp/new.dylib" <<<"$out"
```

Use whatever assertion helpers `tests/cli_test.sh` already defines; if
`expect_not_grep` does not exist, write the equivalent with the file's own
idiom rather than inventing a helper.

- [ ] **Step 2: Run it and watch it fail**

Run: `sh tests/cli_test.sh <bindir>`
Expected: FAIL on "names the op that missed" — nothing is written to stderr today.

- [ ] **Step 3: Thread the counters through `mr_build_lcs`**

Add three parameters and increment at each match site. The existing match
points are `src/rewrite.c:225` (`matched = c`), `:259` (`rmatched = c`) and the
strip-kind comparison in the same callback.

```c
/* Per-operation hit counts, so mr_apply_file can name the operations that
 * matched nothing. Caller-owned and caller-zeroed; sized MR_MAX_OPS /
 * MR_MAX_STRIP, which is what both front-ends cap at. NULL is legal and
 * means "do not count" -- the sizing pass passes NULL, because counting a
 * dry run would double every hit. */
static int mr_build_lcs(const mi_image *im, const mr_ops *ops,
                        uint8_t *new_lcs, uint32_t *out_off, uint32_t *out_ncmds,
                        int *out_mods, int *out_renames, int verbose,
                        int *hit_dylib, int *hit_rpath, int *hit_strip);
```

**The sizing pass must pass NULL.** `mr_process_thin` calls `mr_build_lcs`
twice — once to size (`verbose == 0`, `src/rewrite.c:731`) and once for real.
Counting both would report a miss as a hit.

- [ ] **Step 4: Accumulate and report in `mr_apply_file`**

```c
/* Reported once per FILE, after every slice has run -- not per slice. An
 * operation that matched in one fat slice and not another has matched; a
 * per-slice report would call that a miss on every slice but one. */
static void mr_report_unmatched(const mr_ops *ops, const int *hit_dylib,
                                const int *hit_rpath, const int *hit_strip) {
    for (int i = 0; i < ops->n_dylib_changes; i++)
        if (hit_dylib[i] == 0)
            fprintf(stderr, "macho9: %s matched nothing\n",
                    ops->dylib_changes[i].old_path);
    for (int i = 0; i < ops->n_rpath_changes; i++)
        if (hit_rpath[i] == 0)
            fprintf(stderr, "macho9: rpath %s matched nothing\n",
                    ops->rpath_changes[i].old_path);
    for (int i = 0; i < ops->n_strip_cmds; i++)
        if (hit_strip[i] == 0)
            fprintf(stderr, "macho9: no load command of kind %s to delete\n",
                    lc_kind_name(ops->strip_cmds[i]));
}
```

`lc_kind_name` does not exist yet — `src/lc_kinds.h` holds the table that maps
names to `LC_*` values. Add the reverse lookup there, next to the table, rather
than hand-rolling a switch here.

- [ ] **Step 5: Run the test and the suites**

Run: `cmake --build --preset native-local && ctest --preset native-local`
Expected: PASS, 13/13, no new warnings.

- [ ] **Step 6: Confirm the wrappers' stdout is untouched**

Run: `sh tests/known-callers.sh <bindir> && sh tests/wrapper_test.sh <bindir>`
Expected: PASS. This is the gate on "stderr, not stdout" — if a wrapper's
stdout changed, one of these fails.

- [ ] **Step 7: Confirm the digest**

Run: `sh tests/characterize.sh <bindir> check`
Expected: `characterize: OK (ad12bdd7...)`

- [ ] **Step 8: Commit**

```bash
git add src/rewrite.c src/lc_kinds.c src/lc_kinds.h tests/cli_test.sh
git commit -m "feat: macho9 says which operations matched nothing"
```

---

### Task 2: `--strict` — an unmatched operation is an error

**Files:**
- Modify: `src/rewrite.h` (`mr_ops`, the layout tripwire), `src/rewrite.c`, `cli/macho9.c`
- Test: `tests/cli_test.sh`

**Interfaces:**
- Consumes: Task 1's `mr_report_unmatched` and its counter arrays.
- Produces: `mr_ops.strict_unmatched` (int, 0 = report only, non-zero = refuse);
  `macho9 dylib|rpath|lc FILE --strict OP...`; `--capabilities` gains
  `flags=strict` on those three verbs.

**Expect the tripwire to fire, and treat that as it working.** Adding a field
to `mr_ops` changes `sizeof(mr_ops)` and so fails the negative-size typedef in
`src/rewrite.c` on purpose. That assertion exists to make you re-read
`mr_is_rename_only`'s conjunction and decide whether the new field belongs in
it. It does: a rename-only run with `strict_unmatched` set is still rename-only
for the purpose of skipping `mg_plausible`, **but the conjunction must name the
field explicitly** so the predicate does not silently widen. Update the
conjunction and the tripwire's literals in the same commit, and re-read that
comment block before you touch it — it took five review passes to state its
claim accurately.

- [ ] **Step 1: Write the failing test**

```sh
# --strict turns "matched nothing" from a report into a refusal. Without it a
# caller asking for three replacements and getting two has to parse stderr to
# find out; with it the exit code says so. The file is still written -- strict
# reports after the fact, it does not roll back.
cp "$T/fixture" "$T/strict"
"$BIN/macho9" dylib "$T/strict" --strict \
    -replace /nope/absent.dylib /also/absent.dylib 2>"$T/strict.err"
expect_eq "strict: an unmatched op is EX_REFUSED" 2 "$?"
expect_grep "strict: still names the op" "/nope/absent.dylib" "$T/strict.err"

# Without --strict the same command succeeds, so the flag is what changed the
# answer and not something else.
cp "$T/fixture" "$T/lax"
"$BIN/macho9" dylib "$T/lax" -replace /nope/absent.dylib /also/absent.dylib \
    2>/dev/null
expect_eq "no --strict: an unmatched op still succeeds" 0 "$?"

# --capabilities advertises it, so a wrapper can probe rather than assume.
"$BIN/macho9" --capabilities | grep -q '^verb dylib .*flags=.*strict' \
    && ok "capabilities: dylib advertises strict" \
    || bad "capabilities: dylib does not advertise strict"
```

- [ ] **Step 2: Run it and watch it fail**

Run: `sh tests/cli_test.sh <bindir>`
Expected: FAIL — `--strict` is an unknown operation today.

- [ ] **Step 3: Add the field, and fix the tripwire it breaks**

```c
    int              strict_unmatched;  /* refuse if any operation matched nothing */
```

Build. The negative-size typedef fails. Recompute `sizeof(mr_ops)` and
`offsetof(mr_ops, allow_grow)` with a probe rather than by arithmetic, update
the literals, and add `strict_unmatched` to `mr_is_rename_only`'s conjunction
so a new field cannot silently widen what skips `mg_plausible`.

- [ ] **Step 4: Make the report a refusal**

Have `mr_report_unmatched` return the number of unmatched operations, and in
`mr_apply_file` refuse when `ops->strict_unmatched` and that count is non-zero.
Return the same code a deliberate refusal uses elsewhere so
`cli/macho9.c` maps it to `EX_REFUSED` — read how `MR_ERROR` is currently
mapped before choosing, and say in the report what you chose and why.

**The file is still written.** Strict reports after the rewrite, it does not
roll back. Say so at the site and in `--capabilities`' comment block, because a
reader will assume otherwise.

- [ ] **Step 5: Parse `--strict` in the CLI**

`cli/macho9.c`'s `cmd_dylib_or_rpath` already handles `--allow-grow` as a
verb-level flag in its operation loop; add `--strict` the same way. `cmd_lc`
needs it too. Do **not** add it to `segment` or `retag-swift`: neither takes a
list of operations that could miss.

Add `flags=strict` to those three verbs in `print_capabilities`, alongside the
existing `flags=allow-grow`.

- [ ] **Step 6: Run the suites**

Run: `cmake --build --preset native-local && ctest --preset native-local`
Expected: PASS 13/13, no warnings.

- [ ] **Step 7: Confirm the wrappers did not inherit strictness**

Run: `sh tests/known-callers.sh <bindir> && sh tests/wrapper_test.sh <bindir>`
Expected: PASS. The five wrappers must **not** pass `--strict` — `change_dylib`
has always exited 0 on an unmatched `-change`, and that is compat surface.

- [ ] **Step 8: Commit**

```bash
git add src/rewrite.h src/rewrite.c cli/macho9.c tests/cli_test.sh
git commit -m "feat: macho9 --strict refuses when an operation matched nothing"
```

---

### Task 3: `fix_macho` becomes a wrapper — the last C rewriting tool retires

**Files:**
- Create: `compat/fix_macho.sh`
- Delete: `compat/fix_macho.c`
- Modify: `compat/translate.sh`, `CMakeLists.txt`, `tests/translate_test.sh`,
  `tests/wrapper_test.sh`, `tests/change_dylib_test.sh`, `compat/README.md`,
  `README.md`
- Test: `tests/wrapper_test.sh`, `tests/translate_test.sh`

**Interfaces:**
- Consumes: Task 1's unmatched reporting — it is what lets the wrapper tell a
  caller that a `-change` matched nothing, which `fix_macho`'s own stdout said
  via `No changes needed: %s`.
- Produces: `fix_macho` installed as a `/bin/sh` wrapper; `compat/` contains
  no `.c` files.

**Translation.** All three flags map directly:

| `fix_macho` | `macho9` |
|---|---|
| `-change OLD NEW` | `dylib FILE -replace OLD NEW` |
| `-strip_build_version` | `lc FILE -delete build-version` |
| `-rename_seg OLD NEW` | `segment FILE OLD NEW` |

Ordering matters the same way it does for `change_dylib`: `lc` first, so the
stripped command's bytes are available to the rewrite. `compat/macho9-compat.sh`
already owns that sequencing and the temp-copy atomicity dance — use it rather
than writing a fourth copy.

- [ ] **Step 1: Remove the chained-rename refusal, with its test**

`compat/translate.sh`'s `mt_fm_chain` refuses `-rename_seg A B -rename_seg B C`
because the C tool produced `B` and the translation produces `C`. That refusal
was correct while a wrapper had to preserve behaviour. It is now the wrong
answer: chaining is one of the four adopted improvements.

Delete `mt_fm_chain` and its call site. Change `tests/translate_test.sh`'s
`fm-chain` and `fm-chain-empty` assertions from `refuses()` to `ok()` with the
exact emitted command lines. Leave a comment at the site saying the refusal was
deliberate, why it existed, and which decision reversed it — a future reader
finding this in `git log` will otherwise think it was an oversight.

- [ ] **Step 2: Run it and watch the old assertions fail**

Run: `sh tests/translate_test.sh <bindir>`
Expected: the two `fm-chain` cases FAIL against the old expectations, proving
they were pinning the refusal and are now pinning the translation.

- [ ] **Step 3: Write the wrapper**

Model it on `compat/rename_segment.sh`, which is the closest shape — it already
maps an exit code and documents its divergences in a header block. The header
must carry all four adopted changes from this plan's table, each stated as a
deliberate change with its reason, in the "DELIBERATE DIVERGENCES" convention
the other wrappers use.

**Do not reproduce `fix_macho`'s stdout.** Its `Processing thin Mach-O:` /
`Changed: X -> Y` / `File updated: F` / `No changes needed: F` lines are
replaced by `macho9`'s own reporting plus Task 1's unmatched lines. Say so in
the header. This is a deliberate break and the only caller is this repo's own
test suite.

- [ ] **Step 4: Move `fix_macho` from tool to wrapper in CMake**

`CMakeLists.txt` splits `MACHO_WRAPPERS` and `MACHO_TOOLS`. Move the name, drop
its `add_executable` and `target_link_libraries`, and confirm
`.github/workflows/release.yml`'s artifact list still names `fix_macho` (it
lists names, not targets, so it should need no change — verify rather than
assume).

- [ ] **Step 5: Rewrite the `fix_macho` cases in `change_dylib_test.sh`**

That suite is `fix_macho`'s only caller. Its cases pin the old behaviour,
including case 8b's `No changes needed` assertion on an `LC_LOAD_UPWARD_DYLIB`
fixture. Update each to the wrapper's behaviour and say in a comment what
changed and why. **Do not delete a case to make it pass** — if a case cannot be
expressed against the wrapper, that is a finding to report, not a case to drop.

- [ ] **Step 6: Add wrapper assertions**

In `tests/wrapper_test.sh`, mirroring the other five wrappers' blocks: each of
the three flags, a fat container, the previously-refused longer replacement path
(now succeeding), and a chained rename (now producing the second name). Prove
each can fail.

- [ ] **Step 7: Run everything**

Run: `cmake --build --preset native-local && ctest --preset native-local`
Expected: 13/13. Then `sh tests/characterize.sh <bindir> check` →
`ad12bdd7...`, and `sh tests/known-callers.sh <bindir>` → all pass.

- [ ] **Step 8: Update the documents that say the goal is not met**

`README.md` and `compat/README.md` both say — correctly, until now — that this
repo ships **two** Mach-O rewriting binaries rather than the one the retirement
plan aims at. That callout was added deliberately and must now be replaced with
what is true: `macho9` is the only one, and `compat/` holds six shell wrappers
and no C. `compat/README.md`'s "What a packager has to change" section still
applies and should stay.

- [ ] **Step 9: Commit**

```bash
git add compat/fix_macho.sh compat/translate.sh CMakeLists.txt \
        tests/translate_test.sh tests/wrapper_test.sh \
        tests/change_dylib_test.sh compat/README.md README.md
git rm compat/fix_macho.c
git commit -m "feat: fix_macho becomes a wrapper; macho9 is the only rewriter"
```

---

## Self-Review

**Spec coverage.** `docs/PROPOSAL.md`'s "verify" section argues that every
defect in this code has been a silent success; Task 1 closes one the spec did
not specifically name because nobody had measured it. The spec's "Sequencing"
step 2 — *"`fix_macho` and `change_dylib` converge"* — is what Task 3 completes,
by retirement rather than by merging two implementations. The proposal's
`port` verb remains unbuilt and out of scope; the mixed-family sequencing it
would replace is still handled by `compat/macho9-compat.sh`'s temp-copy dance.

**Placeholders.** None. `lc_kind_name` is named as not-yet-existing with its
home identified (`src/lc_kinds.h`); every other symbol referenced already
exists in the tree.

**Type consistency.** `mr_ops.strict_unmatched` is introduced in Task 2 and
used only there. Task 1's three counter arrays are internal to `src/rewrite.c`
and deliberately do not touch `mr_ops`, so Task 1 cannot trip the layout
tripwire and Task 2 certainly does — which is the point.

**Measurable done.** `compat/` contains no `.c` files; `macho9` is the only
Mach-O rewriting binary installed; `macho9 dylib -replace` on a path the image
does not carry says so on stderr; `--strict` makes it an error; and
`tests/EXPECTED` has not moved.

**Known risk.** Task 3 changes what `fix_macho` does, on purpose, in four ways.
The evidence that this is safe is a caller survey, not a proof: if a caller
exists that the survey missed, it will see different behaviour rather than a
deprecation warning. The previous plan's Task 3 (translate-and-refuse) is the
mechanism that would have caught that, and it remains blocked on a repo the
owner does not control.
