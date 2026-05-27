cmake_minimum_required(VERSION 3.20)

# ── Bundled LLVM compiler (before project()) ─────────────
# If user hasn't overridden CMAKE_CXX_COMPILER, prefer the bundled
# clang++ shipped under llvm-dev/. Monorepo and standalone mode both
# resolve to ${CMAKE_CURRENT_SOURCE_DIR}/llvm-dev/bin/clang++.
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED CACHE{CMAKE_CXX_COMPILER})
    foreach(_EXT "" ".exe")
        set(_CLANGXX "${CMAKE_CURRENT_SOURCE_DIR}/llvm-dev/bin/clang++${_EXT}")
        if(EXISTS "${_CLANGXX}")
            set(CMAKE_CXX_COMPILER "${_CLANGXX}" CACHE FILEPATH "Bundled clang++")
            set(CMAKE_C_COMPILER
                "${CMAKE_CURRENT_SOURCE_DIR}/llvm-dev/bin/clang${_EXT}"
                CACHE FILEPATH "Bundled clang")
            message(STATUS "topo-llvm: using bundled compiler from llvm-dev/bin/")
            break()
        endif()
    endforeach()
endif()

project(topo-llvm
    VERSION 0.1.0
    DESCRIPTION "Topo LLVM backend — IR passes, C-ABI runtime, build tools, JIT engine, decompile"
    HOMEPAGE_URL "https://github.com/topo-lang/topo-llvm"
    LANGUAGES C CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(TopoLLVMCompilerFlags)

# Limit parallel LLVM-heavy link jobs to avoid OOM on constrained devices.
set_property(GLOBAL PROPERTY JOB_POOLS llvm_link=2)

# Compatibility: subdir CMakeLists call `if(TOPO_ENABLE_LLVM) ...` to
# gate LLVM-dependent code paths (the monorepo can disable the whole
# backend via this knob for sanitizer presets). In standalone topo-llvm
# the LLVM backend is the entire raison d'être, so it is always ON.
set(TOPO_ENABLE_LLVM ON)

# ── Bundled LLVM detect ──────────────────────────────────
# Discovery order: cached LLVM_DIR / Homebrew llvm@<major> / bundled tarball
# under llvm-dev/. Sets LLVM_DIR + TOPO_LLVM_BINDIR and runs
# find_package(LLVM REQUIRED CONFIG).
include(llvm-detect)

# ── Optional system PCH host shim (no-op in standalone) ──
# Subdir CMakeLists call topo_apply_std_pch_llvm(target) for monorepo PCH
# sharing. In standalone we skip PCH (each target compiles its own headers);
# the helper in TopoLLVMCompilerFlags.cmake is already a guarded no-op.

# ── Upstream Topo packages ──────────────────────────────
find_package(topo-core CONFIG REQUIRED)

# ── External dependencies ───────────────────────────────
find_package(nlohmann_json CONFIG REQUIRED)

# topo-lang-cpp's user-facing runtime headers (<topo/parallel.h>,
# <topo/jit.h>, etc.) are #include'd by the C-ABI runtime libs and by
# topo-jit-engine for C++-side type definitions. When topo-lang-cpp is
# installed under the same prefix as topo-llvm, find_path locates them.
# Optional — the runtime libs gracefully fall through if not present
# (a small subset of TUs need only the C ABI headers).
find_path(TOPO_LANG_CPP_RUNTIME_INCLUDE topo/parallel.h
    HINTS ${CMAKE_PREFIX_PATH})
if(TOPO_LANG_CPP_RUNTIME_INCLUDE)
    message(STATUS "topo-llvm: topo-lang-cpp runtime headers found at ${TOPO_LANG_CPP_RUNTIME_INCLUDE}")
endif()

# ── Headers (always installed) ──────────────────────────
# topo-llvm exposes its own public headers under include/topo/Backend,
# include/topo/Transforms, include/topo/Decompile. Make them available
# both at build time and after install.
add_library(topo-llvm-headers INTERFACE)
target_include_directories(topo-llvm-headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
install(TARGETS topo-llvm-headers EXPORT topo-llvmTargets)
install(DIRECTORY include/topo
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

# Runtime headers (parallel_rt.h, adaptive_rt.h, ...) install under
# include/topo/rt — these are the headers user code includes via
# <topo/rt/parallel_rt.h>, matching the monorepo layout.
add_library(topo-llvm-runtime-headers INTERFACE)
target_include_directories(topo-llvm-runtime-headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
install(TARGETS topo-llvm-runtime-headers EXPORT topo-llvmTargets)
install(DIRECTORY runtime/include/topo
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

# ── Options ─────────────────────────────────────────────
# C-ABI runtime + IR passes + JIT engine + decompile + LLVM-TTI half of
# topo-prof are always built; they constitute the standalone scope of
# this lift.
option(TOPO_LLVM_BUILD_TESTS "Build unit + integration tests" ON)

# topo-build-llvm-{cpp,rust,mixed} backend tools reverse-link
# topo-lang-cpp's TopoCppDriver / topo-lang-rust's TopoRustDriver. Those
# upstream packages currently ship their LLVM-bound layer gated OFF
# (TOPO_LANG_CPP_ENABLE_LLVM / TOPO_LANG_RUST_ENABLE_LLVM); flipping
# those gates ON in a fresh re-install is the chicken-and-egg unblock.
# topo-prof depends only on topo::core::TopoProfile + LLVM TTI libs, but
# is grouped here for symmetry.
option(TOPO_LLVM_BUILD_TOOLS
    "Build topo-build-llvm-{cpp,rust,mixed} (needs topo-lang-cpp + topo-lang-rust with their LLVM gate ON)"
    OFF)
option(TOPO_LLVM_BUILD_PROF
    "Build topo-prof (LLVM TTI cost-modeling CLI)"
    OFF)
option(TOPO_LLVM_BUILD_BENCHMARKS
    "Build the 41 benchmark projects (each is its own CMake subproject)"
    OFF)
# topo-jit-engine is a SHARED library loaded at user-program runtime
# (libtopo-jit-engine.{so,dylib,dll}). It transitively links the
# bundled LLVM static archives + topo::core::TopoSema/TopoAnalysis/
# TopoAST. Linux GHA runners ship libzstd.a built without -fPIC, so
# linking it into a .so fails; Windows lld-link can't resolve some
# imported-target paths through CMake's response-file mode. Both are
# CI-environment quirks; local builds work. Default ON; CI can flip
# OFF to skip the shared lib without losing IR-pass / backend / decompile.
option(TOPO_LLVM_BUILD_JIT_ENGINE
    "Build libtopo-jit-engine.{so,dylib,dll} (SHARED; needed for runtime JIT use)"
    ON)

if(TOPO_LLVM_BUILD_TOOLS)
    find_package(topo-lang-cpp CONFIG REQUIRED)
    find_package(topo-lang-rust CONFIG REQUIRED)
endif()

# Benchmarks + e2e suite consume topo-build from topo-cli. find_package
# runs early at top scope so the imported target is globally visible to
# the benchmark subdirectory's add_custom_command DEPENDS list.
if(TOPO_LLVM_BUILD_BENCHMARKS)
    find_package(topo-cli CONFIG QUIET)
    if(topo-cli_FOUND)
        # Both benchmarks/ and test/e2e/ reference this. Set once at top
        # scope so the bench-artifacts helper and the e2e CMakeLists agree.
        set(TOPO_BUILD_TARGET topo::cli::topo-build)
    endif()
endif()

# Benchmark project tree path — exposed as a cache variable so cross-
# folder consumers (e.g. topo-jvm/test/e2e equivalence benchmarks) can
# locate it the same way regardless of in-tree vs find_package'd
# consumption.
set(TOPO_LLVM_BENCHMARKS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
    CACHE PATH "Path to topo-llvm's benchmark project tree.")

# ── Components ──────────────────────────────────────────
# C-ABI runtime (zero-LLVM, always built — passes target these symbols).
add_subdirectory(runtime)

# IR pass + backend lib + decompile + JIT engine + LLVM TTI provider.
add_subdirectory(lib/Backend)
add_subdirectory(lib/Transforms)
add_subdirectory(lib/Decompile)
if(TOPO_LLVM_BUILD_JIT_ENGINE)
    add_subdirectory(lib/JitEngine)
endif()
add_subdirectory(lib/Profile)

# topo-build-llvm-{cpp,rust,mixed} backend tools (gated, see option above).
if(TOPO_LLVM_BUILD_TOOLS)
    add_subdirectory(tools/topo-build-llvm-cpp)
    add_subdirectory(tools/topo-build-llvm-rust)
    add_subdirectory(tools/topo-build-llvm-mixed)
endif()

# topo-prof (gated).
if(TOPO_LLVM_BUILD_PROF)
    add_subdirectory(tools/topo-prof)
endif()

# Gate-3 wiring (benchmarks + e2e) is deferred until after the test block
# below so that enable_testing() + GTest fetch have already run when the
# e2e subdirectory's add_executable / gtest_discover_tests fire.

# ── Install / export ────────────────────────────────────
install(EXPORT topo-llvmTargets
    FILE topo-llvmTargets.cmake
    NAMESPACE topo::llvm::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-llvm
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/topo-llvmConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/topo-llvmConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-llvm
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/topo-llvmConfigVersion.cmake
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/topo-llvmConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/topo-llvmConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-llvm
)

# ── Tests ───────────────────────────────────────────────
if(TOPO_LLVM_BUILD_TESTS AND PROJECT_IS_TOP_LEVEL)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
    include(GoogleTest)

    # Runtime contract tests — zero-LLVM, always-buildable in standalone.
    add_subdirectory(runtime/test)
    # IR-pass + integration + input-trust tests.
    add_subdirectory(test)
endif()

# 41 benchmark projects + E2E suite (gated).
#
# Benchmarks pre-build via topo-build (imported from topo-cli) and feed the
# e2e suite (functional / equivalence / perf / pass-bench / decompile).
# Both require topo-cli on CMAKE_PREFIX_PATH. When topo-cli is not present,
# the gate prints a clear skip message and leaves benchmarks + e2e out.
#
# The e2e binaries also require gate-1 (topo-build-llvm-mixed) — they shell
# out to the LLVM backend tool during artifact builds. Sequenced AFTER the
# test block so enable_testing() + GTest are visible to the e2e subdirectory.
if(TOPO_LLVM_BUILD_BENCHMARKS)
    if(NOT topo-cli_FOUND)
        message(WARNING
            "TOPO_LLVM_BUILD_BENCHMARKS=ON but topo-cli not found on "
            "CMAKE_PREFIX_PATH — benchmarks + e2e suite SKIPPED. "
            "Install topo-cli (provides topo-build) and re-configure.")
    elseif(NOT TOPO_LLVM_BUILD_TOOLS)
        message(WARNING
            "TOPO_LLVM_BUILD_BENCHMARKS=ON requires TOPO_LLVM_BUILD_TOOLS=ON "
            "(e2e suite links against topo-build-llvm-mixed). "
            "Benchmarks + e2e suite SKIPPED.")
    else()
        message(STATUS "topo-llvm: TOPO_LLVM_BUILD_BENCHMARKS=ON — benchmarks + e2e enabled")
        add_subdirectory(benchmarks)
        if(TOPO_LLVM_BUILD_TESTS AND PROJECT_IS_TOP_LEVEL)
            add_subdirectory(test/e2e)
        endif()
    endif()
endif()
