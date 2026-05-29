topo_llvm_libs(LLVM_LIBS
    analysis
    core
    irreader
    bitreader
    passes
    support
    demangle
    nativecodegen
    transformutils
    bitwriter
    linker
)

add_library(TopoBackend
    SymbolMapper.cpp
    VisibilityApplier.cpp
    PassPipeline.cpp
    PassReportsSidecar.cpp
    Verifier.cpp
    PostTransformVerifier.cpp
    IREmbed.cpp
    LLVMTransformBackend.cpp
    LayoutBenchmark.cpp
    LayoutCostModel.cpp
    VariantBenchmark.cpp
)
add_library(topo::llvm::TopoBackend ALIAS TopoBackend)

target_include_directories(TopoBackend PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(TopoBackend
    PUBLIC  topo::core::TopoAST topo::core::TopoSema topo::core::TopoAnalysis
    # TopoBuildLib is PUBLIC so downstream consumers linking TopoBackend
    # transitively get topo::categoryOf + topo::toString used by
    # PassPipeline's report header stamping.
    PUBLIC  topo::core::TopoBuildLib
    # topo-pass-event provides topo::markPassFired — called from
    # PassPipeline.cpp / LLVMTransformBackend.cpp. Monorepo builds happen
    # to put the archive on the link line via different routes; standalone
    # CI doesn't, so spell it out explicitly.
    PUBLIC  topo-pass-event
    # Static-archive cycle back-edge: PassPipeline.cpp / LLVMTransformBackend.cpp
    # (here in TopoBackend) call into TopoTransforms (pass run() methods,
    # SymbolObfuscator::obfuscate, markPassFired(Module&,…)), while TopoTransforms
    # already links TopoBackend PUBLIC for the reverse utilities. Declaring this
    # edge is what activates the LINK_INTERFACE_MULTIPLICITY=3 set on both targets
    # (in lib/Transforms/CMakeLists.txt) so ld re-emits both archives and resolves
    # the cycle. Without it the multiplicity is dormant and a single-pass
    # standalone link fails with "undefined reference to topo::…Pass::run / …".
    PUBLIC  TopoTransforms
    PRIVATE ${LLVM_LIBS} nlohmann_json::nlohmann_json topo::core::TopoPlatform
)

# VariantBenchmark links topo runtime static libs into subprocess bench
# executables so calls emitted by Transform passes (topo_arena_*,
# topo_parallel_*, topo_adaptive_*, topo_observe_*) resolve.
target_compile_definitions(TopoBackend PRIVATE
    TOPO_BENCH_RUNTIME_LIBDIR="$<TARGET_FILE_DIR:topo-parallel>"
)
add_dependencies(TopoBackend
    topo-parallel topo-jit-api topo-adaptive topo-arena topo-observe topo-containment
)

topo_set_llvm_flags(TopoBackend)
topo_apply_std_pch_llvm(TopoBackend)

install(TARGETS TopoBackend
    EXPORT topo-llvmTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
