add_executable(topo-build-llvm-cpp main.cpp)

# topo-core / topo-lang-cpp include paths come transitively via the
# imported targets' PUBLIC propagation; no manual -I needed.

# TopoTransforms PUBLIC-propagates TopoBackend → TopoSema → TopoParser;
# TopoCppDriver PUBLIC-propagates TopoPlatform / TopoBuildConfig / TopoBuildLib.
target_link_libraries(topo-build-llvm-cpp PRIVATE
    topo::lang-cpp::TopoCppDriver
    TopoTransforms
)

target_compile_definitions(topo-build-llvm-cpp PRIVATE
    TOPO_LLVM_BINDIR="${TOPO_LLVM_BINDIR}"
    TOPO_LANG_CPP_INCLUDE="$<TARGET_PROPERTY:topo::lang-cpp::topo-lang-cpp-headers,INTERFACE_INCLUDE_DIRECTORIES>"
    TOPO_LLVM_RUNTIME_INCLUDE="$<TARGET_PROPERTY:topo::llvm::topo-llvm-runtime-headers,INTERFACE_INCLUDE_DIRECTORIES>"
    TOPO_BUILD_LIBDIR_SDK="$<TARGET_FILE_DIR:topo-parallel>"
    TOPO_BUILD_LIBDIR_PLATFORM="$<TARGET_FILE_DIR:topo::core::TopoPlatform>"
)

topo_set_llvm_flags(topo-build-llvm-cpp)

topo_apply_std_pch_llvm(topo-build-llvm-cpp)
set_property(TARGET topo-build-llvm-cpp PROPERTY JOB_POOL_LINK llvm_link)

install(TARGETS topo-build-llvm-cpp
    EXPORT topo-llvmTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Relocatable install: rewrite the absolute libLLVM load command to @rpath so a
# copied binary finds libLLVM via @loader_path/../lib instead of the build host.
topo_relocate_llvm_rpath(topo-build-llvm-cpp libLLVM)
