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
    # Backend-tool dirs — colon-separated (semicolon on Windows). The harness
    # prepends every dir to PATH so the imported topo-build CLI resolves
    # sibling backend tools built in this repo.
    TOPO_BUILD_LLVM_TOOLS_DIRS="$<TARGET_FILE_DIR:topo-build-llvm-cpp>$<IF:$<PLATFORM_ID:Windows>,$<SEMICOLON>,:>$<TARGET_FILE_DIR:topo-build-llvm-rust>$<IF:$<PLATFORM_ID:Windows>,$<SEMICOLON>,:>$<TARGET_FILE_DIR:topo-build-llvm-mixed>"
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
include(${CMAKE_SOURCE_DIR}/cmake/TopoE2eProjectLocks.cmake)

# The LABELS lists here are mis-parsed by CTest: gtest_discover_tests writes
# them UNQUOTED into the generated *_tests.cmake (`LABELS e2e functional`),
# which CTest reads as LABELS=e2e plus a bogus functional=<next> property — so
# `ctest -L functional` / `-L equivalence` found 0. Each project-locks injector
# below is given the same LABELS and re-emits them bracket-quoted (the only form
# CTest parses as a list), which is what actually registers the sub-labels.
gtest_discover_tests(topo-e2e-functional DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS "e2e;functional"
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
        LABELS "e2e;equivalence"
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
#   - Pre-build: CMake-level cross-project parallelism via `topo_bench_artifacts`
#     setup fixture (defined in the JVM CMakeLists because both LLVM and JVM
#     share the fixture name). Keeps fc96660's 22min → 460s artifact-build win.
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
        LABELS "e2e;perf;benchmark"
        FIXTURES_REQUIRED "topo_bench_artifacts"
        RESOURCE_LOCK "topo_bench_measure"
        TIMEOUT 300
    DISCOVERY_MODE POST_BUILD
)

# Append per-case category labels (opt / covered / instrument / enhance /
# runtime). Parses CATEGORY_BENCH_TEST_F(...) invocations at configure time
# and writes a CMake fragment that CTest loads after gtest_discover_tests.
include(${CMAKE_SOURCE_DIR}/cmake/TopoBenchCategoryLabels.cmake)
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
