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
