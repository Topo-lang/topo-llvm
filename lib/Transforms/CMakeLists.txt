add_library(TopoTransforms
    TopoInlinePass.cpp
    TopoFlattenPass.cpp
    TopoReorderPass.cpp
    TopoLayoutPass.cpp
    TopoParallelPass.cpp
    AdaptiveDispatchPass.cpp
    ReturnSpecializationPass.cpp
    PipelineCodeGenPass.cpp
    DataLayoutPass.cpp
    IndirectionPass.cpp
    ObservabilityPass.cpp
    SymbolObfuscator.cpp
    LifetimeArenaPass.cpp
    LoopParallelizePass.cpp
    PrefetchPass.cpp
    ContainmentInterceptionPass.cpp
    PassFiredMarker.cpp
    RuntimeAbiCheck.cpp
)
add_library(topo::llvm::TopoTransforms ALIAS TopoTransforms)

target_link_libraries(TopoTransforms PUBLIC
    TopoBackend
    topo::core::TopoAnalysis
    topo::core::TopoCheck
)

# Static-archive circular dep with TopoBackend: TopoTransforms' Passes
# implement entry points that TopoBackend/PassPipeline.cpp calls into;
# TopoBackend in turn provides utilities Passes consume. CMake encodes
# this via repeated archive on the link line — Linux ld won't backtrack
# past a static archive after walking it once. LINK_INTERFACE_MULTIPLICITY
# tells CMake to re-emit both archives when the dep cycle is detected,
# letting ld resolve symbols in either direction.
set_target_properties(TopoTransforms TopoBackend
    PROPERTIES LINK_INTERFACE_MULTIPLICITY 3)

topo_set_llvm_flags(TopoTransforms)
topo_apply_std_pch_llvm(TopoTransforms)

install(TARGETS TopoTransforms
    EXPORT topo-llvmTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)