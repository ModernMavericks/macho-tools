# Release conformance: versioning, and the machinery a release needs

**Status:** design, 2026-09-10. Blocks cutting a release; blocks nothing else.

**Sequenced after** the rename
(`2026-09-10-machotool-rename-and-target-design.md`), because several artifacts
named here carry the tool's name and renaming them twice would be waste.

## The finding that starts this

`CMakeLists.txt:9-14` states the repo's current position:

> *"this repo is its OWN upstream, in ed25519's sense of the phrase — the thing it
> exists to port is itself. So `UPSTREAM_VERSION` is the family's usual file and
> the family's usual `<version>-mavericks.N` shape still applies."*

**The premise is right and the analogy is wrong.** `mavericks-ed25519` is a
**port** — the family conventions list it among "date-versioned ports …
`mavericks-ed25519` `20221003-mavericks.N` … where `UPSTREAM_VERSION` is a date
but they are still repackaging an upstream." Its `-mavericks.N` means "our Nth
repackage of *someone else's* ed25519."

This repo has no someone-else. `docs/PROPOSAL.md` settled that:

> *"So: **one repo, first-party, no `UPSTREAM_VERSION`.** Source, tests, the
> `.pkg`, and the Sparkle appcast together."*

And the conventions are explicit about what that implies:

> *"A repo that is its own upstream — original ModernMavericks code, not a port
> (e.g. `mavericks-porthole`) — has no 'repackage-of-someone-else' axis, so it
> **drops the `-mavericks` suffix** and versions itself directly."*

The right analogue is **`mavericks-porthole`**, not ed25519.

## The versioning decision

**Version as `X.Y.Z`. No `-mavericks` suffix. Tag `X.Y.Z`.**

The conventions offer three self-upstream shapes: date-based `YYYYMMDD.N`
(porthole), semver `vX.Y.Z`, or dimmit's `v0.0.YYYYMMDD.N`. Semver, because this
product's user-visible surface is a **CLI grammar that consumers probe**:
`--capabilities` exists precisely so a wrapper and the tool need not move in
lockstep, and the grammar is about to change materially (`macho9` → `machotool`,
`lc` → `load-command`, `declassify` → `fixups set classic`, the `edit` verb, the
`target` statement). A version that communicates compatibility is worth more here
than one that communicates recency. A date says when; semver says whether your
script still runs.

`UPSTREAM_VERSION` **stays**, holding this repo's own version, hand-bumped — the
conventions permit exactly this for a self-upstream repo ("`UPSTREAM_VERSION` (if
used) is the repo's OWN version/date, hand-bumped (no Renovate datasource —
nothing external to track)"). `CMakeLists.txt` already reads it, and keeping one
declaration rather than two is right. Only the **comment** justifying the scheme
changes, and the `-mavericks.N` machinery around it goes.

Concretely:

| | now | after |
|---|---|---|
| `UPSTREAM_VERSION` | `0.1.0`, hand-bumped | unchanged in form; still hand-bumped |
| release version | `0.1.0-mavericks.N` | **`0.1.0`** |
| tag trigger | `tags: ['*-mavericks.*']` | **`tags: ['[0-9]*.[0-9]*.[0-9]*']`** |
| `local_release` dispatch | cuts `-mavericks.(N+1)` | **removed** — there is no repackage axis |
| `resolve-version.sh` | n/a | **stays unused** — it hardcodes `-mavericks.` |
| `/VERSION` | gitignored | unchanged (gitignored build product) |

**Dropping `local_release` deserves its own sentence**, because it removes a
capability. The `-mavericks.N` axis exists to re-release *the same upstream* with
different packaging. With no external upstream there is nothing to re-release
against: a packaging change here is a change to the product, so it earns a
version bump like any other. If a rebuild-without-source-change is ever genuinely
needed, that is a patch bump.

## What a release needs that does not exist yet

`release.yml` today builds, gates, and uploads raw binaries as a CI artifact.
There is **no `ver` step, no publish job, no `.pkg`, and no updater** — which is
consistent with never having cut a release, and is the bulk of this work.

**1. A version step.** Per the conventions' shared shape: id `ver`, on a tag take
`full=tag=$GITHUB_REF_NAME` and `rel=yes`; otherwise compute from
`UPSTREAM_VERSION` and the existing tags, then **force `rel=no` when
`$GITHUB_REF_NAME != main`** so PRs never publish. Writes `VERSION`, sets outputs
`full`/`tag`/`release`. Because this repo is self-upstream, `release.yml` computes
this itself rather than through `resolve-version.sh`.

**2. A publish job**, via the shared reusable workflow — never by hand:

```yaml
publish:
  needs: [build]
  if: needs.build.outputs.publish == 'true'
  permissions: { contents: write }
  uses: ModernMavericks/shipyard/.github/workflows/publish-release.yml@v1
  with: { version: "${{ needs.build.outputs.version }}", artifact: <name> }
```

It regenerates `SHA256SUMS`, uses `RELEASE_NOTES.md` from the artifact as the
Release body, and **fails if the notes are missing or empty** — the defect that
shipped on every tailscale release.

**3. `release-notes/`** with a `README.md` documenting the convention, plus the
`build/release-notes-file.sh` wrapper so the appcast always gets a non-empty file.

**4. `build/` wrappers.** `build/msc.sh` to locate `$SHIPYARD_SCRIPTS`, and
`build/version.sh` / `build/release-notes-file.sh` exec'ing the shared
implementations. POSIX `/bin/sh` only — these must run natively on 10.9.

**5. `renovate.json`.** Absent today. It extends the shipyard preset and needs
**no upstream `customManager`** — there is nothing external to track, which is the
whole point of self-upstream. It still earns its place: Renovate's built-in
github-actions manager keeps `shipyard/...@v1` and `actions/*@v7` current through
the same green-gate. Do **not** set `ignoreTests` locally: the preset sets
`false`, this repo has a `pull_request` trigger producing the required check, and
`check-family-conventions.sh` fails a local restatement.

**6. A `.pkg` and a Sparkle updater.** `docs/PROPOSAL.md` intends one —
*"ModernMavericks additionally ships a `.pkg` for people who want the tools on
their 10.9 machine"* — and the conventions require the updater not link the
product it updates. This is the largest single piece and the one most reasonably
split into its own increment; everything above can land first and produce a
GitHub Release of the binaries.

## Two things to resolve with the family, not unilaterally

**The concurrency group.** Commit `07b2811` deliberately keyed non-PR runs on
`github.run_id`, reasoning that `cancel-in-progress: false` protects the *running*
job and not the *queued* one — GitHub keeps only the newest pending run per group
and cancels the rest, so a shared group discards runs silently. The commit cites a
real observed loss in `mavericks-golang`.

The conventions describe the opposite shape and state that
`check-family-conventions.sh` **fails** a `run_id`-keyed group, because a
dispatch lock keyed per run is no lock at all.

Both are right about different failures. **The conventions gate currently passes
on this repo**, so either the check is not in `@v1` yet or it is narrower than the
prose. Do not quietly change this to match the doc: the reasoning in `07b2811` is
evidence the family shape has a hole, and the resolution belongs in shipyard
where every repo gets it. Note that dropping `local_release` (above) removes the
dispatch-collision case the family shape exists to prevent, which makes this
repo's variant *more* defensible, not less.

**Whether a developer-tools repo wants a Sparkle updater at all.** The conventions
say every product ships one, and `mavericks-golang` — also a toolchain rather than
an app — does. Following the family is the default and deviating needs a written
reason. Raised because it is the kind of thing worth asking once rather than
assuming twice.

## Sequencing note

`release.yml`'s artifact list names `macho9`, `macho9-compat.sh` and
`macho9-translate.sh`. The rename spec changes all three. Land the rename first,
or this work edits the same lines twice.

## Out of scope

- The rename itself, and the `target` statement.
- Anything in the edit-script language.
- Actually cutting a release. This makes one possible; it does not make one.
