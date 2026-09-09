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

| caller | what it does | how to verify |
|---|---|---|
| `mavericksforever.com/claude/install.sh` | builds `patch_macho`, `change_dylib`, `add_version_min` **by name** and runs them against a real Claude Code binary | fetch it, read what it actually invokes, replay those exact invocations |
| `Wowfunhappy/Mavericks-Porting-Resources` | the upstream this repo was extracted from (`PROVENANCE.md`) | check whether it still calls these tools and how |
| this repo's own suites | `tests/change_dylib_test.sh`, `tests/cli_test.sh`, `tests/characterize.sh`, `tests/chained-fixups.sh` | already run in CI |
| avxemu | consumes `src/live.h`, NOT these tools | confirm it is unaffected; do not assume |

- [ ] **Task 0 (do this first): enumerate the real callers and their exact
      invocations.** Do not guess. Fetch `install.sh` and read it. Grep the
      family checkouts under `~/Documents/code/trees/mavericks-*` for calls to
      these six names. Write the list into this plan before proceeding — if the
      list is wrong, everything after it is built on sand.

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
- [ ] **Exhaustive combination sweep, as DISCOVERY.** Drive every enumerated
      combination through the translator. The expectation is that `macho9`
      expresses all of them; the purpose of the sweep is to find where it does
      not. Treat each gap as a finding to bring back and decide on — extend
      `macho9`, or record the spelling as having no equivalent, with the reason.
      Do NOT quietly narrow the wrapper's surface to whatever happened to work.
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
