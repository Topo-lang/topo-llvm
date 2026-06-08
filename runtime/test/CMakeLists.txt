# Runtime library unit tests — zero LLVM
# Most sources are LLVM-independent. JITRealPathTest.cpp consumes a
# bitcode fixture that needs bundled LLVM clang at build time; it is
# only added when TOPO_ENABLE_LLVM=ON (the bitcode-include block below
# follows the same gate). Without this gate, configuring with
# TOPO_ENABLE_LLVM=OFF would FATAL_ERROR inside
# test_pipeline_bitcode.cmake.
set(_TOPO_RUNTIME_TEST_SOURCES
    unit/ParallelRuntimeTest.cpp
    unit/JITContextTest.cpp
    unit/JITApiStubTest.cpp
    unit/JITEdgeCaseTest.cpp
    unit/AdaptiveMonitorTest.cpp
    unit/ObserveRuntimeTest.cpp
    unit/ArenaRuntimeTest.cpp
    unit/CABIContractTest.cpp
)
if(TOPO_ENABLE_LLVM)
    list(APPEND _TOPO_RUNTIME_TEST_SOURCES unit/JITRealPathTest.cpp)
endif()
add_executable(topo-runtime-tests ${_TOPO_RUNTIME_TEST_SOURCES})
# NOTE: topo-adaptive transitively pulls topo-jit-api (which itself pulls
# topo-parallel), so listing those explicitly would duplicate them on the
# link line — Apple's ld emits "ignoring duplicate libraries" warnings.
# Keep this list minimal: include only libraries that are not transitively
# reachable from another entry.
target_link_libraries(topo-runtime-tests PRIVATE
    topo-adaptive topo-observe topo-arena topo-containment
    GTest::gtest_main
)
# topo-pass-event is transitively pulled by topo-adaptive; not listed
# explicitly to avoid Apple ld duplicate-library warnings (see NOTE above).
# topo-containment has its own translation unit (topo_containment.cpp)
# and is NOT transitively reachable, so it IS listed explicitly.
if(TOPO_LANG_CPP_RUNTIME_INCLUDE)
    target_include_directories(topo-runtime-tests PRIVATE ${TOPO_LANG_CPP_RUNTIME_INCLUDE})
endif()
topo_set_compiler_flags(topo-runtime-tests)

# JITRealPathTest consumes a pre-compiled bitcode blob generated at build
# time (see test_pipeline_bitcode.cmake). The generated header is placed
# under ${CMAKE_CURRENT_BINARY_DIR}/generated. The fixture invokes the
# bundled LLVM clang, so the include + dep edge only attach when
# TOPO_ENABLE_LLVM=ON (matching the JITRealPathTest source gate above).
if(TOPO_ENABLE_LLVM)
    include(test_pipeline_bitcode.cmake)
    target_include_directories(topo-runtime-tests PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    add_dependencies(topo-runtime-tests topo-test-pipeline-bitcode)
    # Lets in-file tests (e.g. AdaptiveMonitorTest's three force-specialize
    # cases) conditionally include the bitcode header and exercise the real
    # JIT path. Tests in the same .cpp that don't need the engine compile
    # unconditionally.
    target_compile_definitions(topo-runtime-tests PRIVATE TOPO_TEST_HAS_LLVM_BITCODE)
endif()

# The JIT engine DLL is built in ${CMAKE_BINARY_DIR}/test/ but the runtime
# test binary lives in ${CMAKE_CURRENT_BINARY_DIR}.  loadEngine() searches
# alongside the executable first, so copy the engine next to the test
# binary after both are built.  This unlocks the real JIT path for
# JITRealPathTest and makes available() return true in JITApiStubTest.
# topo-jit-engine is LLVM-dependent (lives under topo-llvm/lib/JitEngine,
# only added when TOPO_ENABLE_LLVM=ON), so the dep + copy are gated to
# match. With LLVM off, JITApiStubTest::available() simply returns false
# at runtime and the test SKIPs cleanly — preserving the
# "runtime-tests-always-buildable" contract for sanitizer presets.
if(TOPO_ENABLE_LLVM AND TOPO_LLVM_BUILD_JIT_ENGINE)
    add_dependencies(topo-runtime-tests topo-jit-engine)
    add_custom_command(TARGET topo-runtime-tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:topo-jit-engine>
                $<TARGET_FILE_DIR:topo-runtime-tests>/$<TARGET_FILE_NAME:topo-jit-engine>
        COMMENT "Copying topo-jit-engine alongside topo-runtime-tests")
endif()

gtest_discover_tests(topo-runtime-tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "toolchain\;runtime")

# Feature dimension labels
set_property(DIRECTORY APPEND PROPERTY
    TEST_INCLUDE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/FeatureMap.cmake")
