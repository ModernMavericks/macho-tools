# Release Conformance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make this repo able to cut a release, versioned the way the family's conventions say a self-upstream repo versions itself.

**Architecture:** Four tasks. The first is the versioning decision made real — `X.Y.Z`, no `-mavericks` suffix, the tag trigger to match, `local_release` gone, and the deviation declared where the family's gate can see it. The second adds the `build/` script wrappers and `release-notes/`, copied in shape from `mavericks-golang`, which is the family's most complete reference. The third adds `renovate.json`. The fourth wires the `ver` step and the publish job.

**Tech Stack:** POSIX `/bin/sh` (these run natively on 10.9), GitHub Actions, `ModernMavericks/shipyard` via its install action and reusable workflows.

**Spec:** `docs/superpowers/specs/2026-09-10-release-conformance-design.md`

**Depends on:** `docs/superpowers/plans/2026-09-10-machotool-rename-and-target.md` having landed. `release.yml`'s artifact list names `macho9`, `macho9-compat.sh` and `macho9-translate.sh`; the rename changes all three, and this plan edits the same lines. Landing this first means editing them twice.

## Global Constraints

- **POSIX `/bin/sh` only** in anything a native 10.9 build executes — `build/*.sh`, packaging. 10.9's shell is old. No bashisms, no `python3` (10.9 ships Python 2 only), no `sort -V`. Scripts that only ever run in CI may use modern tools, but must say so.
- **Consume shipyard's facilities; never hand-roll them.** Install via `ModernMavericks/shipyard/.github/actions/install@v1`, then use `$SHIPYARD_SCRIPTS`. Do not re-derive it from the CMake user package registry — the action exports it, and that incantation appeared eleven times across the family before it was exported once.
- **Do not restate `ignoreTests: false` locally.** The shipyard preset sets it; a local copy silently stops tracking the preset the day the preset changes, and `check-family-conventions.sh` fails on it.
- **`tests/EXPECTED` is never edited.** `tests/characterize.sh` must keep reproducing `ad12bdd780da4131f81a808e6d08b688e2034f37e434772f81df023332b39792`. Nothing in this plan touches a byte any tool emits, so a moved digest is a real defect.
- **Do not move `@v1` by hand**, and do not push `../mavericks-shipyard` as part of this work. Pushing its `main` moves `@v1` family-wide.
- **A comment or doc that claims more than the code does is a defect.**

## What this plan does not do

**The `.pkg` and the Sparkle updater.** The spec names them as "the largest single piece and the one most reasonably split into its own increment", and says everything else "can land first and produce a GitHub Release of the binaries". That is the split this plan takes. The updater must not link the product it updates, and `mavericks_add_updater_app` is the facility for it — both facts belong in that plan, not this one.

**Actually cutting a release.** This makes one possible; it does not make one.

---

### Task 1: The versioning decision, made real

**Files:**
- Modify: `CMakeLists.txt:9-14` — the comment justifying the scheme
- Modify: `.github/workflows/release.yml` — the tag trigger; delete the `local_release` dispatch input and everything keyed on it
- Modify: `INGREDIENTS.md` — add the conformance deviation
- Modify: `UPSTREAM_VERSION` — unchanged in content; its meaning is what changes

**Interfaces:**
- Produces: a repo that versions itself `X.Y.Z` and publishes from a tag matching `[0-9]*.[0-9]*.[0-9]*`.

- [ ] **Step 1: Correct the comment that states the wrong premise**

`CMakeLists.txt:9-14` currently says:

> *"this repo is its OWN upstream, in ed25519's sense of the phrase — the thing it exists to port is itself. So `UPSTREAM_VERSION` is the family's usual file and the family's usual `<version>-mavericks.N` shape still applies."*

**The premise is right and the analogy is wrong.** `mavericks-ed25519` is a *port* — date-versioned, `20221003-mavericks.N`, where `-mavericks.N` means "our Nth repackage of *someone else's* ed25519". This repo has no someone-else. The right analogue is `mavericks-porthole`, and the conventions are explicit: a repo that is its own upstream "drops the `-mavericks` suffix and versions itself directly."

Rewrite the comment to say that, and to say why semver rather than a date: this product's user-visible surface is a CLI grammar that consumers probe — `--capabilities` exists precisely so a wrapper and the tool need not move in lockstep — and that grammar is about to change materially. A date says when; semver says whether your script still runs.

`UPSTREAM_VERSION` **stays**, holding this repo's own version, hand-bumped. The conventions permit exactly that for a self-upstream repo. Only the comment changes.

- [ ] **Step 2: Change the tag trigger**

In `.github/workflows/release.yml`, `tags: ['*-mavericks.*']` becomes `tags: ['[0-9]*.[0-9]*.[0-9]*']`. Update the header comment on line 7, which currently explains the `*-mavericks.*` shape.

- [ ] **Step 3: Delete `local_release`**

Remove the `workflow_dispatch` input and every conditional keyed on it. **Write the reason in the commit message, because this removes a capability:** the `-mavericks.N` axis exists to re-release *the same upstream* with different packaging. With no external upstream there is nothing to re-release against — a packaging change here is a change to the product, so it earns a version bump like any other. A rebuild-without-source-change, if ever genuinely needed, is a patch bump.

- [ ] **Step 4: Declare the conformance deviation**

Shipyard's `check-artifact-conformance.sh:70` hard-requires `*-mavericks.[0-9]*` and has no self-upstream exemption, so choosing semver — which SKILL.md explicitly sanctions for a self-upstream repo — must be declared as a scoped deviation. That is the mechanism the conventions provide. In `INGREDIENTS.md`:

```markdown
## Conformance deviations

- scheme:*: this repo is its own upstream (no external thing to repackage), so it
  versions itself directly as X.Y.Z per the self-upstream rule, and there is no
  -mavericks.N axis to carry
```

- [ ] **Step 5: Verify the conventions gate still passes**

```bash
sh ../mavericks-shipyard/scripts/check-family-conventions.sh .
```

If it fails, read what it says before changing anything: the gate encodes the family's rules, and a failure here is usually the plan being wrong rather than the gate.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt .github/workflows/release.yml INGREDIENTS.md
git commit -m "feat!: version as X.Y.Z; this repo is its own upstream, not a port"
```

---

### Task 2: `build/` wrappers and `release-notes/`

Thin wrappers around shipyard's implementations, so the logic cannot drift between repos. Shapes copied from `mavericks-golang`, the family's most complete reference.

**Files:**
- Create: `build/msc.sh`, `build/version.sh`, `build/release-notes-file.sh`
- Create: `release-notes/README.md`

**Interfaces:**
- Produces: `sh build/version.sh auto` printing this repo's version; `sh build/release-notes-file.sh TAG FULL` producing a non-empty notes file.

- [ ] **Step 1: Write `build/msc.sh`**

Sourced, not executed. It is the only per-repo part of the version scaffolding:

```sh
# build/msc.sh -- sourced: locate the installed mavericks-shipyard scripts dir as $SHIPYARD.
# Resolution: $SHIPYARD_SCRIPTS (exported by install@v1 in CI) -> the CMake user package registry (a local
# `cmake --install`) -> a sibling checkout (a dev box that has never installed it).
# This is the only per-repo part of the version scaffolding; the logic itself lives in shipyard.
SHIPYARD="${SHIPYARD_SCRIPTS:-}"
[ -d "$SHIPYARD" ] || SHIPYARD="$(cat "$HOME/.cmake/packages/MavericksShipyard/"* 2>/dev/null | head -1)/scripts"
[ -d "$SHIPYARD" ] || SHIPYARD="$(cd "$(dirname "$0")/.." && pwd)/../mavericks-shipyard/scripts"
[ -d "$SHIPYARD" ] || { echo "cannot locate mavericks-shipyard scripts (install it, or set SHIPYARD_SCRIPTS)" >&2; return 1 2>/dev/null || exit 1; }
export SHIPYARD
```

- [ ] **Step 2: Write `build/version.sh`**

Simpler than golang's, which carries a per-line `GO_LINE` this repo has no equivalent of:

```sh
#!/bin/sh
# Thin wrapper: the logic lives in shipyard (scripts/version.sh) so it cannot drift between repos.
# UPSTREAM_VERSION here holds THIS repo's own version -- it is its own upstream -- hand-bumped,
# with no Renovate customManager watching it because nothing external releases it.
set -eu
SELF="$(cd "$(dirname "$0")" && pwd)"
MAVERICKS_ROOT="$(cd "$SELF/.." && pwd)"; export MAVERICKS_ROOT
. "$SELF/msc.sh"
exec sh "$SHIPYARD/version.sh" "$@"
```

- [ ] **Step 3: Write `build/release-notes-file.sh`**

```sh
#!/bin/sh
# Thin wrapper: the logic lives in shipyard (scripts/release-notes-file.sh). Only the product
# name is ours.
#   usage: release-notes-file.sh <TAG> <FULL_VERSION>
set -eu
SELF="$(cd "$(dirname "$0")" && pwd)"
MAVERICKS_ROOT="$(cd "$SELF/.." && pwd)"; export MAVERICKS_ROOT
. "$SELF/msc.sh"
exec sh "$SHIPYARD/release-notes-file.sh" "${1:?TAG required}" "${2:?FULL version required}" "Mavericks Machotool"
```

The product name is the **app-identity** register — "Mavericks Machotool", not "Machotool for Mavericks" — because it drives the Sparkle "A new version of ___ is available" string.

- [ ] **Step 4: Create `release-notes/README.md`**

Document the convention: one file per released version, named for the tag, and that the publish job **fails if the notes are missing or empty** — the defect that shipped on every tailscale release. Copy the convention from `mavericks-golang/release-notes/README.md` rather than inventing wording.

- [ ] **Step 5: Verify they run, on this 10.9 box**

```bash
/bin/sh -n build/msc.sh build/version.sh build/release-notes-file.sh && echo "sh -n ok"
/bin/ksh -n build/version.sh build/release-notes-file.sh
sh build/version.sh auto
```

These run natively on 10.9, so `sh -n` passing is necessary but not sufficient — actually run `version.sh` and read its output. If it cannot find shipyard, that is `msc.sh`'s third fallback failing and worth fixing here rather than in CI.

- [ ] **Step 6: Commit**

```bash
git add build/ release-notes/
git commit -m "feat: build/ wrappers and release-notes/, per the family shape"
```

---

### Task 3: `renovate.json`

**Files:**
- Create: `renovate.json`

- [ ] **Step 1: Write it**

```json
{
  "$schema": "https://docs.renovatebot.com/renovate-schema.json",
  "extends": ["github>ModernMavericks/shipyard"]
}
```

That is the whole file, and the absence of a `customManager` is the point: there is nothing external to track, which is what self-upstream means. It still earns its place — Renovate's built-in github-actions manager keeps `shipyard/...@v1` and `actions/*@v7` current through the same green gate.

**Do not add `ignoreTests`.** The preset sets `false`, this repo has a `pull_request` trigger producing the required check, and `check-family-conventions.sh` fails a local restatement.

**Do not add `packageRules`.** The family's policy is that if it builds and passes, it ships — patch, minor and major alike. Restrict automerge only where a bad bump would build fine and be wrong, and say so in the rule's `description`. Nothing here qualifies.

- [ ] **Step 2: Verify the gate accepts it**

```bash
sh ../mavericks-shipyard/scripts/check-family-conventions.sh .
```

- [ ] **Step 3: Commit**

```bash
git add renovate.json
git commit -m "feat: renovate.json, extending the shipyard preset"
```

---

### Task 4: The `ver` step and the publish job

**Files:**
- Modify: `.github/workflows/release.yml`

**Interfaces:**
- Consumes: `build/version.sh` (Task 2), the tag trigger (Task 1).

- [ ] **Step 1: Add the `ver` step**

Per the conventions' shared shape: on a tag take `full=tag=$GITHUB_REF_NAME` and `rel=yes`; otherwise compute from `UPSTREAM_VERSION` and the existing tags, then **force `rel=no` when `$GITHUB_REF_NAME != main`** so PRs never publish. Write `VERSION`, set outputs `full`, `tag`, `release`.

Because this repo is self-upstream, `release.yml` computes this itself rather than through `resolve-version.sh` — that script hardcodes `-mavericks.` and stays unused.

- [ ] **Step 2: Add the publish job**

Via the shared reusable workflow, never by hand:

```yaml
publish:
  needs: [build]
  if: needs.build.outputs.publish == 'true'
  permissions: { contents: write }
  uses: ModernMavericks/shipyard/.github/workflows/publish-release.yml@v1
  with: { version: "${{ needs.build.outputs.version }}", artifact: <name> }
```

It regenerates `SHA256SUMS`, uses `RELEASE_NOTES.md` from the artifact as the Release body, and fails if the notes are missing or empty.

Replace `<name>` with this repo's actual artifact name — read it from the existing upload step rather than guessing.

- [ ] **Step 3: Remove the stale comment**

`release.yml:132` currently says *"No publish job yet: there is no .pkg or Sparkle updater to ship."* After this task there is a publish job, shipping binaries. Rewrite it to say what is and is not shipped — binaries yes, `.pkg` and updater not yet — so the comment keeps claiming exactly what the file does.

- [ ] **Step 4: Verify without publishing**

Push to a branch, not to `main`, and confirm: the build runs, `ver` sets `rel=no`, and the publish job does not fire. **Do not push a tag** — that would cut a real release, which is out of scope for this plan and is the repo owner's decision.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/release.yml
git commit -m "feat: compute the version and publish from a tag"
```

---

## Self-review

**Spec coverage.** "The versioning decision" and its table → Task 1. "1. A version step" → Task 4. "2. A publish job" → Task 4. "3. `release-notes/`" → Task 2. "4. `build/` wrappers" → Task 2. "5. `renovate.json`" → Task 3. "6. A `.pkg` and a Sparkle updater" → deferred, per the spec's own suggestion, in "What this plan does not do". "Settled against the live shipyard checkout" → Task 1 Steps 1 and 4 carry its two live findings (the concurrency group already conforms and must not change; the artifact check needs a declared deviation). "Sequencing note" → the Depends-on line. "Out of scope" → no tasks.

**Placeholder scan.** One deliberate `<name>` in Task 4 Step 2, with an instruction to read the real value from the existing upload step rather than guess. Everything else is literal.

**Type consistency.** `$SHIPYARD` and `$SHIPYARD_SCRIPTS` are distinct on purpose and used as the sibling repos use them: `$SHIPYARD_SCRIPTS` is what the install action exports, `$SHIPYARD` is what `msc.sh` resolves it into for the `build/` wrappers. `MAVERICKS_ROOT` is exported by both wrappers because shipyard's implementations read it.

**One thing this plan deliberately does not verify.** Task 1 Step 5 and Task 3 Step 2 run `check-family-conventions.sh` from a sibling checkout of shipyard. That checkout may be ahead of or behind what CI installs via `@v1`. If the two disagree, CI is the authority for whether a release works and the sibling is the authority for what the family currently intends — report the disagreement rather than silently satisfying whichever is closer to hand.
