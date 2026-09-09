# Rewrite the Post-Extraction History Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make this repository's post-extraction history read as the designed
progression it turned out to be, rather than the transcript of how it was
discovered.

**Do this LAST.** It is the final task across every macho-tools plan, after the
toolkit, the convergence, and the compat retirement have all landed and are
green. Rewriting history while work is in flight invalidates every in-flight
branch, review diff and ledger reference.

## The boundary — this is the constraint everything else bends around

`PROVENANCE.md` records that this repo was extracted with full history from
[`Wowfunhappy/Mavericks-Porting-Resources`](https://github.com/Wowfunhappy/Mavericks-Porting-Resources),
and that **"Wowfunhappy's is his, under his name, at its original date."**

- [ ] **Task 0: establish the boundary commit and write it into this plan.**
      Find the last commit authored by Wowfunhappy and the first authored after
      the extraction. Everything at or before the boundary is **untouchable** —
      not reworded, not re-dated, not re-authored, not squashed. His name and
      his dates are a provenance record, and this repo's `LICENSE` rests on
      them.
      Verify after every rewrite step that `git log --author=Wowfunhappy` is
      byte-identical to what it was before. Automate that check; do not eyeball it.

## What is actually wrong with the history today

Not that it is messy — that it records **process rather than result**:

- Work done, then undone, then redone: `change_dylib` was converted to
  `mi_open` in one plan and reverted by a later task in the same plan.
- Fixes for bugs introduced two commits earlier, in the same session.
- Review-round churn: "fix round 1", "fix round 2", "the assertion was
  host-dependent, not the tool".
- Decisions accepted as transitional that a later commit reverses.

A reader wanting to know *how this toolkit is built* has to reconstruct it from
a record of *how it was found out*.

## What must NOT be lost

**The commit messages are the most valuable prose in this repository.** They
carry measurements, mutation results, differentials over hundreds of real
binaries, and the reasoning behind refusals. The SDD ledgers and task reports
are gitignored — so for much of this work, **the commit message is the only
surviving record.**

A squash that discards them destroys more than it tidies.

- [ ] **Task 1: harvest before rewriting.** Extract every durable finding from
      the current messages into `docs/` where it belongs — the measurements,
      the mutation tables, the "this shipped broken for months" notes, the
      reasons behind each refusal. `docs/prior-art.md` and `tests/README.md`
      already hold some of this; extend them. **This task must be complete and
      committed before any history is rewritten**, because after the rewrite
      the originals are gone.

## Sequencing

- [ ] **Task 2: design the target history, on paper, before touching git.**
      Write the intended commit list into this plan — subject lines and one
      line of rationale each. Get it reviewed as a document. A history rewrite
      that is improvised mid-rebase produces a different mess.
      Suggested shape (decide it, do not inherit it): the library bottom-up in
      dependency order (`uleb`, `image`, `ordinals`, `fat`, `trie`, `linkedit`,
      `grow`, `live`), then the CLI, then the compat wrappers, then the tests
      and docs that support each.

- [ ] **Task 3: rewrite, with a backup ref that is never deleted.**
      `git branch pre-rewrite-YYYYMMDD` first, and keep it. The reflog is not a
      backup; it expires.

- [ ] **Task 4: every rewritten commit must build and pass its tests.**
      This is what makes a rewritten history trustworthy rather than
      decorative, and it is the expensive part — budget for it. Use
      `git rebase --exec 'cmake --build --preset native-local && ctest --preset native-local'`
      or equivalent. A history that bisects is worth the cost; one that merely
      reads well is worth much less.

- [ ] **Task 5: verify the tree is unchanged.** The final tree after the
      rewrite must be **byte-identical** to the tree before it:
      `git diff pre-rewrite-YYYYMMDD..HEAD` must be empty. If it is not, the
      rewrite changed content, not just history. Also re-run the full suite and
      both CI workflows on the rewritten head.

## Global Constraints

- **Wowfunhappy's commits are untouchable.** See Task 0.
- **The final tree must not change.** Task 5.
- **Every commit builds and passes.** Task 4.
- `PROVENANCE.md`, `LICENSE` and `docs/prior-art.md` describe the history; if
  the rewrite changes what is true about it, they change in the same breath.

## The push

**This requires a force-push to `main`, which is destructive and outward-facing.
Stop and get explicit approval before it, every time — a prior approval of the
plan is not approval of the push.** Note also that `main` is protected by an
org ruleset; a force-push may be refused or may require a bypass, and either
outcome is information the repo owner needs before the attempt, not after.

Anyone with a clone or an open PR is affected. Establish who that is (the
`compat/` retirement plan's Task 0 caller list is a starting point, and
Wowfunhappy is a stakeholder in the provenance) before rewriting, not after.

## Self-Review

**Placeholders.** Task 0's boundary commit and Task 2's target history are
deliberately unfilled — determining them from evidence is the work.

**Measurable done.** `git log --author=Wowfunhappy` unchanged; the tree
byte-identical to `pre-rewrite-*`; every commit builds and passes; CI green on
the rewritten head; and every durable finding from the old messages present in
`docs/`.

**Risk stated plainly.** This is the one plan across the macho-tools series
whose failure mode is losing work rather than shipping a bug. Tasks 1, 3 and 5
exist entirely to make it recoverable.
