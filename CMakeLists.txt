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
add_library(macho9core STATIC src/uleb.c src/image.c)
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

# change_dylib is the one tool that includes macho_grow.h, which now includes
# src/uleb.h. The other five reach nothing in src/ yet.
target_link_libraries(change_dylib PRIVATE macho9core)

install(TARGETS ${MACHO_TOOLS} RUNTIME DESTINATION bin)

enable_testing()

# Hermetic: the grow, every base-relative re-baser, mg_verify and mg_plausible,
# against synthetic images built in memory. -Wno-unused-function because the
# header exposes helpers this translation unit does not all use.
add_executable(macho_grow_test macho_grow_test.c)
target_compile_options(macho_grow_test PRIVATE -O2 -Wno-unused-function)
target_link_libraries(macho_grow_test PRIVATE macho9core)
add_test(NAME macho_grow_test COMMAND macho_grow_test)

# Reads tests/fixture.macho, a real 10.9-built executable, so the walk is
# exercised against a binary a linker actually emitted rather than one this test
# invented. WORKING_DIRECTORY is the source dir because the fixture path is
# relative -- same reason change_dylib_test needs it.
add_executable(image_test tests/image_test.c)
target_compile_options(image_test PRIVATE -O2 -Wall)
target_link_libraries(image_test PRIVATE macho9core)
add_test(NAME image_test COMMAND image_test
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

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
