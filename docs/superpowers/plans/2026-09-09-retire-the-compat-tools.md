# Retire the compat/ Tools Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the six historical C tools in `compat/` with shell wrappers
installed under the same names, so `macho9` becomes the only Mach-O rewriting
binary this repo ships — and prove every real caller still works end to end.

**Status of the idea:** requested by the repo owner. Two shapes were offered:
a wrapper that **invokes** `macho9`, or one that **prints the equivalent
`macho9` command line** so the caller adapts with little effort. This plan
recommends doing both, in that order, as a deprecation ladder.

**Spec:** `docs/PROPOSAL.md`. Its "Verbs" section settles the grammar, and its
"Migration" section is why `macho9 --capabilities` exists: so the tool and its
wrappers never have to move in lockstep.

## STOP — read this before Task 0. The dependency runs the WRONG WAY.

**`macho9` does not implement `dylib`, `rpath`, `lc` or `minos`. It `fork`s and
`exec`s the sibling `change_dylib` / `add_version_min` binaries**, and
`--capabilities` gates each of those verbs on `access(sibling, X_OK)`.

So replacing the compat tools with wrappers onto `macho9` is a **cycle**:

```
change_dylib (wrapper)  ->  macho9  ->  change_dylib (binary)   <-- does not exist any more
```

`macho9` must gain NATIVE implementations of those verbs before any wrapper can
exist. That is not a detail to discover during Task 1 — it is a prerequisite
task, and it is larger than everything else in this plan.

- [ ] **Task 0.5 (before the argument sweep): extract `change_dylib`'s
      operations into `src/`. This ONE extraction produces both the native
      verbs and the wrapper.**

      A whole-branch review described this as "wiring the CLI to the library".
      That was optimistic — **the op logic is not in the library yet.** It is
      in `compat/change_dylib.c` (1166 lines):

      | what | ~lines | destination |
      |---|---|---|
      | `emit_dylib_lc`, `build_lcs_lc`, `build_lcs` | 250 | `src/` — the LC-table builder, the core |
      | `cgb_lc`, `change_growth_bytes` | 110 | `src/` |
      | `process_one`, `process_fat`, `cd_swap32` | 430 | `src/` — the thin and fat drivers |
      | `main` — argument parsing | 210 | stays put; it is the old grammar |

      So this is one more extraction of the shape already done four times
      (`uleb`, `image`/`ordinals`, `linkedit`, `grow` at 1283 lines). What
      makes it worth doing FIRST is that it delivers three things at once:

      1. `macho9` implements `dylib`/`rpath`/`lc` natively — the cycle dissolves.
      2. `compat/change_dylib.c` collapses to ~210 lines of argument
         translation — **which is exactly the wrapper Task 2 was going to
         write.** The wrapper is a by-product of the extraction, not extra work.
      3. `fix_macho` can consume the same drivers, closing `docs/PROPOSAL.md`'s
         sequencing step 2 ("`fix_macho` and `change_dylib` converge"), which
         both prior plans skipped and which is the residue behind the
         `LC_LOAD_UPWARD_DYLIB` divergence.

      `characterize` is the gate: native output must be byte-identical to the
      delegated output, on every path.

- [ ] **Task 0.6: the three missing verbs.** Smaller than they sound.
      `segment` is a thin verb over `compat/rename_segment.c` (147 lines);
      `retag-swift` over `compat/retag_swift_classes.c`; `declassify` is
      `patch_macho`'s conversion, which already exists and needs only a verb.
      `fix_macho`'s `-rename_seg` folds into `segment` once Task 0.5's drivers
      give `macho9` fat support.

**Three of the six tools have NO `macho9` verb at all.** Not edge flags —
whole tools:

| tool | `macho9` verb | state |
|---|---|---|
| `change_dylib` | `dylib` / `rpath` / `lc` | delegates (Task 0.5) |
| `add_version_min` | `minos` | delegates (Task 0.5) |
| `patch_macho` | `declassify` | **stub — errors out** |
| `rename_segment` | `segment` | **does not exist** |
| `retag_swift_classes` | `retag-swift` | **does not exist** |
| `fix_macho` | — | **no verb, and see below** |

`docs/PROPOSAL.md` names `segment` and `retag-swift`; neither was built.

**`fix_macho` is the hardest of the six.** Its `-rename_seg` has no verb, and —
more fundamentally — **every `macho9` verb is thin-only**: `verify`, `info` and
`grow` on a fat file return `EX_REFUSED` with "not a readable 64-bit Mach-O".
`fix_macho` is the only tool that iterates fat slices. Wrapping it requires fat
support in `macho9` first.

**Two behaviour deltas any wrapper must consciously preserve or break:**
- `compat/rename_segment.c:125` exits **2** when nothing matched. A wrapper onto
  a `macho9` verb must reproduce that or deliberately change it.
- `compat/retag_swift_classes.c:227` **always exits 0**, even on write failure.
  A wrapper will either faithfully reproduce a silent success or quietly fix it.
  Decide which, and say so.

**Also blocking, separately:** `src/live.h` has no install or export rule.
`CMakeLists.txt` installs `RUNTIME DESTINATION bin` only — no headers, no
INTERFACE target, no `find_package` package — so avxemu cannot consume it today
except by copying it. `docs/PROPOSAL.md` requires one. Packaging was explicitly
out of scope for the convergence plan, so this is a gap to schedule, not a
defect, but it belongs on someone's list.

## The thing that makes this harder than it looks

**The old grammar was deliberately not adopted.** `docs/PROPOSAL.md` rejected
`-add_rpath`, bare `-rpath`, `-change` and `-add` as synonyms on purpose,
because they "would advertise an interchangeability that does not exist, on
exactly the binaries where it does not hold." So a wrapper is not a rename —
it is a translation between two grammars that were intentionally made
different, and every translation is a claim that two things are equivalent.
Each such claim needs a test.

**The wrappers map their ENTIRE argument surface onto `macho9` — not a
convenient subset.** Every flag, every combination each old tool accepts gets a
`macho9` translation. The working assumption is that `macho9` can already
express all of it; the exhaustive test in Task 1 is how we find out whether
that assumption holds.

Two places it is already known not to: `macho9 --capabilities` omits
`declassify` (patch_macho's chained-fixups conversion) and refuses
`rpath -insert`. Those are not accommodations to design around — they are the
first two entries on the list of gaps this work exists to surface. When the
exhaustive test finds more, **stop and decide**: extend `macho9`, or record
that the old spelling has no equivalent and why. Never let a wrapper silently
do something adjacent to what was asked.

## Known callers — the acceptance criterion

**A wrapper that passes the test suite but breaks a real caller is a failure.**

These two bodies of evidence are not weighted equally, and the difference is
deliberate:

| evidence | purpose | weight |
|---|---|---|
| **Known-caller end-to-end tests** | the actual gate — these are the people we are migrating | **decisive**; a failure here blocks |
| Exhaustive argument-combination sweep (Task 1) | discovery — find where `macho9` cannot express an old spelling | informative; a gap here is a finding to decide on, not automatically a blocker |

A surprising result in the sweep starts a conversation. A failure against a
known caller stops the work. Enumerate the callers before writing a line of
wrapper, and make each one an end-to-end test:

**Task 0 evidence (2026-09-09).** `install.sh` was fetched directly
(`curl -fsSL https://mavericksforever.com/claude/install.sh`, 336 lines, read in
full — no WebFetch summarization). `~/Documents/code/trees/mavericks-*` and
`~/Documents/trees/mavericks-*` were both grepped for the six names (every
checkout listed by name below, including zero-hit ones); the two roots turned
out to be the same filesystem object (identical inode/device numbers), so this
was one search surfaced twice, not two independent ones. Full method, raw
grep output, and self-review are in
`.superpowers/sdd/2026-09-09-retire-the-compat-tools/task-0-report.md`.

| caller | what was actually found | evidence |
|---|---|---|
| `mavericksforever.com/claude/install.sh` | Confirmed, real, production. Its top level downloads pre-built `patch_macho`, `change_dylib`, `add_version_min` **by name** from `$BASE_URL` — it does **not** compile them (no `fix_macho`/`rename_segment`/`retag_swift_classes` anywhere in it). The `/usr/local/bin/claude` wrapper it generates invokes all three, in order, against the user's real Claude Code binary, only when a byte-pattern check on the binary shows it isn't already patched. Exact invocations and stdout/exit-code handling are in the subsection below. | fetched and read the full script directly, 2026-09-09 |
| `Wowfunhappy/Mavericks-Porting-Resources` | **Refined, not confirmed as written.** A local checkout exists (`~/Documents/code/trees/Mavericks-Porting-Resources`, remotes `origin=schmonz/…` `upstream=Wowfunhappy/…`). It ships its own copies of all six `.c` files and its own hermetic `change_dylib_test.sh`, but that test compiles *its own* `change_dylib.c` from scratch — it is not a caller of this repo's binaries, it's the source lineage PROVENANCE.md already documents. Separately, `gh pr list --repo Wowfunhappy/Mavericks-Porting-Resources` shows PRs #11 and #12 (the fixes this repo already carries) still **OPEN**, unmerged — confirming PROVENANCE.md's claim live rather than just repeating it. | read the checkout directly; `git remote -v`, `git log`; `gh pr list` (network, live) |
| this repo's own suites | The plan named four; **there are five**. `tests/change_dylib_test.sh`, `tests/cli_test.sh`, `tests/characterize.sh`, `tests/chained-fixups.sh`, and `tests/leaf-tool-crashes.sh` (missed by the original table) are all registered via `add_test(...)` in `CMakeLists.txt` (lines 177, 190, 194, 202, 211) and run under `ctest`, which `.github/workflows/release.yml` runs in CI, with `chained-fixups` and `characterize` additionally called out as their own explicit steps. | read `CMakeLists.txt` and `.github/workflows/release.yml` directly |
| avxemu | **Confirmed unaffected, and the table's own premise was more optimistic than reality.** `grep -rn` over the complete `~/Documents/code/trees/mavericks-avxemu` checkout is zero hits for all six tool names **and** for `src/live.h`/`macho9` — avxemu does not consume `live.h` today; that link is still aspirational (`docs/PROPOSAL.md`, and this plan's own Task 4-adjacent note that `live.h` has no install/export rule yet). | `grep -rn` over the full checkout; zero hits confirmed two ways |
| **added — not in the original table:** a personal, repo-owner-controlled script pair in `~/Documents/code/trees/mavericks-claude-ongoing` (`scripts/mf-build-local.sh`, `scripts/mf-wrapper-rebase.sh`) | Builds `change_dylib` **from `Mavericks-Porting-Resources`, not from this repo**, with plain `clang -O2 -Wall -std=c11` (no `macho9core`), specifically because it needs `-insert` (merged upstream as PR #6, but not yet rebuilt onto the CDN `install.sh` fetches from). Invokes it to link `libavxemu.dylib` as an ordinary `LC_LOAD_DYLIB` dependency instead of `DYLD_INSERT_LIBRARIES`. `docs/linking-avxemu.md` **documents this as** "done and running here since 2026-09-08" — I confirmed the mechanism works as described (the scripts exist and do what the doc says), but did not independently confirm it is presently the active path on that machine; it is, either way, a personal dev tool on the repo owner's own machine, not something distributed to other users. | read `docs/linking-avxemu.md`, `scripts/mf-build-local.sh`, `scripts/mf-wrapper-rebase.sh` directly |
| **checked and struck — not a real caller:** `mavericks-macdown3000`'s porting plan | Its plan document names exact `fix_macho`/`add_version_min`/`change_dylib` invocations it *intends* to run, but its own `HANDOFF.md` says outright: "**Status:** Planned, not started. No binary has been patched." No `PORTING-LOG.md` exists, no `build/` directory exists, and every task checkbox in the plan is unchecked. Never named by the original table; investigated because the family-wide grep surfaced it. | read `HANDOFF.md`; checked for `PORTING-LOG.md` and `build/` (absent); read the plan's checkbox state |
| **checked and struck — an approval record, not a confirmed execution:** `mavericks-magic-trackpad2/.claude/settings.local.json` | Records four approved Bash permission entries naming the already-*installed* `~/.local/share/claude-mavericks/{patch_macho,add_version_min,change_dylib}` against a real Claude binary (`2.1.181`). A pre-approved permission entry is evidence the command was **approved**, not that it **ran** — I did not do a full audit of that unrelated (trackpad-driver) project to confirm execution, and found no wrapping script that would run these repeatedly. **Unverified: whether these four commands were ever actually executed**, versus pre-approved and never run. Identical entries also appear in its `voodooinput-fork-history` worktree copy (same session history, not a second instance). | read `.claude/settings.local.json` directly in both locations; did not audit the rest of that project |
| **filed here for accuracy, not a caller:** `mavericks-machotools.orig` | **Non-zero hits — this is source lineage, not a caller, and does not belong in a "zero hits" list.** It's the earlier, superseded extraction PROVENANCE.md's "Earlier extraction" section already documents (2026-08-14, local-only, never pushed): `grep` returns hits in `macho_grow.h`, `macho_grow_test.c`, `fix_macho.c`, `change_dylib.c`, `PROVENANCE.md`, and `add_version_min.c` because it literally *is* an earlier copy of these tools' own source, exactly like `Mavericks-Porting-Resources` above — it does not invoke any binary built from this repo, or from anywhere else. | `grep -rln` over the checkout: six files hit, listed above |

Every other `mavericks-*` checkout under both roots (`1password`, `apfs`,
`ca-certs`, `cctools`, `clang`, `compat`, `consomme`, `container-tools`,
`ed25519`, `flavours-darkmode`, `golang`, `hypervisor`, `legacysupport`,
`mosh`, `ninja`, `nodejs`, `openssh`, `openssl`, `orion`, `porthole`, `remote`,
`reverse-engineering`, `rust`, `sdl`, `signal-desktop`, `swift-runtime`,
`swift-toolchain`, `tailscale`, `zfs`) had **zero** hits for the six names.
(`mavericks-machotools.orig` is NOT in this list — see its own row above.)
`mavericks-shipyard` had one hit, in `SKILL.md`, describing `macho9`'s own
fork/exec design — not a caller.

### Exact invocation lines (quoted verbatim from source)

`install.sh`'s embedded `/usr/local/bin/claude` wrapper, the only production
invocation of these tools by name:

```sh
"$MF/patch_macho"     "$REAL" "$T" >/dev/null || { echo "claude: patch_macho failed"     >&2; exit 1; }
"$MF/add_version_min" "$T"         >/dev/null || { echo "claude: add_version_min failed" >&2; exit 1; }
"$MF/change_dylib"    "$T" -strip-lc uuid -strip-lc codesig \
    -change "/usr/lib/libSystem.B.dylib"  "@loader_path/../S.dylib" \
    -change "/usr/lib/libicucore.A.dylib" "@loader_path/../I.dylib" \
    -change "/usr/lib/libc++.1.dylib"     "@loader_path/../c++.1.dylib" \
    >/dev/null || { echo "claude: change_dylib failed" >&2; exit 1; }
```

This repo's own CI equivalence gate, `tests/characterize.sh` (lines 22-26,
quoted exactly):

```sh
"$BIN/patch_macho"     "$T/in" "$T/out" >/dev/null
"$BIN/add_version_min" "$T/out"         >/dev/null
"$BIN/change_dylib"    "$T/out" -strip-lc uuid -strip-lc codesig \
    -change "/usr/lib/libSystem.B.dylib" "@loader_path/../S.dylib" >/dev/null
"$BIN/rename_segment"  "$T/out" >/dev/null 2>&1 || true
```

`tests/chained-fixups.sh` (lines 133-160), which covers `patch_macho`'s
chained-fixups conversion the `characterize` fixture can't reach, runs the
same `patch_macho` -> `add_version_min` -> `change_dylib` pipeline (same
`-strip-lc`/`-change` flags as above) against a dynamically-built fixture,
twice, to also prove determinism — it does not call `rename_segment`.

`mavericks-claude-ongoing/scripts/mf-wrapper-rebase.sh`'s local, `-insert`-using
call (built from `Mavericks-Porting-Resources`, per the row above):

```sh
mf_change_dylib "$MFL/change_dylib" "$AVXOPS"
# where AVXOPS is "-strip-lc uuid -strip-lc codesig -change ... -insert <path>"
```

`mavericks-magic-trackpad2`'s recorded, exact permission entries (struck above
as an approval record, not a confirmed execution):

```sh
/Users/schmonz/.local/share/claude-mavericks/patch_macho /tmp/claude-2.1.181 /tmp/c181.patched
/Users/schmonz/.local/share/claude-mavericks/patch_macho /tmp/claude-2.1.181 /tmp/c181.b
/Users/schmonz/.local/share/claude-mavericks/add_version_min /tmp/c181.b
/Users/schmonz/.local/share/claude-mavericks/change_dylib /tmp/c181.b -change /usr/lib/libSystem.B.dylib /usr/lib/sysW.dylib -change /usr/lib/libicucore.A.dylib /usr/lib/icuW.dylib -change /usr/lib/libc++.1.dylib /usr/lib/cxx.dylib
```

Full invocation lines for every other test case in `change_dylib_test.sh`,
`cli_test.sh`, and `leaf-tool-crashes.sh` are in the task report
(`.superpowers/sdd/2026-09-09-retire-the-compat-tools/task-0-report.md`) —
they're plentiful (dozens of combinations) and already live, verbatim, in
those test files themselves.

### How the callers use stdout, stderr, and exit codes

- **`install.sh`'s embedded wrapper** redirects all three tools' stdout to
  `/dev/null` — nothing is parsed as data. The **exit code is checked**: every
  call is `"$MF/$tool" ... >/dev/null || { echo "claude: $tool failed" >&2; exit 1; }`,
  so a nonzero exit aborts the wrapper outright. **Stderr is not captured or
  parsed** — it passes straight through to whatever launched `claude` (a
  terminal), for a human to read. Whether to run the three tools at all is
  decided separately, by `grep -qE` over the raw bytes of the *target binary*
  itself, not over any tool's output.
- **This repo's own suites** (`change_dylib_test.sh`, `cli_test.sh`,
  `chained-fixups.sh`, `leaf-tool-crashes.sh`) follow the same shape throughout:
  `>/dev/null` on every invocation whose output isn't the thing under test,
  `||`/`$?` exit-code checks everywhere, and stdout/stderr captured into a
  variable only on the specific cases that assert a particular message (e.g.
  `change_dylib_test.sh`'s case 8b, `fix_macho -change` on an
  `LC_LOAD_UPWARD_DYLIB` fixture, captures with `out=$(... 2>&1)` to check for
  "no changes needed"; `leaf-tool-crashes.sh` checks exact exit codes and
  greps captured stderr for specific diagnostic wording). `characterize.sh`
  never reads any tool's stdout at all — it `shasum`s the **output file's
  bytes**, which is the whole point of characterization.
- **`Wowfunhappy/Mavericks-Porting-Resources`'s own `change_dylib_test.sh`**
  (a self-test of its own binary, not a caller of this repo's) is the same
  shape again: `>/dev/null` on every tool run, `||`/exit-code checks, and
  stderr captured to a file only in the six capacity-refusal cases, where it's
  grepped for the substring `"too many"` — a diagnostic-message check, not a
  byte-exact one.
- **`mavericks-claude-ongoing`'s `mf-wrapper-rebase.sh`** is `>/dev/null` on
  the `change_dylib` call with a hard `|| exit 1` on failure — unsurprising,
  since it's a rebase of `install.sh`'s own wrapper.
- **`mavericks-macdown3000`'s plan** (struck above, never executed) specifies
  a human reading `otool -L` output afterward, not a script parsing the
  tool's own stdout — moot, since it never ran.

**What this means for Task 2's byte-identity requirement:** no real caller
found parses these six tools' stdout as machine-readable data. Every one
either discards it (`>/dev/null`) or — only inside this repo's own tests and
upstream's own test — captures it deliberately to assert on one specific,
named message, not a byte-exact transcript. Exit codes are checked by every
caller that checks anything at all, and are the thing that actually gates
control flow (`install.sh`'s wrapper aborts on nonzero). Stderr is read by a
human in the one production caller, and grepped for a diagnostic substring
(never byte-compared) in the test suites. Conclusion: `characterize`'s
byte-identical-stdout requirement is stricter than any real caller needs
today — but it should stay the internal gate anyway, because this repo's own
tests are real callers too, and at least one of them (case 8b) does assert
specific stdout wording that a wrapper must still produce.

### Evidence for the open question — is there only one caller, and does the repo owner control it?

**No, on both counts.**

- **Not only one.** Found: the production `install.sh`/wrapper; this repo's
  own five-suite CI gate; and a second, repo-owner-controlled tool
  (`mavericks-claude-ongoing`'s `mf-build-local.sh`/`mf-wrapper-rebase.sh`)
  that also invokes `change_dylib` — built from Wowfunhappy's tree, with
  `-insert` — for an ongoing personal avxemu-linkage experiment.
- **The one production caller is not repo-owner-controlled.**
  `mavericks-claude-ongoing/docs/upstream/mf-installer-link-avxemu/REPORT.md`
  is addressed, in its own words, "For: mavericksforever.com / Wowfunhappy —
  the `claude` wrapper `install.sh` emits." `gh pr list --repo
  Wowfunhappy/Mavericks-Porting-Resources` (checked live, 2026-09-09) shows
  PRs #11 and #12 — the two fixes this repo already has — still **OPEN**
  against Wowfunhappy's repo, unmerged. Wowfunhappy, not this repo's owner,
  controls when (or whether) `install.sh` and its CDN artifacts move.
- **Implication:** going straight to phase two (Task 3, "translate and
  refuse") would force a migration on a third party who does not control this
  repo's release cadence and has two of this repo's own fixes still
  unreviewed. The plan's phase-one-then-phase-two sequencing is the safer
  call, not an unnecessary precaution to skip.

- [x] **Task 0 (do this first): enumerate the real callers and their exact
      invocations.** Do not guess. Fetch `install.sh` and read it. Grep the
      family checkouts under `~/Documents/code/trees/mavericks-*` for calls to
      these six names. Write the list into this plan before proceeding — if the
      list is wrong, everything after it is built on sand.
      **Done 2026-09-09** — see the evidence and tables above, and the full
      method/self-review in
      `.superpowers/sdd/2026-09-09-retire-the-compat-tools/task-0-report.md`.

## Global Constraints

- **Do not break installs.** The six binaries must keep their names and keep
  being buildable from this repo throughout. `install.sh` moving is a separate
  change in a separate repo, and until it happens the wrappers must be
  drop-in.
- **`tests/EXPECTED` is a characterization reference**, never edited;
  `characterize` passing is the proof a wrapper produced identical bytes.
- The tools must never move a byte of file data (two reviewed exceptions
  stay: `-grow`'s memmove after the load commands, and the export-trie append
  past `__LINKEDIT`'s end).
- POSIX `/bin/sh` only in the wrappers — 10.9's shell is old, and these run on
  the target. No bashisms.
- Stock 10.9 AppleClang 6.0 for anything still compiled; no post-10.9 APIs.

## Host portability

`tests/README.md` carries ten lessons, every one earned from a real failure
here. All apply. The two most likely to bite a wrapper task:
- Never parse `nm`/`otool` human-readable output as an oracle.
- Assert the behaviour, not which guard fired.

---

### Task 1: Translate, don't run — and map the WHOLE argument surface

Before any wrapper exists, the translation must be expressible and testable on
its own, independently of whether a wrapper then executes it.

- [ ] Add a way to take an old-style invocation and PRINT the `macho9`
      equivalent without doing anything. Decide deliberately whether it lives
      in `macho9` or in the wrappers; put it wherever it can be tested directly.
- [ ] **Enumerate every flag and flag combination the six tools accept** — from
      their argument parsers, not from their `--help` text, which may lag. This
      is the input to everything below.
- [ ] One test per translation, asserting the exact emitted command line.
- [ ] **Exhaustive combination sweep — and it must be exhaustive.** Anything
      less cannot establish that everything previously handled is still handled.
      Single flags will mostly look clean; the surprises live in PAIRS and
      TRIPLES, because that is where the tools interact. Precedent from this
      repo: `-change X` combined with `-delete X` produced a binary dyld
      refused, exited 0, and shipped that way for months — each flag alone was
      fine.

- [ ] **Record BOTH behaviours per combination, then classify.** For every
      combination, capture what the OLD tool did (accepted / refused / crashed,
      and the output bytes if it acted) and what `macho9` does. The matrix, not
      a pass/fail list, is the deliverable:

      | old tool | macho9 | meaning | action |
      |---|---|---|---|
      | accepted | accepts, same bytes | preserved | none |
      | accepted | accepts, DIFFERENT bytes | **regression or deliberate fix** | investigate; `characterize` decides |
      | accepted | refuses | **REGRESSION — blocks** | fix `macho9` or the translation |
      | refused for a real reason | refuses | preserved | none |
      | refused ARTIFICIALLY | accepts | **improvement — keep it** | verify it is safe, then document |
      | crashed | refuses | improvement | document |

- [ ] **`macho9` does NOT have to inherit the old tools' artificial limits.**
      Where an old tool refused a combination only because its parser never
      grew the case — not because the combination is unsafe — `macho9` handling
      it is a feature, not a compatibility break. Say which of the two each
      refusal was; "the old one didn't do this either" is a reason to look, not
      a reason to stop. This repo has already lifted three such limits
      deliberately: fat binaries in the rewrite path, the export-trie rebuild,
      and the `-change`/`-delete` conflict.
      The limits that must NOT be lifted are the ones with a stated safety
      reason — 32-bit Mach-O, `LC_NOTE`/`LC_ATOM_INFO` layout, and every
      `-grow` refusal. Those are refuse-rather-than-guess decisions with
      recorded reasoning, not gaps.
- [ ] Where no translation exists, emit a clear "no equivalent" and a non-zero
      exit — never a plausible-looking command that would do something else.

### Task 2: Phase one wrappers — warn, translate, and still do the work

The kind form of deprecation: the caller's script keeps working, and the
message teaches the new grammar.

- [ ] Replace each `compat/*.c` with a `/bin/sh` wrapper of the same installed
      name that prints the `macho9` equivalent to **stderr** (so stdout stays
      byte-identical for anything parsing it), then execs `macho9` to do the work.
- [ ] stdout, exit codes and the rewritten file must be byte-identical to the
      C tool's. `characterize` is the gate.
- [ ] Every known caller from Task 0 replayed end to end, output compared.

### Task 3: Phase two — translate and refuse

Only after Task 2 has shipped and the known callers have been updated.

- [ ] Wrappers print the equivalent and exit non-zero without acting.
- [ ] Requires `install.sh` (and any other Task 0 caller) to have moved first.
      **This task is blocked on a change in another repo — do not start it
      until that has landed.**

### Task 4: Remove

- [ ] Delete the wrappers and the `compat/` directory. Update
      `PROVENANCE.md`, `README.md`, `compat/README.md`'s successor, and the
      install docs.

---

## Open question for the repo owner

Task 2 and Task 3 are two different products. **Phase one** (warn + still work)
is safe and slow; **phase two** (translate + refuse) forces migration but breaks
anyone who has not moved. The plan sequences them, but if the only real caller
is an `install.sh` you control, going straight to phase two may be simpler and
honest — one repo to update, no long deprecation tail. Decide after Task 0 tells
us who the callers actually are.

## Self-Review

**Spec coverage.** The proposal's "Migration" section says `--capabilities`
exists so tool and wrapper need not move in lockstep; Task 1 makes the
translation itself testable, which the proposal did not specify but the
grammar's deliberate incompatibility demands.

**Placeholders.** Task 0's caller table is deliberately incomplete — filling it
in from evidence is the task.

**Measurable done.** `compat/` is gone, `macho9` is the only shipped rewriting
binary, and every caller enumerated in Task 0 has an end-to-end test that
passes.
