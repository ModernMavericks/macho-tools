# `machotool` Rename and `target 10.9` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the tool and the repo from `macho9` to `machotool`, add the `target 10.9` statement, and remove the option to be quiet.

**Architecture:** The rename is four mechanically distinct passes with different risk profiles — include guards, build targets and the binary, the two shared wrapper scripts, and prose — done in that order so the riskiest lands against an already-green tree. `target 10.9` is then one new statement kind that expands, in place, into statements the language already has. Verbosity stops being a flag.

**Tech Stack:** C99, stock 10.9 AppleClang 6.0, CMake + ctest, POSIX `/bin/sh`.

**Spec:** `docs/superpowers/specs/2026-09-10-machotool-rename-and-target-design.md`

**Depends on:** `docs/superpowers/plans/2026-09-10-edit-scripts.md` having landed. `target` is a statement in the edit-script language, so Task 5 has nothing to add it to until that exists. Tasks 1–4 could technically run earlier, but doing so would make the edit-scripts plan's every file path stale mid-review.

## Global Constraints

- **`tests/EXPECTED` is a characterization reference and is never edited.** `tests/characterize.sh` must keep reproducing `ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792`. A rename changes no emitted byte, so a moved digest is a real defect, not an expected consequence.
- **The six `compat/` wrappers' stdout must stay byte-identical**, and their names do not change: `patch_macho`, `change_dylib`, `add_version_min`, `fix_macho`, `rename_segment`, `retag_swift_classes`. `mavericksforever.com/claude/install.sh` fetches three of them by name. `tests/known-callers.sh` and `tests/wrapper_test.sh` are the gates.
- **The tools must never move a byte of file data.** Two reviewed exceptions: `-grow`'s memmove after the load commands, and the export-trie append past `__LINKEDIT`'s end.
- **POSIX `/bin/sh` only** in `compat/*.sh` and test scripts. No `[[`, `local`, `+=`, arrays, `<<<`, `$'...'`, `function`, `source`, `shopt`.
- **Stock 10.9 AppleClang 6.0, warning-free.**
- **A comment or doc that claims more than the code does is a defect.** This repo means it literally.
- **Module prefixes do not change.** `mi_`, `mr_`, `mg_`, `mo_`, `mseg_`, `mswift_`, `wa_`, `ms_`, `me_` all stay. They name modules, not the tool, and their readability is queue item 6's question.

## What is deliberately NOT renamed

Three categories, and an implementer who renames any of them has caused a defect:

1. **`tests/compat-matrix.tsv`.** 1,209 of its lines contain `macho9`. They are a *measurement* taken against the six historical C tools, which no longer exist. Renaming them falsifies a record rather than updating one. The spec's "Execution note" is the long version.
2. **Completed plans and specs** — `plans/2026-09-08-macho9-toolkit.md`, `plans/2026-09-09-finish-the-convergence.md`, `plans/2026-09-09-retire-the-compat-tools.md`, `plans/2026-09-10-report-what-macho9-did.md`. They describe work as it was actually done, under the name it was done with. Their filenames stay too.
3. **The six wrapper names**, per the Global Constraints.

*Pending* plans and specs — the edit-scripts pair, the relations pair, the release-conformance spec, and this plan's own spec — describe work not yet done and **are** updated, in Task 4.

---

### Task 1: Include guards

The largest mechanical change and the one with the least to go wrong. Landing it first shrinks every later diff.

**Files:**
- Modify: every `src/*.h` and `cli/*.h` carrying a `MACHO9_*_H` guard

**Interfaces:**
- Consumes: nothing. Produces: nothing. No symbol visible outside a translation unit changes.

- [ ] **Step 1: Enumerate the guards**

```bash
grep -rIo "MACHO9_[A-Z_]*" src/ cli/ | sed 's/.*://' | sort -u
```

Expect these ten or so: `MACHO9_ATOMIC_WRITE_H`, `MACHO9_DECLASSIFY_H`, `MACHO9_FAT_H`, `MACHO9_GROW_H`, `MACHO9_IMAGE_H`, `MACHO9_LC_KINDS_H`, `MACHO9_LINKEDIT_H`, `MACHO9_LIVE_H`, `MACHO9_MACH_COMPAT_H`, `MACHO9_ORDINALS_H`, plus whatever the edit-scripts work added (`MACHO9_SCRIPT_H`, `MACHO9_EDIT_H`). Work from the command's output, not from this list.

- [ ] **Step 2: Rename them**

`MACHO9_` → `MACHOTOOL_` on the guard identifiers only. **Bound the substitution to `src/` and `cli/`** — a tree-wide `sed` would hit `tests/compat-matrix.tsv` and the completed plans.

- [ ] **Step 3: Build and run everything**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native 2>&1 | grep -ci "warning:\|error:"
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
```

Expected: `0` warnings, every test passing. A guard rename cannot change behaviour; if anything moved, a substitution went wider than intended.

- [ ] **Step 4: Verify nothing outside src/ and cli/ moved**

```bash
git diff --stat
```

Expected: only `src/` and `cli/` files. If `tests/` or `docs/` appear, revert and redo with a bounded substitution.

- [ ] **Step 5: Commit**

```bash
git add src/ cli/
git commit -m "refactor: include guards become MACHOTOOL_*"
```

---

### Task 2: The binary, the CMake targets, and the release artifact list

**Files:**
- Rename: `cli/macho9.c` → `cli/machotool.c`
- Modify: `CMakeLists.txt` — `project()`, the `macho9` executable target, `macho9core`, the wrapper staging block, the test invocations
- Modify: `.github/workflows/release.yml` — the artifact list
- Modify: every test script that invokes the binary by name

**Interfaces:**
- Produces: a binary named `machotool`, a library target named `machotoolcore`. Every later task and every test harness consumes these.

- [ ] **Step 1: Find every reference to the target names**

```bash
grep -rn "macho9core\|macho9\b" CMakeLists.txt .github/workflows/release.yml | head -40
grep -rln "macho9" tests/ --exclude=compat-matrix.tsv
```

- [ ] **Step 2: Rename the source file and the targets**

```bash
git mv cli/macho9.c cli/machotool.c
```

Then in `CMakeLists.txt`: the `project()` name becomes `machotool`, `add_executable(macho9 …)` becomes `add_executable(machotool …)`, `macho9core` becomes `machotoolcore`, and every `target_link_libraries` referencing either follows.

- [ ] **Step 3: Update the test harnesses**

The shell suites take a bindir and build the binary path from it. Find where each names `macho9` and change it. `tests/cli_test.sh`'s `$MACHO9` variable is the main one; rename the variable too, so a reader is not chasing a name that no longer exists.

- [ ] **Step 4: Update the release workflow's artifact list**

`.github/workflows/release.yml` lists `build-cross/` paths by name. `macho9` becomes `machotool`; `macho9-compat.sh` and `macho9-translate.sh` are Task 3's and stay for now — leave them, and note in the report that Task 3 finishes this file.

- [ ] **Step 5: Build and run everything**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native 2>&1 | grep -ci "warning:\|error:"
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/characterize.sh /private/tmp/mm-build/schmonz/macho-tools/native check
```

The digest must be unmoved. A rename emits no different bytes.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt cli/ tests/ .github/workflows/release.yml
git commit -m "refactor: the binary is machotool, the library machotoolcore"
```

---

### Task 3: The two shared wrapper scripts

The riskiest of the four rename passes, because the six wrappers are a shipped interface and `compat/translate.sh` emits command lines naming the binary.

**Files:**
- Rename: `compat/macho9-compat.sh` → `compat/machotool-compat.sh`
- Modify: `compat/translate.sh` — the emitted grammar names the binary
- Modify: all six `compat/*.sh` wrappers — they source the shared script by name
- Modify: `CMakeLists.txt` — the staging block installs both under their new names
- Modify: `.github/workflows/release.yml` — the remaining two artifact names
- Modify: `tests/wrapper_test.sh`, `tests/translate_test.sh`, `tests/known-callers.sh`

**Interfaces:**
- Consumes: the `machotool` binary name from Task 2.
- Produces: staged scripts named `machotool-compat.sh` and `machotool-translate.sh`.

- [ ] **Step 1: Write the failing assertion first**

Before renaming anything, pin the property that must survive. In `tests/wrapper_test.sh`:

```sh
# The rename must not reach the wrappers' own names or their stdout. These
# six names are a shipped interface -- mavericksforever.com/claude/install.sh
# fetches three of them by name -- and the whole point of the compat layer is
# that a caller who learned it in 2024 still works.
for w in patch_macho change_dylib add_version_min fix_macho rename_segment retag_swift_classes; do
    [ -x "$BINDIR/$w" ] \
        && ok "wrapper $w still exists under its historical name" \
        || bad "wrapper names" "$w is missing from $BINDIR after the rename"
done
```

- [ ] **Step 2: Run it against the current build and watch it pass**

It should already pass — that is the point. It is a tripwire for Task 3, not a test of new behaviour, and its comment must say so rather than implying it tests something the rename adds.

- [ ] **Step 3: Rename the shared script and fix its callers**

```bash
git mv compat/macho9-compat.sh compat/machotool-compat.sh
```

Each of the six wrappers locates the shared script by name; update all six. `compat/translate.sh` emits command lines beginning with the binary's name — update the emitted grammar, and check whether the tool's name appears in any *diagnostic* it prints, since those reach stderr and some are asserted.

- [ ] **Step 4: Update the staging block and the workflow**

`CMakeLists.txt` stages both scripts under their historical installed names (`macho9-compat.sh`, `macho9-translate.sh`); both become `machotool-*`. `.github/workflows/release.yml`'s artifact list follows.

- [ ] **Step 5: Run the wrapper gates, which are the real test**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/known-callers.sh /private/tmp/mm-build/schmonz/macho-tools/native
tests/wrapper_test.sh /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/translate_test.sh /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/characterize.sh /private/tmp/mm-build/schmonz/macho-tools/native check
```

`known-callers` replays the shapes real callers use. If it moved, the rename reached the interface it was not supposed to.

- [ ] **Step 6: Commit**

```bash
git add compat/ CMakeLists.txt .github/workflows/release.yml tests/
git commit -m "refactor: the shared wrapper scripts become machotool-*"
```

---

### Task 4: Prose

**Files:**
- Modify: `README.md`, `compat/README.md`, `tests/README.md`, `docs/PROPOSAL.md`
- Modify: `docs/superpowers/QUEUE.md`
- Modify: the **pending** specs and plans only — `specs/2026-09-10-edit-scripts-design.md`, `plans/2026-09-10-edit-scripts.md`, `specs/2026-09-10-relations-and-verb-lowering-design.md`, `plans/2026-09-10-relations-and-verb-lowering.md`, `specs/2026-09-10-release-conformance-design.md`
- **Do not modify:** `tests/compat-matrix.tsv`, or any of the four completed plans named in "What is deliberately NOT renamed"

**Interfaces:** none.

- [ ] **Step 1: Add the two naming registers**

Per the family conventions, and per the spec's table: app identity is **"Mavericks Machotool"** (the `.app` bundle, `CFBundleName`, the Sparkle `PRODUCT_NAME`); prose is **"Machotool for Mavericks"** (the `.pkg` title, appcast channel title, README prose). The bundle id is `dev.modernmavericks.machotool`. Introduce these where the documents currently have no product name at all — they are new, not renamed.

- [ ] **Step 2: Rename in the pending documents**

Bound every substitution to the file list above. After the pass:

```bash
grep -rIl "macho9" . 2>/dev/null | grep -v "^./.git\|^./.superpowers"
```

Expected output: exactly `tests/compat-matrix.tsv` and the four completed plans. Anything else is either a miss or a file that should have been excluded — investigate rather than adding it to one list or the other.

- [ ] **Step 3: Explain the exclusions where a reader will hit them**

`tests/README.md` should say, in a sentence, why `compat-matrix.tsv` still names a binary that no longer exists. A reader who greps for `macho9`, finds 1,209 hits, and has to reconstruct the reason has been failed by the documentation.

- [ ] **Step 4: Run everything and commit**

```bash
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
git add README.md compat/README.md tests/README.md docs/
git commit -m "docs: machotool, and why two things keep the old name"
```

---

### Task 5: `target 10.9`

A third kind of line: not a directive, which describes the run, and not an operation, which edits the binary. It is the only line whose meaning depends on the binary.

**Files:**
- Modify: `src/script.h`, `src/script.c` — parse it
- Modify: `src/edit.c` — expand it
- Modify: `tests/script_test.c`, `tests/cli_test.sh`

**Interfaces:**
- Consumes: `ms_script`, `ms_stmt`, `ms_parse`, `me_run` from the edit-scripts work.
- Produces: `ms_stmt` entries with `kind == MS_TARGET`, and an expansion performed at the statement's position.

- [ ] **Step 1: Write the failing parser tests**

```c
static void test_target_parses_and_is_positional(void) {
    static const char src[] = "allow-grow\ntarget 10.9\nload-command delete uuid\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == 0, "parses (%s)", err);
    /* target IS a statement -- it occupies a position, because the expansion
     * lands where it is written and position changes what later statements
     * can do (fixups set classic rewrites __LINKEDIT, moving the header pad
     * available to every dylib replace after it). */
    CHECK(s.n == 2, "target occupies a statement slot (got n=%d)", s.n);
    CHECK(s.stmts[0].kind == MS_TARGET, "and it is the first of the two");
    ms_free(&s);
}

static void test_two_targets_is_a_parse_error(void) {
    static const char src[] = "target 10.9\ntarget 10.9\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1, "a second target is refused");
    CHECK(strstr(err, "line 2") != NULL, "and names the line (got: %s)", err);
}

static void test_unknown_target_is_refused_not_guessed(void) {
    static const char src[] = "target 10.10\n";
    ms_script s; char err[256] = {0};
    CHECK(ms_parse(src, sizeof src - 1, &s, err, sizeof err) == -1,
          "an unknown target errors rather than silently doing 10.9's work");
}
```

- [ ] **Step 2: Run and watch them fail**

Expected: `MS_TARGET` does not exist.

- [ ] **Step 3: Parse it**

Add `MS_TARGET` to the kind enum and a row to the table with one operand. Enforce the two rules at parse time: one `target` per script, and `10.9` is the only accepted operand.

Do **not** add a `disturbs` mask for it if the relations work has landed — `target` disturbs whatever its expansion disturbs, which is only known after expansion. If that column exists, give `MS_TARGET` its own case saying so, rather than a zero that would read as "disturbs nothing".

- [ ] **Step 4: Write the failing expansion tests**

In `tests/cli_test.sh`, one assertion per detection, each on a fixture exhibiting exactly that condition:

```sh
# target 10.9 expands, in place, into the statements the binary actually
# needs. Detection is EXACT in every case -- a load command is present or it
# is not, a tag bit is set or it is not -- so these assertions pin behaviour,
# not a heuristic's mood.
build_main "$T/tgt_plain"
printf 'target 10.9\n' >"$T/tgt.edits"
"$MACHOTOOL" edit --dry-run "$T/tgt_plain" "$T/tgt.edits" \
    >/dev/null 2>"$T/tgt.err" || bad "target" "$(cat "$T/tgt.err")"
# A fixture already built for 10.9 needs nothing, and saying so is a correct
# answer for a profile -- unlike for an explicit operation.
grep -q "target 10.9" "$T/tgt.err" \
    && ok "target: the report names the profile line" \
    || bad "target" "no target line in the report: $(cat "$T/tgt.err")"
```

Then one per row of the spec's two tables: `LC_DYLD_CHAINED_FIXUPS` present → `fixups set classic`; `LC_BUILD_VERSION` present → `load-command delete build-version`; no `LC_VERSION_MIN_MACOSX` → `version-min set 10.9`; `__DATA_CONST` carrying `__objc_*` → `segment rename __DATA_CONST __DATA`; the stable-ABI Swift tag → `swift-abi set legacy`.

**Build each fixture so the condition is true by construction**, not by hoping the host linker emits it. The cross runner's modern linker emits `LC_BUILD_VERSION` where 10.9's does not — that exact dependence made three `cli_test.sh` assertions pass here and fail in CI, and the fix was to make the premise true rather than assume it (`tests/cli_test.sh`'s `build_main_without_build_version`). Use the same discipline: strip or add the load command yourself, then assert it is in the state you need with `otool`, not with the tool under test.

- [ ] **Step 5: Expand it**

At the statement's position, `me_run` replaces one `MS_TARGET` with the statements its detections produced, then continues. The report lists the expansion line by line — that is the whole reason `target` is visible rather than hidden behaviour.

`target` never counts as unmatched under `fatal-warnings`: "this binary already targets 10.9 correctly" is a correct answer for a profile.

- [ ] **Step 6: Run, prove the tests can fail, commit**

Break one detection (make the chained-fixups check always report absent) and confirm exactly that assertion fails. Revert. Record it.

```bash
ctest --test-dir /private/tmp/mm-build/schmonz/macho-tools/native
git add src/script.h src/script.c src/edit.c tests/script_test.c tests/cli_test.sh
git commit -m "feat: target 10.9 expands where it is written"
```

---

### Task 6: There is no quiet mode

**Files:**
- Modify: `cli/machotool.c`, `src/edit.h`, `src/edit.c`, `tests/cli_test.sh`, `README.md`

**Interfaces:**
- Consumes: `me_opts` from the edit-scripts work.

- [ ] **Step 1: Establish which of the two situations you are in**

```bash
grep -n "verbose" src/edit.h src/edit.c cli/machotool.c
```

The spec records both paths. If `--verbose` exists, this task **deletes** it. If the edit-scripts plan was executed with the spec's cheaper option — never building it — this task has only the documentation half to do. Say which in the task report; do not assume.

- [ ] **Step 2: Write the failing test**

```sh
# There is no quiet mode, so there is no flag. A tool whose job is to make
# edits nobody can see afterwards should not have an option to say nothing
# about them. Anyone who wants silence has 2>/dev/null, which needs no flag
# of ours.
build_main "$T/noverb"
printf 'load-command delete uuid\n' >"$T/nv.edits"
"$MACHOTOOL" edit --verbose "$T/noverb" "$T/nv.edits" >/dev/null 2>"$T/nv.err" \
    && nv_rc=0 || nv_rc=$?
[ "$nv_rc" -ne 0 ] && ok "edit: --verbose is not a flag any more" \
    || bad "no quiet mode" "--verbose was accepted; the flag survives"

# And the report happens anyway, with no flag asked for.
build_main "$T/noverb2"
"$MACHOTOOL" edit "$T/noverb2" "$T/nv.edits" >"$T/nv2.out" 2>"$T/nv2.err" \
    || bad "no quiet mode" "$(cat "$T/nv2.err")"
grep -q "load-command delete" "$T/nv2.err" \
    && ok "edit: reports without being asked" \
    || bad "no quiet mode" "no report on stderr: $(cat "$T/nv2.err")"
[ ! -s "$T/nv2.out" ] && ok "edit: writes nothing on stdout, so it stays pipe-safe" \
    || bad "no quiet mode" "edit wrote to stdout: $(cat "$T/nv2.out")"
```

- [ ] **Step 3: Remove the flag and make the report unconditional**

Delete `me_opts.verbose` and every branch on it. `me_opts` keeps `log`. The report goes to **stderr** so the wrappers' stdout is unaffected, `edit` stays pipe-safe, and `info`/`verify` keep stdout for their data.

- [ ] **Step 4: Run the wrapper gates**

```bash
cmake --build /private/tmp/mm-build/schmonz/macho-tools/native
sh tests/known-callers.sh /private/tmp/mm-build/schmonz/macho-tools/native
tests/wrapper_test.sh /private/tmp/mm-build/schmonz/macho-tools/native
```

These are what prove the report went to stderr and not stdout.

- [ ] **Step 5: Document it and commit**

`README.md` states that there is no quiet mode and that `2>/dev/null` is the answer, with the reason — otherwise the absence reads as an oversight.

```bash
git add cli/machotool.c src/edit.h src/edit.c tests/cli_test.sh README.md
git commit -m "feat!: always verbose, on stderr; the --verbose flag is gone"
```

---

## The three steps the repo owner takes

Not tasks. The plan cannot do them, and they must happen **in this order**, before a new session starts in the renamed clone.

1. **Move the agent's project directory.** It is keyed to the working directory's path, so renaming the clone first orphans the memories and every transcript of this work:
   ```sh
   mv ~/.claude/projects/-Users-schmonz-Documents-code-trees-mavericks-macho-tools \
      ~/.claude/projects/-Users-schmonz-Documents-code-trees-mavericks-machotool
   ```
2. **Rename the local clone.** Both roots are the same filesystem object, so this moves both views at once:
   ```sh
   mv ~/Documents/code/trees/mavericks-macho-tools ~/Documents/code/trees/mavericks-machotool
   ```
3. **Rename the GitHub repo** — `ModernMavericks/macho-tools` → `ModernMavericks/machotool` — and update the local remote. GitHub redirects the old URL, so every task above lands without it.

None of this affects the adoption path: `install.sh` fetches `patch_macho`, `change_dylib` and `add_version_min` by name from Wowfunhappy's repo, and none of those names change.

## Self-review

**Spec coverage.** "The rename" table → Tasks 1–4. "`target 10.9`" including both detection tables and all four rules → Task 5. "Always verbose, on stderr" → Task 6. "Steps the repo owner takes" → the section above, unchanged from the spec. "Execution note: the behaviour matrix" → the "What is deliberately NOT renamed" section, and Task 4 Step 2 asserts the exclusion held. "Out of scope" → no tasks, correctly.

**Placeholder scan.** None. Every step carries its command or its code; Task 5 Step 6 names the mutation to make and what must fail.

**Type consistency.** `MS_TARGET`, `ms_parse`, `ms_script`, `ms_stmt`, `me_run`, `me_opts` match the edit-scripts plan's spellings. `$MACHOTOOL` replaces `$MACHO9` from Task 2 Step 3 onward, and Tasks 5 and 6 use the new name because they run after it.

**One judgement recorded rather than buried.** Task 4 excludes four completed plans from the rename on the same ground as the matrix: they describe work as it was done, under the name it was done with. That is a weaker argument than the matrix's — a plan is prose, not a measurement — so it is stated where a reviewer can disagree with it rather than discovering it as an inconsistency.
