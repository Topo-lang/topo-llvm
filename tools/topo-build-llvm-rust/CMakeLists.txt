add_executable(topo-build-llvm-rust main.cpp)

# topo-core include path comes transitively via imported targets.

# toml++ as header-only (needed by RustDriver for Topo.toml parsing)
find_path(TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp)
target_include_directories(topo-build-llvm-rust PRIVATE ${TOMLPLUSPLUS_INCLUDE_DIR})

# TopoTransforms PUBLIC-propagates TopoBackend → TopoBuildLib;
# TopoRustDriver PUBLIC-propagates TopoPlatform / TopoBuildConfig.
target_link_libraries(topo-build-llvm-rust PRIVATE
    topo::lang-rust::TopoRustDriver
    TopoTransforms
)

target_compile_definitions(topo-build-llvm-rust PRIVATE
    TOPO_LLVM_BINDIR="${TOPO_LLVM_BINDIR}"
    TOPO_BUILD_LIBDIR_SDK="$<TARGET_FILE_DIR:topo-parallel>"
    TOPO_BUILD_LIBDIR_PLATFORM="$<TARGET_FILE_DIR:topo::core::TopoPlatform>"
)

topo_set_llvm_flags(topo-build-llvm-rust)

topo_apply_std_pch_llvm(topo-build-llvm-rust)
set_property(TARGET topo-build-llvm-rust PROPERTY JOB_POOL_LINK llvm_link)

install(TARGETS topo-build-llvm-rust
    EXPORT topo-llvmTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
