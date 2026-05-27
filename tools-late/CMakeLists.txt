# topo-llvm/tools-late/ — embedded-mode late hook for tools + benchmarks
# blocks that reach into downstream packages (topo-lang-cpp /
# topo-lang-rust for tools, topo-cli for benchmarks).
#
# When topo-llvm's root CMakeLists is configured with
# TOPO_LLVM_DEFER_TOOLS=ON, it skips the tools/topo-build-llvm-{cpp,rust,
# mixed} and benchmarks/+test/e2e blocks. A parent project (the Topo
# meta-repo) add_subdirectory's THIS file after lang-cpp + lang-rust +
# topo-cli are in scope, so those blocks can resolve their reverse-edge
# dependencies.
#
# Standalone topo-llvm never enters this path — TOPO_LLVM_DEFER_TOOLS
# defaults OFF and the root's normal blocks run inline.
#
# Scope note: tools-late runs in the PARENT (meta) scope, not inside
# topo-llvm's project() scope. Variables set by topo-llvm root
# (LLVM_INCLUDE_DIRS, LLVM_DEFINITIONS, CMAKE_MODULE_PATH additions,
# helper functions like topo_set_llvm_flags / topo_llvm_libs) are not
# automatically visible here. We re-stage them: prepend topo-llvm's
# cmake/ to CMAKE_MODULE_PATH, include the compiler-flags helper and
# llvm-detect, and re-find_package(LLVM). IMPORTED targets created by
# find_package(LLVM) are global, but the LLVM_* variables are not — so
# they have to be re-populated in this scope before subdir CMakeLists
# can call topo_set_llvm_flags / topo_llvm_libs.

if(NOT TOPO_LLVM_BUILD_TOOLS AND NOT TOPO_LLVM_BUILD_BENCHMARKS)
    # Nothing was gated on; the late hook is a no-op.
    return()
endif()

# Re-stage topo-llvm's cmake/ helpers + LLVM detect into this scope.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../cmake")
include(TopoLLVMCompilerFlags)
include(llvm-detect)

# ── Tools: topo-build-llvm-{cpp,rust,mixed} ─────────────────
if(TOPO_LLVM_BUILD_TOOLS)
    if(NOT TARGET topo::lang-cpp::TopoCppPlugin)
        find_package(topo-lang-cpp CONFIG REQUIRED)
    endif()
    if(NOT TARGET topo::lang-rust::TopoRustPlugin)
        find_package(topo-lang-rust CONFIG REQUIRED)
    endif()

    # Place binary outputs under build/topo-llvm/tools/... mirroring the
    # standalone topo-llvm build layout — keeps scripts that expect the
    # canonical relative path (e.g. scripts/check-feature-taxonomy.sh)
    # working regardless of mode.
    add_subdirectory(
        ${CMAKE_CURRENT_LIST_DIR}/../tools/topo-build-llvm-cpp
        ${CMAKE_BINARY_DIR}/topo-llvm/tools/topo-build-llvm-cpp
    )
    add_subdirectory(
        ${CMAKE_CURRENT_LIST_DIR}/../tools/topo-build-llvm-rust
        ${CMAKE_BINARY_DIR}/topo-llvm/tools/topo-build-llvm-rust
    )
    add_subdirectory(
        ${CMAKE_CURRENT_LIST_DIR}/../tools/topo-build-llvm-mixed
        ${CMAKE_BINARY_DIR}/topo-llvm/tools/topo-build-llvm-mixed
    )
endif()

# ── Benchmarks + e2e ────────────────────────────────────────
if(TOPO_LLVM_BUILD_BENCHMARKS)
    if(NOT TARGET topo::cli::topo-build)
        find_package(topo-cli CONFIG QUIET)
    endif()
    if(NOT TARGET topo::cli::topo-build)
        message(WARNING
            "TOPO_LLVM_BUILD_BENCHMARKS=ON (deferred-mode) but topo-cli's "
            "topo-build target is not visible — benchmarks + e2e SKIPPED.")
        return()
    endif()
    if(NOT TOPO_LLVM_BUILD_TOOLS)
        message(WARNING
            "TOPO_LLVM_BUILD_BENCHMARKS=ON requires TOPO_LLVM_BUILD_TOOLS=ON "
            "(deferred-mode). Benchmarks + e2e SKIPPED.")
        return()
    endif()

    set(TOPO_BUILD_TARGET topo::cli::topo-build)
    message(STATUS "topo-llvm (deferred tools-late): benchmarks + e2e enabled")
    # Binary dirs mirror the standalone topo-llvm build layout so external
    # scripts (e.g. scripts/check-feature-taxonomy.sh) find the same
    # generated artifacts regardless of mode.
    add_subdirectory(
        ${CMAKE_CURRENT_LIST_DIR}/../benchmarks
        ${CMAKE_BINARY_DIR}/topo-llvm/benchmarks
    )
    if(TOPO_LLVM_BUILD_TESTS)
        add_subdirectory(
            ${CMAKE_CURRENT_LIST_DIR}/../test/e2e
            ${CMAKE_BINARY_DIR}/topo-llvm/test/e2e
        )
    endif()
endif()
