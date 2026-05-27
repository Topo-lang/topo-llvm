# topo-prof is now a thin shim. The analyze/profile/hints core
# lives in TopoProfile (topo-core, zero-LLVM) behind the TTIProvider seam;
# TopoProfileLlvm supplies the LLVM-bound TTI provider. TopoProfile PUBLIC-
# propagates the seam header + nlohmann; TopoProfileLlvm PUBLIC-propagates
# TopoProfile and brings the LLVM TTI libs.
add_executable(topo-prof
    main.cpp
)

target_link_libraries(topo-prof PRIVATE
    TopoProfileLlvm
)

topo_set_llvm_flags(topo-prof)

topo_apply_std_pch_llvm(topo-prof)
set_property(TARGET topo-prof PROPERTY JOB_POOL_LINK llvm_link)

install(TARGETS topo-prof
    EXPORT topo-llvmTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
