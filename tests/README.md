# tests

Fifteen suites, all run by `ctest` (and so by shipyard's `run-repo-tests.sh`):

| test | what it proves |
|---|---|
| `grow_test` | hermetic: the grow, every base-relative re-baser, `mg_verify`, `mg_plausible`, against synthetic images |
| `image_test` | walks `tests/fixture.macho`, a real 10.9-built executable, against `src/image.c`'s reader — exercised against a binary a linker actually emitted, not one this test invented |
| `trie_test` | hermetic: `src/trie.c`'s export-trie rebuild (decode, shift, re-serialize) against hand-built and hand-computed trie byte buffers — no fixture file needed, same reasoning as `grow_test` |
| `linkedit_test` | hermetic: `src/linkedit.c`'s `ml_bump_all` (the `__LINKEDIT` offset-bump table) against a synthetic image built by hand via `mi_wrap` — no fixture file needed, same reasoning as `trie_test` |
| `script_test` | hermetic: `src/script.c`'s edit-script tokenizer and parser against hand-written script text — the quoting shapes `compat/translate.sh`'s `mt_quote` emits, the statement table, the directives, and parse errors reported by line. Run with `MallocScribble=1` so its use-after-free regression test actually fails if the bug returns |
| `edit_test` | hermetic: `src/edit.c`'s execution model against a synthetic image written to a temp directory — statements apply in order and each sees the last one's result, a refusal part-way leaves the input byte-for-byte and inode-for-inode untouched with no `--output` or temp file created, a dry run writes nothing but still verifies, the final verify ignores `MACHO_NO_VERIFY`, and only a thin 64-bit Mach-O is accepted |
| `change_dylib_test` | builds real dylibs, rewrites a real executable, and **runs it** — a wrong library ordinal shows up as a dyld failure, not a silent mis-binding. Also covers `src/fat.c`'s fat-arch validation (both read-side, via `fix_macho`, and write-side) and `write_atomic`'s symlink/hard-link/ordinary-file handling |
| `chained_fixups` | `patch_macho`'s chained-fixups conversion, against a fixture only a modern linker can produce. `SKIP`s (exit 77) on a host that can't emit chained fixups — 10.9 included — so it's real coverage on a modern host and an honest no-op on the target |
| `characterize` | **build equivalence**: the pipeline's output over `fixture.macho` must match `EXPECTED` |
| `cli_test` | `macho9`'s own CLI: `--capabilities` (including that its advertised kinds=/ops= match what the parsers actually accept) plus one exemplar op per verb it implements. `rpath -insert`, `segment` and `retag-swift` get more than one exemplar each, because each has an observable the exemplar alone cannot pin: `-insert` is only correct if the new search path lands FIRST (an `-append` of the very same path, asserted to land LAST, is what rules out a silent downgrade), `segment` has to rename each section's own copy of the segment name (`macho9 info` does not print those, so a purpose-built reader does) and has to work on a fat container, and `retag-swift` has to move the tag bit without disturbing the rest of the word |
| `leaf_tool_crashes` | regression coverage for heap-overflow/out-of-bounds crashes found by code review in `add_version_min`, `retag_swift_classes` and `patch_macho` after each was converted onto `src/image.h` — hand-built fixtures that pass `mi_open`'s load-command validation cleanly while still containing a section/offset a tool used to dereference unconditionally. Also holds `macho9`'s load-command rewriters (`dylib`, `rpath`, `lc`, `segment`, `edit`) and `grow` to refusing, byte-for-byte unchanged, an image with no section data, `grow` to refusing one whose first section lies past its end, and `info` to calling its header pad unknown |
| `translate_test` | `compat/translate.sh`, the old-grammar-to-`macho9` translator Task 2's wrappers source: one assertion per translation, pinning the EXACT emitted command line (every flag of all six tools, every `-strip-lc` KIND, the mixed-family `lc`/`dylib`/`rpath` ordering, `install.sh`'s production line, the quoting, every refusal's origin message, and both capacity caps). Also checks every verb/op/KIND the translator can emit against `macho9 --capabilities` rather than assuming they agree, and re-runs one translation under `/bin/ksh` so a bashism fails here rather than on the target |
| `wrapper_test` | the six `/bin/sh` wrappers that replaced `patch_macho`, `change_dylib`, `add_version_min`, `rename_segment`, `retag_swift_classes` and `fix_macho`: the grammar each translates, the exit codes it maps back to the C tool's (`patch_macho`'s flat 1 where `macho9` returns `EX_FAIL`, or `EX_REFUSED`, which is already 1; `rename_segment`'s 2 for "nothing matched"; `retag_swift_classes`' silent skip of a non-Mach-O), and the stdout it reshapes. Every assertion names the divergence it closes, from the list at the top of `compat/translate.sh` or from `cli/macho9.c`'s own "DELIBERATE DIVERGENCES" blocks. Also parses every wrapper under `/bin/sh` **and** `/bin/ksh`, the same second-shell cross-check `translate_test` does |
| `known_callers` | **the gate for the wrappers**: every known caller of the six historical tools, replayed end to end — `mavericksforever.com/claude/install.sh`'s generated `/usr/local/bin/claude` wrapper first, then the repo owner's local `-insert` variant and `magic-trackpad2`'s recorded invocations — plus the atomicity property a mixed-family refusal must keep (the caller's file untouched). Each pipeline's result is pinned to the SHA-256 the **C binaries built from commit `91b30b3`** produced from `fixture.macho` on real 10.9, the same device `EXPECTED` uses. The retirement plan says it outright: "a wrapper that passes the test suite but breaks a real caller is a failure" |
| `live_test` | `src/live.h`, the header-only malloc-free query surface for `avxemu`: queries run against this test binary's OWN loaded image (`_dyld_get_image_header` etc.), and a separate compile-and-`nm` check proves a translation unit that includes only `live.h` stays free of `malloc`/`free`/stdio |

## Not run by `ctest`: the two by-hand sweeps

Two scripts here need more than a build tree and are run deliberately, on real
hardware, with their results committed:

- **`differential.sh`** runs TWO builds of these tools over hundreds of real
  Mach-Os and proves they behave identically. It exists for the "the work
  moved, the behaviour must not" shape of change. It compares bytes, exit
  code, stdout **and stderr** — so note that comparing a pre-wrapper build
  against a current one reports a **stderr** difference on every wrapper
  invocation, because printing the `macho9` equivalent there is the whole
  point of the wrappers. Read its report by category: bytes, exit and stdout
  are the ones that must be clean.
- **`compat-sweep.sh`** runs every enumerated argument combination of the six
  historical tools BOTH ways -- the old binary, and `compat/translate.sh`'s
  `macho9` command line(s) -- on the same input, and records what each did in
  `compat-matrix.tsv`: singles, ordered pairs and ordered triples of every
  flag (1110 combinations for `change_dylib`, 39 for `fix_macho`), plus every
  arity the four flagless tools distinguish, plus hand-picked cases the fixed
  vocabulary cannot reach (the capacity caps, `install.sh`'s production line,
  the chained `-rename_seg`). **Point its `<bindir>` at a build of commit
  `91b30b3`** (`compat: refuse chained -rename_seg, and make the matrix
  replayable`), the last commit carrying all six `.c` files: all six are
  shell wrappers around `macho9` now, so a current build makes the "old
  side" a wrapper and the comparison close to tautological. (`f500021`
  -- `tests: add the real elision detector, aimed at the case that can elide`
  -- is the last commit carrying the *pre-extraction* originals, from before
  change_dylib.c/rename_segment.c/etc. became thin argv-parsers over the
  shared `src/` library; it is not what `known_callers`' pinned digests were
  measured against and is named here only so both landmarks are on record.)
  The bindir it ran against is recorded in the matrix header.

`compat-matrix.tsv` is a **committed artifact, not a report**: Task 2 of the
retirement plan replaced five of the six C sources with shell wrappers, and a
later commit replaced the sixth, `fix_macho`, so all six now exist as C only in
git history. The matrix and the SHA-256s in
it are what outlive them — as are `known-callers.sh`'s pinned pipeline
digests. Regenerate it only from a real 10.9 build of both families, and say
in the commit why a row changed.

## `macho9`'s exit codes

`macho9` uses three exit codes throughout. `verify`, `info`, `grow`, `minos`
and `lc`'s KIND validation always have; `dylib`, `rpath`, `lc` and `minos`
get theirs from the shared rewrite drivers they call into (`mr_apply_file`,
`mv_add_version_min`), which now draw this exact same line themselves for
EVERY considered refusal they can reach -- not only the one `--fatal-
warnings` adds ("an operation matched nothing"), but every refusal those
two functions already had (bad magic, no room to grow, and the rest of
`src/rewrite.h`'s list on `mr_apply_file`). `edit` gets its code from
`me_run` (`src/edit.h`), which draws the same line. Also documented
machine-readably in `--capabilities`' `exitcodes` line:

| code | meaning |
|---|---|
| `0` | success |
| `1` (`EX_REFUSED`) | `macho9` examined the input and declined ON PURPOSE — not a Mach-O, not plausible, an unsupported KIND/version, a `segment` NEW name longer than the 16 bytes a `segname` field holds, a grow `mg_grow_header` itself refused (its own "refuse rather than guess" rule), or an operation that matched nothing — under `dylib`/`rpath`/`lc`'s `--fatal-warnings`, or an edit script's `fatal-warnings` directive. For `dylib`/`rpath`/`lc` this last case never rolls back a write it made: if some OTHER operation in the same run matched, that write already happened; if every operation matched nothing, there was no write to roll back in the first place, same as any other all-miss run. `edit` writes nothing at all when it refuses, this case included: its single write comes after the last statement and the final verify |
| `2` (`EX_FAIL`) | everything else: a syscall or malloc failure, a usage error — genuinely something going wrong, not a considered refusal. ONE EXCEPTION: an allocation failure INSIDE `mg_grow_header` or `mg_plausible` (`src/grow.c`) is folded into `1` instead, same as every other reason either one refuses, on every verb that reaches either one (`dylib`/`rpath`/`lc`, `grow`, `verify`, `edit`) — see `src/rewrite.c`'s comment on that fold |

The numbering is deliberately backwards from what first shipped (`0` ok, `1`
failed, `2` refused): `diff`, `grep` and `cmp` all reserve their highest code
for "the tool could not do its job", not for a normal, expected non-success
answer, and this repo now follows that precedent instead of contradicting
it.

Refusal is load-bearing throughout this codebase (`-grow` refuses rather than
widening a default case is a global rule, not a `macho9`-specific one), so a
caller that wants to script around "this file just isn't one `macho9` will
touch" versus "something is actually broken, investigate or retry" can check
for `1` specifically instead of scraping stderr text. Any existing caller
checking only `== 0` or `!= 0` is unaffected by this distinction's addition
regardless of which of `1`/`2` means which.

`dylib`/`rpath`/`lc` (past its own KIND check) and `minos` (past its own
version check) hand back the exit code of the shared rewrite drivers,
`mr_apply_file` and `mv_add_version_min` (`src/rewrite.h`,
`src/version_min.h`). Those two now use the very same `MR_REFUSED` (1) /
`MR_FAIL` (2) split this table documents -- `mr_apply_file`'s own comment in
`src/rewrite.h` has the full classification, including several sites reached
through a helper's own nonzero return rather than a check written out in
that function -- so their exit codes ARE covered by the table above, exactly
as `cli/macho9.c`'s own `--capabilities` comment says. What the table's
prose does not spell out per verb is WHICH of `mr_apply_file`'s many
considered-refusal cases fired -- that detail is on stderr, not in the exit
code, same as everywhere else in this table.

(They used to be forwarded from a `change_dylib`/`add_version_min`
SUBPROCESS, which returned a flat 0/1 with no refused/failed distinction at
all; the code is linked in now, and its exit codes DO make that distinction
today, which is a real, deliberate behaviour change for the two `compat/`
wrappers of the same names -- they forward this code verbatim, so a genuine
operational failure through either one now exits 2 where the old C tool
always exited a flat 1, apart from one case that reaches `change_dylib`
only: the fold in the table above's `2` row. See `compat/README.md`'s
"drop-in" section, which names this as one of its known exceptions.)

## EXPECTED, and what it is for

`EXPECTED` holds the SHA-256 of what the whole pipeline produces from
`fixture.macho`. **It was recorded from a native build on real 10.9 hardware.**

That is what makes it useful. There is no 10.9 runner in CI, so every build
there is a cross-build, and a cross-build has to be shown equivalent to a native
one rather than assumed to be. Comparing the *tools* is the wrong test — a 2014
clang and a 2026 one will never emit matching bytes. Comparing what the tools
*produce* is the right one, and it works because:

- the pipeline is deterministic (same input and flags, byte-identical output);
- a cross-built tool is x86_64 with a 10.9 **floor**, so it still runs on the
  modern host that built it — CI can execute what it just produced.

If `characterize` fails, either a tool's behaviour changed (update `EXPECTED`
deliberately, in the same commit, and say why) or this build is not equivalent
to the native reference. Do not update `EXPECTED` to make the test pass.

## Writing a test here: lessons from the cross runner

This suite went red on the modern cross runner (CI, or any host newer than
Mac OS X 10.9) seven separate times over the course of this project. Every
one of those seven was a bad assumption baked into a TEST, never an actual
defect in a tool. That track record is worth internalizing before adding a
new fixture or assertion here, because the same handful of mistakes keeps
reproducing the failure in new shapes:

- **A fixture built without `-mmacosx-version-min=10.9` asks a different
  question on a modern host than it does on 10.9.** A modern linker's
  defaults change the load commands it emits (see the next two points), so
  a fixture compiled with the host's defaults tests "whatever this host's
  toolchain happens to do today", not the 10.9 behavior the tool actually
  targets. Every fixture-building helper in this directory and in
  `change_dylib_test.sh` passes `-mmacosx-version-min=10.9` for exactly this
  reason — copy that pattern for any new one rather than reasoning about
  which particular default would otherwise bite.

- **Never parse `nm`/`otool` human-readable output as an oracle.** Their
  output format is Apple's to reformat at will, on any OS release, with no
  compatibility promise to a test script parsing it. When a fact about a
  binary is needed that a stable tool output (`macho9 info`, `--capabilities`)
  doesn't already provide, write a tiny throwaway C program that reads the
  Mach-O structure directly (`ordinal_of.c`, `has_lc.c`, `has_bytes.c`,
  `mk2fat_overlap.c`, and others in `change_dylib_test.sh`;
  `tests/strip_version_min.c`, shared by `cli_test.sh` and `wrapper_test.sh`,
  is the same idiom pointed the other way — it writes the structure, to build
  a fixture, rather than reading it). It asks the same question on a 10.9 host
  and a 2020s one because it depends only on the file format, not on any
  tool's text formatting.

- **A 2026 linker emits `LC_BUILD_VERSION` where a 2014 one emits
  `LC_VERSION_MIN_MACOSX`.** A fixture built to exercise "a binary that has
  no `LC_VERSION_MIN_MACOSX`" needs to check for the ABSENCE of that specific
  load command, not assume a normal build lacks it — a modern host's default
  build already lacks it (it emits `LC_BUILD_VERSION` instead), which makes
  a naive assertion pass for the wrong reason and silently stop testing what
  it claims to. See `cli_test.sh`'s `minos` fixture construction for the
  full story, including the failed first fix below.

- **`-Wl,-no_version_load_command` exists only on 10.9's `ld`.** A first fix
  for the point above tried this flag to suppress the load command at link
  time. It linked locally and broke the whole suite on the cross runner
  (`ld: unknown options: -no_version_load_command`) — trading one host
  dependency for a worse one, a hard link failure instead of a silently-wrong
  assertion. The fix that actually holds up on both hosts: build the fixture
  NORMALLY (portable) and then remove the load command by direct Mach-O
  structure surgery, with a throwaway C program compiled by plain `$CC`, no
  special flags. Any linker flag that isn't in this file already is
  guilty until proven portable — check it against a modern `ld` before
  relying on it, or avoid needing a special flag at all, as above.

- **Modern macOS SIGKILLs a resized binary** (code-signing enforcement: the
  signature made at link time no longer matches once the file is modified
  in place), so **"run the rewritten binary" assertions are 10.9-only.**
  Do not assume this — probe it. `cli_test.sh` establishes whether the
  CURRENT host enforces this by perturbing a copy of a binary that never
  went near macho9 or change_dylib and observing the result (exit 137 means
  yes) before it ever runs a macho9-modified binary; the grow/lc "still
  runs" assertions are gated on that probe, not on a Darwin version check,
  distinguishing "this host's OS policy" from "macho9 broke the binary" —
  the same symptom (the child doesn't run) would otherwise look identical
  and either mask a real defect or fail a totally healthy build.

- **Assert the BEHAVIOUR, not which guard fired, because fixture byte sizes
  vary by toolchain.** Two different checks can both be correct refusals of
  the same underlying condition — see `change_dylib_test.sh` case 13
  (the fat-collision test): the read-side check in `mfat_parse` and the
  write-side check in `process_fat` can both legitimately catch the same
  malformed layout, and which one fires first depends on exact byte offsets
  a *different* host's compiler produced for the same source. Case 13 pins
  the observable outcome instead — refuses, leaves the input untouched,
  names the overlap in its message — never the specific code path or exact
  byte count that produced it. Where a fixture is entirely hand-built
  byte-for-byte (as the fat fixtures in `change_dylib_test.sh` mostly are),
  pinning an exact size or offset is fine — the risk is only when a
  compiler's own output feeds the assertion.

- **Mach-O name fields (`segname`, `sectname`) are `char[16]` and are NOT
  NUL-terminated.** A 16-character name legally fills the field with no room
  left for a terminator — the same trap `src/image.c`'s `name_eq` comment
  documents for the tools themselves, and it bites a fixture-building helper
  exactly as hard. `strcpy`/`sprintf` into one of these fields is wrong
  regardless of host (it writes past the field once the name is 16
  characters), but the two hosts disagree about what happens next: 10.9's
  clang lets the overflow silently land in the next struct member and
  carries on, while a modern clang wraps `strcpy` as `__strcpy_chk` under
  `_FORTIFY_SOURCE` and aborts (SIGTRAP) the instant it detects the
  overflow — so the fixture generator itself dies before writing the file,
  failing the test on the cross runner while it passes natively (this
  suite's eighth cross-runner-only failure: `tests/leaf-tool-crashes.sh`'s
  `mkfixture.c` wrote `"__objc_classlist"`, exactly 16 characters, via
  `strcpy(s->sectname, ...)`). Use `memcpy` with a length capped at 16 and
  zero-fill the rest, never `strcpy`/`sprintf`, for any `segname`/`sectname`
  write in a fixture helper — and note that `-Wall -Wextra` catches this at
  COMPILE time on any host (a `-Wfortify-source` "will always overflow"
  diagnostic), even though the abort itself is modern-clang-only, so it is
  worth compiling every hand-written fixture helper with those flags as a
  check before trusting it.

- **A mutation test proves nothing if the rebuild silently didn't happen.**
  Mutation testing (deliberately break the code, confirm the test you're
  trusting actually fails, then revert) is this project's primary technique
  for proving a test discriminates — used throughout `tests/grow_test.c`
  (formerly `macho_grow_test.c`, before Task 3 of the toolkit convergence
  plan moved `macho_grow.h` to `src/grow.c`/`src/grow.h` and this test with
  it), `tests/trie_test.c`, and `tests/linkedit_test.c`'s own commit
  history. It depends
  entirely on the binary under test actually reflecting the source edit.
  On at least one host, `cmake --build` after a one-line source edit
  produced IDENTICAL results for two different mutations — because the
  edited source file and its stale `.o` ended up with the same one-second
  `mtime`, so `make`'s timestamp comparison judged the object current and
  skipped recompiling it entirely. The mutation silently never took effect;
  the test wasn't discriminating anything, it was just re-running against
  the unmodified binary. This is a strictly worse failure mode than a test
  that's wrong: it looks identical to both "the mutation was caught" and
  "the mutation was missed", so a result from a build that might have
  skipped the rebuild is not evidence either way. It only surfaced because
  two mutation runs gave contradictory results for what should have been
  independent, reproducible outcomes. FIX: when mutating for verification,
  force the rebuild and confirm it actually happened — compile the test
  directly with `cc` (as this repo's hermetic tests' own header comments
  already suggest, e.g. `trie_test.c`'s), or `touch` the source and use
  `cmake --build --clean-first`, or otherwise check the object's mtime
  genuinely advanced past the edit. Never trust a mutation result from a
  build you did not affirmatively force.

## Known gap

`fixture.macho` was built on 10.9, so it carries **no chained fixups** and does
not exercise `patch_macho`'s conversion — the heaviest transform in the
pipeline. A second fixture from a modern toolchain should be added; 10.9's clang
cannot emit one. Until then this proves equivalence over the load-command
editing and header growth, not over the fixups conversion.
