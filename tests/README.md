# tests

Three suites, all run by `ctest` (and so by shipyard's `run-repo-tests.sh`):

| test | what it proves |
|---|---|
| `macho_grow_test` | hermetic: the grow, every base-relative re-baser, `mg_verify`, `mg_plausible`, against synthetic images |
| `change_dylib_test` | builds real dylibs, rewrites a real executable, and **runs it** — a wrong library ordinal shows up as a dyld failure, not a silent mis-binding |
| `characterize` | **build equivalence**: the pipeline's output over `fixture.macho` must match `EXPECTED` |

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

## Known gap

`fixture.macho` was built on 10.9, so it carries **no chained fixups** and does
not exercise `patch_macho`'s conversion — the heaviest transform in the
pipeline. A second fixture from a modern toolchain should be added; 10.9's clang
cannot emit one. Until then this proves equivalence over the load-command
editing and header growth, not over the fixups conversion.
