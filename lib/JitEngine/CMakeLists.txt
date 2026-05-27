# topo-jit-engine: LLVM-heavy JIT engine (shared library).
#
# `orcjit` and `nativeasmparser` are unique to this target.
# `core / support / bitreader / irreader / demangle / native (codegen)`
# are all already pulled in by TopoBackend
# (`topo-llvm/lib/Backend/CMakeLists.txt`) and propagate to us via
# TopoTransforms's PUBLIC link to TopoBackend. Listing them again here
# produced the `ld: warning: ignoring duplicate libraries` block at link
# time for libtopo-jit-engine.dylib (9 LLVM static archives duplicated).
#
# `nativeasmparser` resolves the symbol the inline
# `InitializeNativeTargetAsmParser()` macro expands to per host arch
# (`LLVMInitializeAArch64AsmParser` on arm64, `LLVMInitializeX86AsmParser`
# on x86_64). `nativecodegen` (in TopoBackend) only covers the AsmPrinter
# side — it does NOT include the parser archive.
#
# Some LLVM builds (e.g. the bundled 22.1.1 tarball seen on GHA Linux +
# Windows runners) don't expand the `nativeasmparser` virtual component
# through llvm_map_components_to_libnames, leaving the literal
# `LLVMnativeasmparser` name in the link line. Use LLVM_NATIVE_ARCH
# (set by LLVMConfig.cmake; values like X86, AArch64, ARM) to pin the
# host AsmParser archive explicitly.
set(_native_asm_parser "${LLVM_NATIVE_ARCH}AsmParser")
topo_llvm_libs(JIT_LLVM_LIBS orcjit ${_native_asm_parser})

add_library(topo-jit-engine SHARED
    topo_jit_engine.cpp
)

target_include_directories(topo-jit-engine PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/include
    ${CMAKE_SOURCE_DIR}/include
)
if(TOPO_LANG_CPP_RUNTIME_INCLUDE)
    target_include_directories(topo-jit-engine PRIVATE ${TOPO_LANG_CPP_RUNTIME_INCLUDE})
endif()

target_link_libraries(topo-jit-engine PRIVATE
    ${JIT_LLVM_LIBS} nlohmann_json::nlohmann_json
    TopoTransforms TopoBackend
    # Explicit links to the topo-core libs whose symbols topo-jit-engine
    # uses directly (topo::SymbolTable::addLogicBlock from TopoSema,
    # topo::analysis::groupNodesByStage / resolveNodeCallee / isSourceNode
    # / findUpstreamNodes / analyzePriority from TopoAnalysis).
    # Without these, Linux ld accidentally resolves through TopoBackend's
    # static archive chain, but Windows lld-link rejects the missing
    # symbols when building topo-jit-engine.dll.
    topo::core::TopoSema topo::core::TopoAnalysis topo::core::TopoAST
)

topo_set_llvm_flags(topo-jit-engine)
topo_apply_std_pch_llvm(topo-jit-engine)
set_property(TARGET topo-jit-engine PROPERTY JOB_POOL_LINK llvm_link)

# Place engine DLL alongside test executables so it can be found at test time.
set_target_properties(topo-jit-engine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test"
)

install(TARGETS topo-jit-engine
    EXPORT topo-llvmTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
