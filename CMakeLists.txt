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

# No -std=: patch_macho.c assigns to an anonymous struct, which clang rejects
# under -std=c11 and accepts in its default gnu dialect. Verified across all six.
set(CMAKE_C_STANDARD_REQUIRED OFF)

# The toolkit library the seven rewriters are being converged onto (docs/PROPOSAL.md
# "Shape"; docs/superpowers/plans/2026-09-08-macho9-toolkit.md). It starts with the
# smallest shared thing and grows one extraction at a time.
#
# Target name is macho9core, not macho9, so Task 4 can add the `macho9` CLI
# executable without a target-name collision. It is deliberately NOT linked into
# every tool: only what actually includes a header from src/ links it, so a tool
# that gains a dependency does so visibly, in this file.
add_library(macho9core STATIC src/uleb.c src/image.c src/ordinals.c src/fat.c src/trie.c src/lc_kinds.c src/atomic_write.c src/linkedit.c src/grow.c)
target_include_directories(macho9core PUBLIC src)
target_compile_options(macho9core PRIVATE -O2 -Wall)

set(MACHO_TOOLS
  patch_macho
  change_dylib
  add_version_min
  fix_macho
  rename_segment
  retag_swift_classes)

foreach(tool IN LISTS MACHO_TOOLS)
  add_executable(${tool} ${tool}.c)
  target_compile_options(${tool} PRIVATE -O2 -Wall)
  # The compat guard is the in-CI equivalence proof: no 10.9 runner validates
  # these, so the build fails if a binary declares a floor above 10.9 or links a
  # symbol 10.9 lacks. It matters more here than in most of the family, because
  # these tools are themselves what makes other binaries loadable on 10.9.
  mavericks_assert_binary_compatible(${tool})
endforeach()

# change_dylib includes src/grow.h, which includes src/uleb.h, and src/fat.h
# directly; patch_macho includes src/image.h directly; fix_macho includes
# src/fat.h directly (Task 5: both fat-aware tools now validate a fat file's
# arch table through the same code, src/fat.c, instead of each having its own
# hand-rolled walk that could -- and did -- disagree about validation).
# add_version_min, rename_segment and retag_swift_classes join here one at a
# time as Task 1 (docs/superpowers/plans/2026-09-08-macho9-toolkit.md)
# converts each off its own open/fstat/malloc/read/ncmds-walk preamble onto
# src/image.h.
target_link_libraries(change_dylib PRIVATE macho9core)
target_link_libraries(patch_macho PRIVATE macho9core)
target_link_libraries(fix_macho PRIVATE macho9core)
target_link_libraries(add_version_min PRIVATE macho9core)
target_link_libraries(rename_segment PRIVATE macho9core)
target_link_libraries(retag_swift_classes PRIVATE macho9core)

# The multi-call CLI (Task 4, docs/PROPOSAL.md "Verbs"): `verify`/`info`/`grow`
# delegate in-process to macho9core (including src/grow.h); `dylib`/`rpath`/`lc`
# delegate by running change_dylib (see cli/macho9.c's file header for why --
# its rewrite+renumbering logic is not yet a reusable function). Kept out of
# the ${tool}.c-named foreach above because its source lives in cli/. No root
# include path is needed: every header it includes now lives in src/, reached
# via macho9core's PUBLIC src include (below), same as every other tool.
add_executable(macho9 cli/macho9.c)
target_compile_options(macho9 PRIVATE -O2 -Wall)
target_link_libraries(macho9 PRIVATE macho9core)
mavericks_assert_binary_compatible(macho9)

install(TARGETS ${MACHO_TOOLS} macho9 RUNTIME DESTINATION bin)

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
# relative -- same reason change_dylib_test needs it.
add_executable(image_test tests/image_test.c)
target_compile_options(image_test PRIVATE -O2 -Wall)
target_link_libraries(image_test PRIVATE macho9core)
add_test(NAME image_test COMMAND image_test
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# Hermetic tests for src/trie.c's export-trie rebuild (Task 5 gap 1): decode,
# shift, re-serialize, all against hand-built and hand-computed trie byte
# buffers -- no fixture file needed, same reasoning as macho_grow_test.
add_executable(trie_test tests/trie_test.c)
target_compile_options(trie_test PRIVATE -O2 -Wall)
target_link_libraries(trie_test PRIVATE macho9core)
add_test(NAME trie_test COMMAND trie_test)

# Hermetic tests for src/linkedit.c's ml_bump_all (Task 2b): a synthetic
# image built by hand via mi_wrap, so no fixture file is needed -- same
# reasoning as trie_test.
add_executable(linkedit_test tests/linkedit_test.c)
target_compile_options(linkedit_test PRIVATE -O2 -Wall -Wextra)
target_link_libraries(linkedit_test PRIVATE macho9core)
add_test(NAME linkedit_test COMMAND linkedit_test)

# Builds real dylibs, rewrites a real executable, and RUNS it -- a wrong library
# ordinal shows up as a dyld failure rather than a silent mis-binding. Needs the
# source dir because it compiles change_dylib.c itself.
add_test(NAME change_dylib_test
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/change_dylib_test.sh"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

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
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/chained-fixups.sh" "$<TARGET_FILE_DIR:patch_macho>")
set_tests_properties(chained_fixups PROPERTIES SKIP_RETURN_CODE 77)

add_test(NAME characterize
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/characterize.sh" "$<TARGET_FILE_DIR:change_dylib>" check)

# macho9's own CLI: --capabilities plus each verb it implements. Builds its own
# fixtures (FIXTURE_FLAGS = -mmacosx-version-min=10.9, same reasoning as
# change_dylib_test.sh) rather than reading tests/fixture.macho, so no
# WORKING_DIRECTORY is needed -- everything it needs is the bindir, passed the
# same way chained_fixups passes patch_macho's.
add_test(NAME cli_test
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.sh" "$<TARGET_FILE_DIR:macho9>")

# Regression coverage for two heap-overflow crashes a code review found in
# add_version_min and retag_swift_classes after Task 1 converted them onto
# src/image.h -- mi_open validates load commands, not section file ranges or
# addresses derived from them, so a fixture can pass mi_open cleanly and
# still contain an out-of-bounds reference. See the script's own header for
# what each fixture is shaped to trigger.
add_test(NAME leaf_tool_crashes
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/leaf-tool-crashes.sh" "$<TARGET_FILE_DIR:add_version_min>")

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
