cmake_minimum_required(VERSION 3.16)

# These tools RUN on 10.9 -- they are the thing being ported, not a cross-built
# artifact -- so the deployment target and architecture are the product, not a
# packaging detail.
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.9 CACHE STRING "runs on 10.9")
set(CMAKE_OSX_ARCHITECTURES x86_64 CACHE STRING "Intel-only (10.9)")

# FIRST-PARTY versioning, following mavericks-magic-trackpad2 rather than the
# upstream-tracking siblings. macho-tools has no upstream: there is no
# UPSTREAM_VERSION for Renovate to bump, no `<upstream>-mavericks.N` suffix, and
# no gitignored VERSION written by the workflow -- because nothing external
# releases and triggers a rebuild. Single source of truth, bumped by hand, tag
# v<MAVERICKS_VERSION> to match.
set(MAVERICKS_VERSION "0.1.0")
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

install(TARGETS ${MACHO_TOOLS} RUNTIME DESTINATION bin)

enable_testing()

# Hermetic: the grow, every base-relative re-baser, mg_verify and mg_plausible,
# against synthetic images built in memory. -Wno-unused-function because the
# header exposes helpers this translation unit does not all use.
add_executable(macho_grow_test macho_grow_test.c)
target_compile_options(macho_grow_test PRIVATE -O2 -Wno-unused-function)
add_test(NAME macho_grow_test COMMAND macho_grow_test)

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
add_test(NAME characterize
  COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/characterize.sh" "$<TARGET_FILE_DIR:change_dylib>" check)
