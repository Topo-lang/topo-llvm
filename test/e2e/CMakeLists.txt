# Shared E2E harness — compiled once as OBJECT library.
#
# topo-build comes from topo-cli (find_package'd at top level). The
# remaining inputs all live in topo-llvm itself: benchmark project tree,
# LLVM bindir, jit engine target dir, and TOPO_CORE_BUILD_FIXTURES_DIR
# exported by topo-core's package config.
if(NOT TARGET ${TOPO_BUILD_TARGET})
    message(FATAL_ERROR
        "topo-llvm e2e: TOPO_BUILD_TARGET=${TOPO_BUILD_TARGET} not defined — "
        "ensure find_package(topo-cli CONFIG) at top level succeeded.")
endif()

add_library(E2eHarnessLlvm OBJECT E2eHarness.cpp)
target_link_libraries(E2eHarnessLlvm PUBLIC topo::core::TopoPlatform GTest::gtest_main)
target_include_directories(E2eHarnessLlvm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
# Normalize the find_path cache result (top-level CMakeLists) to a clean empty
# string when topo-lang-cpp is not installed (meta build, where it is a sibling
# checkout), rather than the literal "<VAR>-NOTFOUND". Baked in below so the
# vanilla compile paths can add it as -I in the standalone/installed layout.
set(_TOPO_E2E_STDLIB_INC "")
if(TOPO_LANG_CPP_RUNTIME_INCLUDE)
    set(_TOPO_E2E_STDLIB_INC "${TOPO_LANG_CPP_RUNTIME_INCLUDE}")
endif()
target_compile_definitions(E2eHarnessLlvm PUBLIC
    TOPO_BUILD_EXE="$<TARGET_FILE:${TOPO_BUILD_TARGET}>"
    TOPO_BENCHMARKS_DIR="${TOPO_LLVM_BENCHMARKS_DIR}"
    TOPO_E2E_FIXTURES_DIR="${TOPO_CORE_BUILD_FIXTURES_DIR}"
    TOPO_LLVM_BINDIR="${TOPO_LLVM_BINDIR}"
    TOPO_JIT_ENGINE_DIR="$<TARGET_FILE_DIR:topo-jit-engine>"
    # Directory holding the topo runtime static archives (libtopo-parallel.a,
    # …). vanillaBuild() points `-L` here to link benchmarks that declare
    # [build].link_libs (currently only `pipeline` → topo-parallel). Baked in
    # so the link resolves regardless of whether topo-build is a build-tree
    # target (meta build) or the installed binary (standalone CI).
    TOPO_LLVM_RUNTIME_DIR="$<TARGET_FILE_DIR:topo-parallel>"
    # Installed topo-lang-cpp stdlib include dir (<prefix>/include holding
    # topo/parallel.h), from find_path in the top-level CMakeLists. The vanilla
    # compile paths add `-I` here so <topo/*.h> resolves in the standalone CI
    # layout, where the benchmark Topo.toml's meta-sibling include is absent.
    # Empty in the meta build → the harness's existence guard adds nothing.
    TOPO_LANG_CPP_RUNTIME_INCLUDE="${_TOPO_E2E_STDLIB_INC}"
    # Backend-tool dirs — passed as three separate single-path macros, joined
    # at runtime by the harness with the platform PATH separator and prepended
    # to PATH so the imported topo-build CLI resolves sibling backend tools
    # built in this repo. A single delimited macro is unsafe: a ';' inside a
    # compile definition is CMake's list separator, so it splits into multiple
    # -D flags and mangles the compile command on Windows (clang then sees the
    # source swallowed by the broken quoting → "no input files").
    TOPO_BUILD_LLVM_TOOLS_DIR_CPP="$<TARGET_FILE_DIR:topo-build-llvm-cpp>"
    TOPO_BUILD_LLVM_TOOLS_DIR_RUST="$<TARGET_FILE_DIR:topo-build-llvm-rust>"
    TOPO_BUILD_LLVM_TOOLS_DIR_MIXED="$<TARGET_FILE_DIR:topo-build-llvm-mixed>"
)
topo_set_compiler_flags(E2eHarnessLlvm)

# add_dependencies(target IMPORTED-target) is not legal in CMake — the
# imported topo-build executable cannot be a target-level dep. The bench
# binaries instead reference $<TARGET_FILE:${TOPO_BUILD_TARGET}> via the
# compile-time TOPO_BUILD_EXE define; the file dependency is implicit
# through the harness's source-tree fixture mutation.
set(_e2e_runtime_deps
    topo-build-llvm-mixed
    topo-parallel
    topo-jit
    topo-adaptive
    topo-arena
    topo-observe
    topo-containment)

# --- B1: Functional ---
add_executable(topo-e2e-functional FunctionalTests.cpp)
target_link_libraries(topo-e2e-functional PRIVATE E2eHarnessLlvm)
add_dependencies(topo-e2e-functional ${_e2e_runtime_deps})

# --- B2: Equivalence ---
add_executable(topo-e2e-equivalence EquivalenceTests.cpp)
target_link_libraries(topo-e2e-equivalence PRIVATE E2eHarnessLlvm)
# Equivalence tests cover passes whose benchmarks link against
# topo runtime libs (parallel, adaptive, arena, observe). Runtime libs must
# exist before the test runs or the topo-build link step fails.
add_dependencies(topo-e2e-equivalence ${_e2e_runtime_deps})

# --- B3: Performance ---
add_executable(topo-e2e-perf PerfBaselineTests.cpp)
target_link_libraries(topo-e2e-perf PRIVATE E2eHarnessLlvm)

# --- B4: Per-Pass Benchmarks ---
add_executable(topo-e2e-pass-bench PassBenchmarkTests.cpp)
target_link_libraries(topo-e2e-pass-bench PRIVATE E2eHarnessLlvm)

# --- ThreadSanitizer backstop ---
#
# Builds the known-wrong tsan_race fixture (force-parallel + check=off over a
# racy declaration) with -fsanitize=thread injected, runs it, and asserts TSan
# trips on the forced variant while the serial variant runs clean. The fixture
# lives under THIS dir's fixtures/ (NOT benchmarks/), so the bench-artifacts
# glob in benchmarks/CMakeLists.txt never enumerates it and no equivalence /
# functional registry references it — it is exercised only by this binary.
add_executable(topo-e2e-tsan TsanBackstopTests.cpp)
target_link_libraries(topo-e2e-tsan PRIVATE E2eHarnessLlvm)
# Needs the cpp backend tool (the build dispatches to it) plus the parallel +
# pass-event runtime archives the forced build auto-links. topo-build itself is
# the imported topo-cli target (file-dep flows via the TOPO_BUILD_EXE define).
add_dependencies(topo-e2e-tsan topo-build-llvm-cpp topo-parallel topo-pass-event)
target_compile_definitions(topo-e2e-tsan PRIVATE
    TOPO_E2E_TSAN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tsan_race")

# --- [build.cpp].flags passthrough ---
#
# Builds the cpp_flags fixture (a plain-cpp project whose source #errors
# without -DTOPO_E2E_FLAGS_PROOF) and asserts the Topo.toml flags array
# reaches both the compile-to-IR and the link clang invocation. Like
# tsan_race, the fixture lives under THIS dir's fixtures/ so the
# bench-artifacts glob never enumerates it.
add_executable(topo-e2e-cpp-flags CppFlagsTests.cpp)
target_link_libraries(topo-e2e-cpp-flags PRIVATE E2eHarnessLlvm)
add_dependencies(topo-e2e-cpp-flags topo-build-llvm-cpp)
target_compile_definitions(topo-e2e-cpp-flags PRIVATE
    TOPO_E2E_CPP_FLAGS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cpp_flags")

# --- topo-bench-artifacts (real pre-build) ---
#
# Register every benchmark project under topo-llvm/benchmarks/ for
# pre-build. Each project produces a stamp file under
# build/artifacts/<project>.stamp; the driver script
# (cmake/TopoBenchArtifactsDriver.cmake) runs vanilla + topo base/auto/forced
# sequentially, so inter-variant races inside a project are impossible by
# construction. Cross-project parallelism is delegated to CMake — multiple
# stamps build concurrently under `cmake --build . --target topo-bench-artifacts`.
#
# VANILLA_CPP: enabled for LLVM benchmarks. Not every project builds under
# plain clang++ (topo runtime headers pull in topo/tuple, parallel, arena...)
# — the driver's vanilla step fails loudly in those cases. The E2E harness's
# vanillaBuild() helper has the same limitation (some benchmark cases pass
# "" as vanillaOutput to signal "no vanilla baseline"). Projects without a
# vanilla baseline will report a build failure from the driver; those
# projects need their VANILLA_CPP flag dropped or the driver's vanilla step
# skipped. See the per-TODO below.
# Bench artifact registration moved to root CMakeLists.txt — see comment there.
# `add_custom_command(OUTPUT …)` rules declared in a subdirectory scope aren't
# visible to the aggregate target declared in a sibling scope, so the topo-llvm
# registrations live in the root scope alongside topo-jvm's.

# --- CTest fixture provider for `topo_bench_artifacts` (standalone) ---
# CTest treats a fixture with no FIXTURES_SETUP test as legal and SILENTLY
# runs FIXTURES_REQUIRED tests without it (the harness then falls back to
# slow inline per-case builds). Meta-only contract: in the meta union,
# topo-jvm/test/e2e/CMakeLists.txt registers the single shared provider
# (configured before this file — meta adds topo-jvm before
# topo-llvm/tools-late), so this block is skipped there via
# TOPO_META_DRIVING_TESTS to keep exactly one provider. Standalone
# topo-llvm self-provides: benchmarks/ (configured just before this dir in
# both layouts) already created the `topo-bench-artifacts` aggregate.
# Caveat: a partial meta (TOPO_META_ALLOW_PARTIAL=ON, no topo-jvm clone)
# with LLVM benchmarks ON would again lack a provider — local-only, exotic.
if(NOT TOPO_META_DRIVING_TESTS)
    add_test(NAME topo_bench_artifacts_build
        COMMAND ${CMAKE_COMMAND}
                --build ${CMAKE_BINARY_DIR}
                --target topo-bench-artifacts)
    set_tests_properties(topo_bench_artifacts_build PROPERTIES
        FIXTURES_SETUP "topo_bench_artifacts"
        TIMEOUT 3600
        LABELS "bench_fixture")
endif()

# --- B5: Decompile Round-trip ---
add_executable(topo-e2e-decompile DecompileTests.cpp)
target_link_libraries(topo-e2e-decompile PRIVATE E2eHarnessLlvm TopoDecompile)
# topo-build comes from topo-cli (imported); no target-level add_dependencies
# on imported targets — file-level dep flows via TOPO_BUILD_EXE define.
topo_set_llvm_flags(topo-e2e-decompile)
topo_apply_std_pch_llvm(topo-e2e-decompile)
set_property(TARGET topo-e2e-decompile PROPERTY JOB_POOL_LINK llvm_link)

# E2E binaries: split functional + equivalence into per-TEST_F CTest
# entries via gtest_discover_tests, then inject per-project RESOURCE_LOCK
# entries so cases on different benchmarks/<project>/ trees can run in
# parallel while same-project cases still serialise (the Topo.toml swap
# + .topo-cache mutation are per-project, not global).
#
# perf + decompile remain monolithic for now: perf is excluded from CI
# default (label "perf") and decompile is short (~6s); both still share
# the global topo_e2e_build lock with everyone except split entries.
include(GoogleTest)
# Resolve the shared e2e cmake helper from THIS package's own cmake/ dir.
# Use PROJECT_SOURCE_DIR (the topo-llvm project root — topo-llvm calls
# project()), NOT CMAKE_SOURCE_DIR: when topo-llvm is configured as a
# subdirectory of a larger super-project build, CMAKE_SOURCE_DIR is the
# super-project root and would resolve a different copy of the helper than a
# standalone build does — a silent, build-mode-dependent swap of which file
# backs the suite. PROJECT_SOURCE_DIR backs it with this package's own
# topo-llvm/cmake/TopoE2eProjectLocks.cmake in both layouts.
include(${PROJECT_SOURCE_DIR}/cmake/TopoE2eProjectLocks.cmake)

# The LABELS lists here are mis-parsed by CTest: gtest_discover_tests writes
# them UNQUOTED into the generated *_tests.cmake (`LABELS e2e functional`),
# which CTest reads as LABELS=e2e plus a bogus functional=<next> property — so
# `ctest -L functional` / `-L equivalence` found 0. Each project-locks injector
# below is given the same LABELS and re-emits them bracket-quoted (the only form
# CTest parses as a list), which is what actually registers the sub-labels.
gtest_discover_tests(topo-e2e-functional DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "e2e\;functional"
        TIMEOUT 180
)
topo_inject_e2e_project_locks(
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/FunctionalTests.cpp"
    FIXTURE Functional
    PREFIX topo_e2e_llvm
    LABELS "e2e;functional"
    BUILD_CALLS topoBuild topoBaseBuild topoForcedBuild vanillaBuild
)

gtest_discover_tests(topo-e2e-equivalence DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "e2e\;equivalence"
        TIMEOUT 300
)
topo_inject_e2e_project_locks(
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/EquivalenceTests.cpp"
    FIXTURE Equivalence
    PREFIX topo_e2e_llvm
    LABELS "e2e;equivalence"
    BUILD_CALLS topoBuild topoBaseBuild topoForcedBuild vanillaBuild
                assertPassFired assertPassNotFired
    ASSERT_CALLS assertBaseEquivalence assertForcedEquivalence assertDefaultEquivalence
                 assertBaseVsDefaultEquivalence assertBaseVsForcedEquivalence
)

add_test(NAME e2e.perf      COMMAND topo-e2e-perf)
add_test(NAME e2e.decompile COMMAND topo-e2e-decompile)
set_tests_properties(e2e.perf e2e.decompile PROPERTIES
    RESOURCE_LOCK "topo_e2e_build"
    TIMEOUT 300)
set_tests_properties(e2e.perf      PROPERTIES LABELS "e2e;perf")
set_tests_properties(e2e.decompile PROPERTIES LABELS "e2e;functional")

# ThreadSanitizer backstop — discovered per-TEST_F. Each case builds + runs in a
# private temp copy of the fixture, so no per-project RESOURCE_LOCK is needed for
# correctness; it shares the global "topo_e2e_build" lock with perf/decompile so
# the TSan build does not contend for cores with the other inline-building e2e
# suites under `ctest -j N` (CPU starvation could let the runtime's work-helping
# await drain both tasks on one thread and mask the race). Labelled "e2e" so the
# benchmark+e2e job's `ctest -L e2e --label-exclude "perf|benchmark"` runs it;
# "perf"/"benchmark" are deliberately absent so it is never treated as a timing
# bench. A single-element extra label avoids the multi-label LABELS mis-parse
# documented for gtest_discover_tests above.
include(GoogleTest)
gtest_discover_tests(topo-e2e-tsan DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "e2e"
        RESOURCE_LOCK "topo_e2e_build"
        TIMEOUT 300
)

# [build.cpp].flags passthrough — each case builds a one-TU fixture in a
# private temp copy, so no per-project or global lock is needed. Single
# "e2e" label (see the LABELS mis-parse note above) puts it in the
# benchmark+e2e job's `ctest -L e2e --label-exclude "perf|benchmark"` gate.
gtest_discover_tests(topo-e2e-cpp-flags DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "e2e"
        TIMEOUT 180
)

# --- per-case benchmark discovery ---
#
# Each CATEGORY_BENCH_TEST_F expands to a gtest class named
# `<Suite>_<Category>_<Name>.Run`. gtest_discover_tests creates one CTest
# case per gtest class; the post-process script below (invoked via the
# directory-level TEST_INCLUDE_FILES property) parses each test name and
# appends a lowercase `<category>` label to its LABELS property.
#
# The base labels `e2e;perf;benchmark` are set on every benchmark case so
# existing `scripts/test.sh perf` filtering continues to work unchanged.
#
# Split-responsibility locking:
#   - Pre-build: CMake-level cross-project parallelism via the
#     `topo_bench_artifacts` setup fixture. Provider split: standalone
#     registers the in-repo provider above (bench-artifacts section); in the
#     meta union topo-jvm/test/e2e registers the single shared provider and
#     the in-repo one is skipped via TOPO_META_DRIVING_TESTS.
#   - Measurement: CTest-level serialisation via `topo_bench_measure` lock.
#     Benchmarks time CPU-bound subprocess runs; `ctest -j N` without a
#     measurement lock lets concurrent bench binaries contend for cores and
#     pollutes timing.
# `FIXTURES_REQUIRED` and `RESOURCE_LOCK` are independent CTest properties, so
# combining them gives "run fixture in parallel, then serialise measurement."
# The `llvm_heavy` label-based serialisation is preserved only on the
# legacy single-binary e2e tests above (e2e.functional / e2e.equivalence /
# e2e.decompile), which still do builds inline.
include(GoogleTest)
gtest_discover_tests(topo-e2e-pass-bench DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "e2e\;perf\;benchmark"
        FIXTURES_REQUIRED "topo_bench_artifacts"
        RESOURCE_LOCK "topo_bench_measure"
        TIMEOUT 300
    DISCOVERY_MODE POST_BUILD
)

# Append per-case category labels (opt / covered / instrument / enhance /
# runtime). Parses CATEGORY_BENCH_TEST_F(...) invocations at configure time
# and writes a CMake fragment that CTest loads after gtest_discover_tests.
# PROJECT_SOURCE_DIR (this package's own cmake/ copy), not CMAKE_SOURCE_DIR —
# see the TopoE2eProjectLocks include above for why mode-independent resolution
# matters when topo-llvm is configured inside a larger super-project build.
include(${PROJECT_SOURCE_DIR}/cmake/TopoBenchCategoryLabels.cmake)
topo_inject_bench_category_labels(${CMAKE_CURRENT_SOURCE_DIR}/PassBenchmarkTests.cpp)

# ClassTemplate micros expand via TOPO_CLASS_TEMPLATE_MICRO — the
# regex-driven parser above sees the macro definition but not the 6
# invocations, so register those explicitly.
set(_topo_ct_micros
    ClassTemplate_Types_Friendly ClassTemplate_Types_Unfriendly
    ClassTemplate_Templates_Friendly ClassTemplate_Templates_Unfriendly
    ClassTemplate_CRTP_Friendly ClassTemplate_CRTP_Unfriendly
    ClassTemplate_Constraint_Friendly ClassTemplate_Constraint_Unfriendly
    ClassTemplate_Comptime_Friendly ClassTemplate_Comptime_Unfriendly
    ClassTemplate_Variadic_Friendly ClassTemplate_Variadic_Unfriendly)
topo_inject_bench_category_labels_explicit(ENHANCE PassBench _topo_ct_micros)
