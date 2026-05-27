# topo-build-llvm-{cpp,rust,mixed} CLI input-trust unit tests
#
# Exercises each LLVM backend's per-value backendExtras validation by
# spawning the binary with hand-crafted request JSON and asserting the
# exit code + stderr label. Validation fires before any compile step
# is reached, so no LLVM IR / clang toolchain is invoked.

add_executable(topo-llvm-input-trust-tests
    BackendExtrasInputTrustTest.cpp
)
target_link_libraries(topo-llvm-input-trust-tests PRIVATE
    TopoPlatform
    nlohmann_json::nlohmann_json
    GTest::gtest_main
)
target_include_directories(topo-llvm-input-trust-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/topo-core/include
)
target_compile_definitions(topo-llvm-input-trust-tests PRIVATE
    TOPO_BUILD_LLVM_CPP_EXE="$<TARGET_FILE:topo-build-llvm-cpp>"
    TOPO_BUILD_LLVM_RUST_EXE="$<TARGET_FILE:topo-build-llvm-rust>"
    TOPO_BUILD_LLVM_MIXED_EXE="$<TARGET_FILE:topo-build-llvm-mixed>"
)
add_dependencies(topo-llvm-input-trust-tests
    topo-build-llvm-cpp topo-build-llvm-rust topo-build-llvm-mixed)
topo_set_compiler_flags(topo-llvm-input-trust-tests)

gtest_discover_tests(topo-llvm-input-trust-tests
    PROPERTIES LABELS "toolchain;llvm" TIMEOUT 30)
