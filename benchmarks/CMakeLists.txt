# topo-llvm benchmarks — 41 benchmark projects.
#
# Gated by TOPO_LLVM_BUILD_BENCHMARKS at the top level. When ON, this
# subdirectory registers per-project pre-build stamps via
# topo-bench-artifacts, driving topo-build (imported from topo-cli) over
# each benchmarks/<project>/Topo.toml.
#
# The 41 projects fall into two shapes:
#   1. .topo-only fixtures (Topo.toml + .topo + src/). These need only
#      topo-build to produce variants; CMake does not configure them
#      directly. They get registered as bench artifacts.
#   2. Mixed CMake subprojects (13 of 41 ship a CMakeLists.txt for the
#      vanilla C++ build path). These are NOT add_subdirectory'd here
#      because each is `cmake_minimum_required(VERSION 3.20)
#      project(...)` — they configure as standalone projects when
#      tests need a vanilla baseline; the TopoBenchArtifactsDriver
#      drives that path via the `VANILLA_CPP` option.

# topo-cli ships the `topo-build` CLI we drive. find_package is QUIET +
# optional at the top-level gate; reaching this subdirectory means it
# was confirmed available.
if(NOT TARGET topo::cli::topo-build)
    message(FATAL_ERROR
        "topo-llvm benchmarks: topo::cli::topo-build not imported — "
        "ensure find_package(topo-cli CONFIG) at top level succeeded.")
endif()

# TOPO_BUILD_TARGET is set at top scope (CMakeLists.txt) when topo-cli is
# found; TopoBenchArtifacts.cmake reads it directly.
include(${CMAKE_SOURCE_DIR}/cmake/TopoBenchArtifacts.cmake)

# Excludes mirror the monorepo's lift list — orphan benchmark projects
# with no associated TEST_F (Topo.toml present, but no E2E case consumes
# them). Keeping the list in sync with the monorepo simplifies future
# alignment.
topo_register_all_bench_artifacts(
    "${CMAKE_CURRENT_SOURCE_DIR}"
    EXCLUDE
        lifetime_rust
        observability_rust
        parallel_rust
        pipeline_rust
        visibility_rust)

topo_finalize_bench_artifacts_target()

# Check-clean guard: every benchmark project's Topo.toml + .topo + host
# sources must pass topo-check as committed. Projects that opt out must say
# so explicitly via `check = "off"` in [build] (only mixed_lang today — no
# checker is registered for mixed); the skip below keys on that marker, so
# an opt-out is always visible in the project's own Topo.toml. Single label
# only — multi-label LABELS strings mis-parse (see test/e2e LABELS note).
#
# RESOURCE_LOCK pairs each guard with the e2e cases on the same project:
# those swap Topo.toml in place (TopoTomlSwap RAII, test/e2e/E2eHarness.cpp)
# and a concurrent topo-check read of a swapped file would check the wrong
# config (issue e2e-inplace-toml-swap-race-dirties-tree). The lock name must
# stay in lockstep with the PREFIX in test/e2e/CMakeLists.txt's
# topo_inject_e2e_project_locks calls (topo_e2e_llvm).
file(GLOB _topo_bench_tomls "${CMAKE_CURRENT_SOURCE_DIR}/*/Topo.toml")
foreach(_toml IN LISTS _topo_bench_tomls)
    file(READ "${_toml}" _toml_content)
    if(_toml_content MATCHES "check *= *\"off\"")
        continue()
    endif()
    get_filename_component(_proj_dir "${_toml}" DIRECTORY)
    get_filename_component(_proj "${_proj_dir}" NAME)
    add_test(NAME e2e.check_clean.${_proj}
        COMMAND $<TARGET_FILE:topo::cli::topo-check> --project "${_proj_dir}")
    set_tests_properties(e2e.check_clean.${_proj} PROPERTIES
        LABELS "e2e"
        RESOURCE_LOCK "topo_e2e_llvm_${_proj}")
endforeach()
unset(_toml_content)
