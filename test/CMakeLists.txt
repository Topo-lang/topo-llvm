# topo-llvm tests (standalone scope).
#
# This file builds the IR-pass unit tests and the LLVM-touching
# integration tests. The e2e/ suite (functional / equivalence / perf /
# pass-bench / decompile) is intentionally not added — it depends on the
# `topo-build` CLI binary (owned by topo-cli) and on per-project test
# resource locks that live in the monorepo. e2e is documented as
# deferred in .aidesk/live/40-issue/split-topo-llvm-tools-benchmarks-deferred.md
# alongside the tools + benchmarks gates.

# topo-core's Config.cmake exposes TOPO_CORE_FIXTURES_DIR via its
# install set (D4). Tests read .topo source files relative to it.
if(NOT DEFINED TOPO_CORE_FIXTURES_DIR)
    message(WARNING "TOPO_CORE_FIXTURES_DIR not set — topo-llvm tests reading demo.topo will fail at runtime.")
endif()

# ── Unit tests: IR pass + lifter unit coverage ────────────
add_executable(topo-llvm-unit-tests
    unit/TopoParallelPassTest.cpp
    unit/AdaptiveParallelTest.cpp
    unit/AdaptiveDispatchPassTest.cpp
    unit/IREmbedTest.cpp
    unit/DataLayoutPassTest.cpp
    unit/LayoutBenchmarkTest.cpp
    unit/LayoutCostModelTest.cpp
    unit/IndirectionPassTest.cpp
    unit/LoopParallelizePassTest.cpp
    unit/ObservabilityPassTest.cpp
    unit/LLVMTransformBackendTest.cpp
    unit/SymbolMappingRebindTest.cpp
    unit/TopoProfHintsTest.cpp
    unit/PrefetchPassTest.cpp
    unit/LLVMLifterL2Test.cpp
    unit/PipelineCodeGenPassTest.cpp
    unit/ReturnSpecializationPassTest.cpp
    unit/TopoInlinePassTest.cpp
    unit/ContainmentInterceptionPassTest.cpp
    unit/TopoFlattenPassTest.cpp
    unit/TopoReorderPassTest.cpp
    unit/TopoLayoutPassTest.cpp
    unit/SymbolObfuscatorTest.cpp
    unit/PassReportsSidecarSchemaTest.cpp
    unit/RuntimeAbiCheckInjectionTest.cpp
)
target_link_libraries(topo-llvm-unit-tests PRIVATE
    TopoTransforms TopoDecompile
    nlohmann_json::nlohmann_json
    GTest::gtest_main
)
target_compile_definitions(topo-llvm-unit-tests PRIVATE
    TOPO_TEST_FIXTURES_DIR="${TOPO_CORE_FIXTURES_DIR}"
)
topo_set_llvm_flags(topo-llvm-unit-tests)
topo_apply_std_pch_llvm(topo-llvm-unit-tests)
set_property(TARGET topo-llvm-unit-tests PROPERTY JOB_POOL_LINK llvm_link)
gtest_discover_tests(topo-llvm-unit-tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "toolchain;ir" TIMEOUT 120 RESOURCE_LOCK "llvm_heavy")

# ── Integration tests: Builder/Verifier/JIT/Adaptive ──────
add_executable(topo-llvm-integration-tests
    integration/BuilderPipelineTest.cpp
    integration/InternalVisibilityTest.cpp
    integration/VerifierTest.cpp
    integration/JITSpecializeTest.cpp
    integration/AdaptiveJITTest.cpp
)
target_link_libraries(topo-llvm-integration-tests PRIVATE
    TopoTransforms
    topo::core::TopoBuildConfig
    nlohmann_json::nlohmann_json
    GTest::gtest_main
)
target_compile_definitions(topo-llvm-integration-tests PRIVATE
    TOPO_TEST_FIXTURES_DIR="${TOPO_CORE_FIXTURES_DIR}"
)
topo_set_llvm_flags(topo-llvm-integration-tests)
topo_apply_std_pch_llvm(topo-llvm-integration-tests)
set_property(TARGET topo-llvm-integration-tests PROPERTY JOB_POOL_LINK llvm_link)
gtest_discover_tests(topo-llvm-integration-tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "toolchain;ir" TIMEOUT 120 RESOURCE_LOCK "llvm_heavy")

# JIT tests need the engine DLL alongside the test binaries — gated on
# TOPO_LLVM_BUILD_JIT_ENGINE so CI runs (with the shared lib OFF) skip
# the dependency cleanly.
if(TOPO_LLVM_BUILD_JIT_ENGINE)
    add_dependencies(topo-llvm-integration-tests topo-jit-engine)
    add_custom_command(TARGET topo-llvm-integration-tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:topo-jit-engine>
                $<TARGET_FILE_DIR:topo-llvm-integration-tests>/$<TARGET_FILE_NAME:topo-jit-engine>
        COMMENT "Copying topo-jit-engine alongside topo-llvm-integration-tests")
endif()
