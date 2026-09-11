cmake_minimum_required(VERSION 3.16)

# These tools RUN on 10.9 -- they are the thing being ported, not a cross-built
# artifact -- so the deployment target and architecture are the product, not a
# packaging detail.
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.9 CACHE STRING "runs on 10.9")
set(CMAKE_OSX_ARCHITECTURES x86_64 CACHE STRING "Intel-only (10.9)")

# Versioning: this repo is its OWN upstream, in ed25519's sense of the phrase --
# the thing it exists to port is itself. So UPSTREAM_VERSION is the family's
# usual file and the family's usual `<version>-mavericks.N` shape still applies;
# what differs is only that no Renovate customManager watches it, because nothing
# external releases and bumps it. Bumped by hand; the release cuts the -mavericks.N.
#
# Read from the file rather than duplicated here: the conventions gate requires a
# computable version, and two declarations would be one too many.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/UPSTREAM_VERSION" MAVERICKS_VERSION LIMIT_COUNT 1)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" MAVERICKS_VERSION_NUMERIC "${MAVERICKS_VERSION}")

project(MachOTools VERSION ${MAVERICKS_VERSION_NUMERIC} LANGUAGES C)

# MavericksShipyard, not MavericksSharedCMake: shipyard ships
# MavericksShipyardConfig.cmake, and the family is mid-rename (six local repos
# still ask for the old name, six for the new). New repos take the new one.
find_package(MavericksShipyard REQUIRED)
include(Mavericks)             # AppleClang gate + build conventions
include(MavericksMode)         # native-on-10.9 vs cross-on-modern
include(MavericksCompatGuard)  # assert every shipped binary is 10.9-safe

mavericks_build_mode(MAVERICKS_MODE)
mavericks_require_expected_mode("${MAVERICKS_MODE}")
message(STATUS "macho-tools ${MAVERICKS_VERSION}: build mode ${MAVERICKS_MODE}")

# Cross builds compile against the PINNED 10.9 SDK, not the host's. Building
# with a modern SDK and only -mmacosx-version-min=10.9 is weaker: the compiler
# would happily accept declarations 10.9's headers never had, and the first
# symptom would be a link or load failure on the target. Native builds already
# have the right headers, so they need nothing.
if(MAVERICKS_MODE STREQUAL "cross")
  mavericks_fetch_sdk(MAVERICKS_SDK)
  set(CMAKE_OSX_SYSROOT "${MAVERICKS_SDK}" CACHE PATH "pinned 10.9 SDK" FORCE)
  message(STATUS "macho-tools: cross build against ${MAVERICKS_SDK}")
endif()

# No -std=: src/declassify.c (patch_macho's conversion, extracted in Task 0.6b)
# assigns to an anonymous struct, which clang rejects under -std=c11 and accepts
# in its default gnu dialect. Verified across all six.
set(CMAKE_C_STANDARD_REQUIRED OFF)

# The toolkit library the seven rewriters are being converged onto (docs/PROPOSAL.md
# "Shape"; docs/superpowers/plans/2026-09-08-macho9-toolkit.md). It starts with the
# smallest shared thing and grows one extraction at a time.
#
# Target name is macho9core, not macho9, so Task 4 can add the `macho9` CLI
# executable without a target-name collision. It is deliberately NOT linked into
# every tool: only what actually includes a header from src/ links it, so a tool
# that gains a dependency does so visibly, in this file.
add_library(macho9core STATIC src/uleb.c src/image.c src/ordinals.c src/fat.c src/trie.c src/lc_kinds.c src/atomic_write.c src/linkedit.c src/grow.c src/rewrite.c src/version_min.c src/segname.c src/swift_retag.c src/declassify.c src/script.c)
target_include_directories(macho9core PUBLIC src)
target_compile_options(macho9core PRIVATE -O2 -Wall)

# ALL SIX historical tools are now /bin/sh WRAPPERS around macho9 (the
# retirement plan's Task 2 did five; this plan's Task 3 did fix_macho, the
# last one). Each keeps its original installed name, because
# mavericksforever.com/claude/install.sh fetches patch_macho, change_dylib and
# add_version_min BY THOSE NAMES -- the wrappers are drop-in, and
# compat/<tool>.sh's own header says how each one maps that tool's grammar,
# its exit codes and its stdout onto macho9's.
#
# fix_macho was the holdout: it could not be wrapped while a wrapper had to
# PRESERVE its behaviour, because `macho9 dylib` rewrites a longer path using
# header pad where fix_macho refused outright, and a chained -rename_seg
# chains where fix_macho's single pass did not. The repo owner ruled those --
# and three more -- improvements to ADOPT deliberately, which is what made
# this possible; compat/fix_macho.sh's "DELIBERATE DIVERGENCES FROM fix_macho"
# block states all five with their reasons.
#
# So macho9 is now the ONLY Mach-O rewriting binary this repo builds or
# installs, and compat/ contains no C at all. MACHO_TOOLS is deliberately
# still here, empty: it is the list a future compiled compat tool would join,
# and its emptiness is the measurable form of "the goal is met".
set(MACHO_WRAPPERS
  patch_macho
  change_dylib
  add_version_min
  rename_segment
  retag_swift_classes
  fix_macho)

set(MACHO_TOOLS)

foreach(tool IN LISTS MACHO_TOOLS)
  add_executable(${tool} compat/${tool}.c)
  target_compile_options(${tool} PRIVATE -O2 -Wall)
  target_link_libraries(${tool} PRIVATE macho9core)
  # The compat guard is the in-CI equivalence proof: no 10.9 runner validates
  # these, so the build fails if a binary declares a floor above 10.9 or links a
  # symbol 10.9 lacks. It matters more here than in most of the family, because
  # these tools are themselves what makes other binaries loadable on 10.9.
  mavericks_assert_binary_compatible(${tool})
endforeach()

# The multi-call CLI (Task 4, docs/PROPOSAL.md "Verbs"): every verb it
# implements delegates in-process to macho9core -- `verify`/`info`/`grow` to
# src/grow.h and src/image.h, `dylib`/`rpath`/`lc` to src/rewrite.h and
# `minos` to src/version_min.h. Those last four used to be run as SUBPROCESSES
# (change_dylib / add_version_min next to macho9), which made macho9 depend at
# runtime on the very binaries it is meant to replace; Task 0.5 extracted the
# rewrite into src/ so both callers share the code instead of the binary. Kept
# out of the ${tool}.c-named foreach above because its source lives in cli/. No
# root include path is needed: every header it includes now lives in src/,
# reached via macho9core's PUBLIC src include (below), same as every other tool.
add_executable(macho9 cli/macho9.c)
target_compile_options(macho9 PRIVATE -O2 -Wall)
target_link_libraries(macho9 PRIVATE macho9core)
mavericks_assert_binary_compatible(macho9)

# Stage the wrappers and the two files they source NEXT TO macho9 in the build
# tree, under their installed names, so ctest exercises exactly the layout
# that ships -- a wrapper looks for macho9, macho9-compat.sh and
# macho9-translate.sh in its own directory. $<TARGET_FILE_DIR:macho9> rather
# than CMAKE_CURRENT_BINARY_DIR so this holds for a multi-config generator
# too, and a plain ALL target rather than OUTPUT rules because a generator
# expression in an OUTPUT needs CMake 3.20 and this project asks for 3.16.
# copy_if_different keeps the per-build cost to eight stat calls (six
# wrappers plus the two files they source).
#
# translate.sh is the one file renamed on the way out: a bin directory full of
# tools should not gain a file called `translate.sh`. Nothing else about it
# changes, and compat/macho9-compat.sh is what knows the installed name.
add_custom_target(compat_wrappers ALL
  COMMENT "staging the compat wrappers next to macho9")
add_dependencies(compat_wrappers macho9)
foreach(tool IN LISTS MACHO_WRAPPERS)
  add_custom_command(TARGET compat_wrappers POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/compat/${tool}.sh"
            "$<TARGET_FILE_DIR:macho9>/${tool}")
endforeach()
add_custom_command(TARGET compat_wrappers POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/compat/macho9-compat.sh"
          "$<TARGET_FILE_DIR:macho9>/macho9-compat.sh")
add_custom_command(TARGET compat_wrappers POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/compat/translate.sh"
          "$<TARGET_FILE_DIR:macho9>/macho9-translate.sh")

# ${MACHO_TOOLS} is empty now that fix_macho is a wrapper, so this installs
# macho9 and nothing else. It is left expanded rather than deleted for the
# same reason the empty list above is: a future compiled compat tool joins
# that list and needs no change here.
install(TARGETS ${MACHO_TOOLS} macho9 RUNTIME DESTINATION bin)

# The wrappers install under the historical binaries' exact names, into the
# same directory as macho9 -- that co-location is how each one finds macho9
# and its two support files at run time.
foreach(tool IN LISTS MACHO_WRAPPERS)
  install(PROGRAMS "compat/${tool}.sh" DESTINATION bin RENAME "${tool}")
endforeach()
install(PROGRAMS "compat/macho9-compat.sh" DESTINATION bin)
install(PROGRAMS "compat/translate.sh" DESTINATION bin RENAME macho9-translate.sh)

enable_testing()

# Hermetic: the grow, every base-relative re-baser, mg_verify and mg_plausible,
# against synthetic images built in memory. -Wno-unused-function because
# src/grow.h declares helpers this translation unit does not all call.
add_executable(grow_test tests/grow_test.c)
# -Wall deploys lesson nine's detection net (tests/README.md): a
# -Wfortify-source "will always overflow" diagnostic on a strcpy into a
# char[16] segname/sectname fires here at COMPILE time, on any host, well
# before a modern-clang runner would abort at RUNTIME building the same
# fixture -- and this file holds the fixture builders. It was missing
# -Wall entirely (silent since c06bdce first added this target); adding it
# used to produce exactly one warning, unrelated to this file: a `static`
# mg_trie_scan defined inside header-only macho_grow.h, included into
# multiple translation units, tripped -Wunneeded-internal-declaration. Task
# 3's move to src/grow.c gave every one of these functions external linkage,
# which is exactly what that warning was asking for -- it is gone now, not
# suppressed. -Wno-unused-function stays: src/grow.h declares helpers this
# translation unit does not all use.
target_compile_options(grow_test PRIVATE -O2 -Wall -Wno-unused-function)
target_link_libraries(grow_test PRIVATE macho9core)
add_test(NAME grow_test COMMAND grow_test)

# Reads tests/fixture.macho, a real 10.9-built executable, so the walk is
# exercised against a binary a linker actually emitted rather than one this test
# invented. WORKING_DIRECTORY is the source dir because the fixture path is
# relative. (change_dylib_test.sh, below, needs no WORKING_DIRECTORY of its
# own: it `cd`s to its own directory itself, the way every sh-based test here
# does.)
add_executable(image_test tests/image_test.c)
target_compile_options(image_test PRIVATE -O2 -Wall)
target_link_libraries(image_test PRIVATE macho9core)
add_test(NAME image_test COMMAND image_test
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Hermetic tests for src/trie.c's export-trie rebuild (Task 5 gap 1): decode,
# shift, re-serialize, all against hand-built and hand-computed trie byte
# buffers -- no fixture file needed, same reasoning as grow_test.
add_executable(trie_test tests/trie_test.c)
target_compile_options(trie_test PRIVATE -O2 -Wall)
target_link_libraries(trie_test PRIVATE macho9core)
add_test(NAME trie_test COMMAND trie_test)

# Hermetic tests for src/script.c's tokenizer: hand-written script text and
# the field vector it must produce, so no fixture file is needed -- same
# reasoning as trie_test. The quoting cases are exactly the shapes
# compat/translate.sh's mt_quote emits, because the spec requires the
# generator and the parser cannot drift.
add_executable(script_test tests/script_test.c)
target_compile_options(script_test PRIVATE -O2 -Wall -Wextra)
target_link_libraries(script_test PRIVATE macho9core)
add_test(NAME script_test COMMAND script_test)
# Ruling 16: test_error_message_quoting_survives_the_free (a use-after-free
# regression test) does not reliably fail on a plain run -- libc's allocator
# typically leaves a freed block's bytes untouched until reused, so the
# use-after-free read often still returns the original text by luck.
# MallocScribble=1 overwrites every freed block with 0x55 on free, so ctest
# actually exercises the regression this test exists to catch.
set_tests_properties(script_test PROPERTIES ENVIRONMENT MallocScribble=1)

# Hermetic tests for src/linkedit.c's ml_bump_all (Task 2b): a synthetic
# image built by hand via mi_wrap, so no fixture file is needed -- same
# reasoning as trie_test.
add_executable(linkedit_test tests/linkedit_test.c)
target_compile_options(linkedit_test PRIVATE -O2 -Wall -Wextra)
target_link_libraries(linkedit_test PRIVATE macho9core)
add_test(NAME linkedit_test COMMAND linkedit_test)

# Builds real dylibs, rewrites a real executable, and RUNS it -- a wrong library
# ordinal shows up as a dyld failure rather than a silent mis-binding. Now
# matches chained-fixups.sh/characterize.sh/cli_test.sh/leaf-tool-crashes.sh:
# passed the bindir so it runs the change_dylib and fix_macho wrappers CMake
# actually staged (and the macho9 under them), instead of compiling its own
# copies from a hand-enumerated source list that had to be kept in sync with
# macho9core by hand (Task 5) -- it still builds macho9 from source, deriving
# the list from a glob, and copies the two wrappers beside it, when run
# standalone with no bindir argument.
add_test(NAME change_dylib_test
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/change_dylib_test.sh" "$<TARGET_FILE_DIR:macho9>")

# Build equivalence. The tool binaries can never match across a 2014 clang and a
# 2026 one, so comparing them is the wrong invariant. These tools are
# deterministic file transformers, so pin what they PRODUCE: run the pipeline
# over a committed fixture and compare the digest. Native-on-10.9 and
# cross-from-a-modern-host must agree, which is the equivalence claim made
# checkable rather than asserted.
# Covers patch_macho's chained-fixups conversion, which the characterization
# above cannot: its fixture is 10.9-built and has no chained fixups to convert.
# SKIPs (77) on hosts that cannot emit them -- 10.9 included -- so it is real
# coverage on a modern host and an honest no-op on the target.
add_test(NAME chained_fixups
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/chained-fixups.sh" "$<TARGET_FILE_DIR:macho9>")
set_tests_properties(chained_fixups PROPERTIES SKIP_RETURN_CODE 77)

add_test(NAME characterize
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/characterize.sh" "$<TARGET_FILE_DIR:macho9>" check)

# macho9's own CLI: --capabilities plus each verb it implements. Builds its own
# fixtures (FIXTURE_FLAGS = -mmacosx-version-min=10.9, same reasoning as
# change_dylib_test.sh) rather than reading tests/fixture.macho, so no
# WORKING_DIRECTORY is needed -- everything it needs is the bindir, passed the
# same way chained_fixups passes patch_macho's.
add_test(NAME cli_test
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.sh" "$<TARGET_FILE_DIR:macho9>")

# compat/translate.sh: one assertion per translation from an old tool's grammar
# to macho9's, pinning the EXACT emitted command line. Text only -- it never
# runs macho9 on a file; whether the emitted commands produce the same BYTES is
# tests/compat-sweep.sh's job, and that one is run by hand (it needs both
# families of binaries and takes minutes), with its result committed as
# tests/compat-matrix.tsv. The bindir is here for one thing: the last block
# checks every verb/op/KIND the translator can emit against
# `macho9 --capabilities`, rather than assuming they agree.
add_test(NAME translate_test
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/translate_test.sh" "$<TARGET_FILE_DIR:macho9>")

# The six /bin/sh wrappers' own behaviour: the grammar each translates, the
# exit codes it maps back to the C tool's, and the stdout it reshapes. Sits
# between translate_test (which pins the emitted TEXT and never runs macho9 on
# a file) and known_callers (which replays the real callers end to end); every
# assertion in it names the divergence it closes, from the list at the top of
# compat/translate.sh or from cli/macho9.c's own "DELIBERATE DIVERGENCES"
# blocks. Also parses every wrapper under /bin/sh AND /bin/ksh, the same
# second-shell cross-check translate_test does.
add_test(NAME wrapper_test
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/wrapper_test.sh" "$<TARGET_FILE_DIR:macho9>")

# THE GATE for the compat-retirement plan's Task 2: every known caller of the
# six historical tools, replayed end to end against the wrappers. The plan
# says it outright -- "A wrapper that passes the test suite but breaks a real
# caller is a failure" -- and weights the argument sweep as discovery and
# these replays as decisive. Each pipeline's result is pinned to the SHA-256
# the C binaries built from commit 91b30b3 (the last commit carrying all six
# compat/*.c files) produced from tests/fixture.macho on real 10.9, the same
# device tests/EXPECTED uses.
add_test(NAME known_callers
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/known-callers.sh" "$<TARGET_FILE_DIR:macho9>")

# Regression coverage for two heap-overflow crashes a code review found in
# add_version_min and retag_swift_classes after Task 1 converted them onto
# src/image.h -- mi_open validates load commands, not section file ranges or
# addresses derived from them, so a fixture can pass mi_open cleanly and
# still contain an out-of-bounds reference. See the script's own header for
# what each fixture is shaped to trigger.
add_test(NAME leaf_tool_crashes
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/leaf-tool-crashes.sh" "$<TARGET_FILE_DIR:macho9>")

# src/live.h -- the header-only, malloc-free query surface Task 4 adds for
# avxemu (docs/PROPOSAL.md's "Shape": "live.h is deliberately a header, not
# part of the library"). Deliberately NOT linked against macho9core: live.h
# is standalone by design (see its own top comment), and this test proves
# that by needing nothing but -I src. Queries run against this test binary's
# OWN loaded image via _dyld_get_image_header/_dyld_get_image_vmaddr_slide,
# same reasoning as image_test reading a real linker-built fixture instead
# of a synthetic one. WORKING_DIRECTORY is the source dir because the test
# shells out to `cc -c tests/live_probe.c` and `nm` on the result to prove
# a TU that includes only live.h stays free of malloc/free/stdio -- the
# constraint pin `tests/README.md` and the task brief both call for, wired
# into CI rather than left as an unchecked comment.
add_executable(live_test tests/live_test.c)
target_compile_options(live_test PRIVATE -O2 -Wall -Wextra)
target_include_directories(live_test PRIVATE src)
add_test(NAME live_test COMMAND live_test
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
