add_executable(topo-build-llvm-mixed main.cpp)

# topo-core include path comes transitively via imported targets.

# toml++ as header-only (needed by RustDriver for Cargo.toml parsing)
find_path(TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp)
target_include_directories(topo-build-llvm-mixed PRIVATE ${TOMLPLUSPLUS_INCLUDE_DIR})

# TopoTransforms PUBLIC-propagates TopoBackend → TopoSema → TopoParser;
# TopoCppDriver PUBLIC-propagates TopoPlatform / TopoBuildConfig / TopoBuildLib;
# TopoRustDriver brings TopoPlatform / TopoBuildConfig via its own PUBLIC chain.
target_link_libraries(topo-build-llvm-mixed PRIVATE
    topo::lang-cpp::TopoCppDriver
    topo::lang-rust::TopoRustDriver
    TopoTransforms
)

target_compile_definitions(topo-build-llvm-mixed PRIVATE
    TOPO_LLVM_BINDIR="${TOPO_LLVM_BINDIR}"
    TOPO_LANG_CPP_INCLUDE="$<TARGET_PROPERTY:topo::lang-cpp::topo-lang-cpp-headers,INTERFACE_INCLUDE_DIRECTORIES>"
    TOPO_LLVM_RUNTIME_INCLUDE="$<TARGET_PROPERTY:topo::llvm::topo-llvm-runtime-headers,INTERFACE_INCLUDE_DIRECTORIES>"
    TOPO_BUILD_LIBDIR_SDK="$<TARGET_FILE_DIR:topo-parallel>"
    TOPO_BUILD_LIBDIR_PLATFORM="$<TARGET_FILE_DIR:topo::core::TopoPlatform>"
)

topo_set_llvm_flags(topo-build-llvm-mixed)

topo_apply_std_pch_llvm(topo-build-llvm-mixed)
set_property(TARGET topo-build-llvm-mixed PROPERTY JOB_POOL_LINK llvm_link)

install(TARGETS topo-build-llvm-mixed
    EXPORT topo-llvmTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
