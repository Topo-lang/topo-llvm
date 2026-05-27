topo_llvm_libs(DECOMPILE_LLVM_LIBS
    analysis
    core
    irreader
    bitreader
    object
    support
    demangle
)

add_library(TopoDecompile
    LLVMLifter.cpp
)

target_include_directories(TopoDecompile PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(TopoDecompile
    PUBLIC  topo::core::TopoTranspile topo::core::TopoSema
    PRIVATE ${DECOMPILE_LLVM_LIBS} TopoBackend
)

topo_set_llvm_flags(TopoDecompile)
topo_apply_std_pch_llvm(TopoDecompile)

install(TARGETS TopoDecompile
    EXPORT topo-llvmTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)