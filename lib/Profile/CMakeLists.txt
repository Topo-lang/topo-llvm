# LLVM-bound half of `topo-prof`.
#
# topo-core/lib/Profile (TopoProfile) owns the zero-LLVM analyze / profile /
# hints orchestration behind the topo::profile::TTIProvider seam. This
# library supplies the ONLY LLVM-bound data source — static Target
# Transform Info cost estimation read from build/*.ll — as a concrete
# provider. The topo-prof CLI is a thin shim that wires the two together.

topo_llvm_libs(PROFILE_LLVM_LIBS
    analysis
    core
    irreader
    mc
    support
    target
    demangle
    native
)

add_library(TopoProfileLlvm
    Llvm/LlvmTTIProvider.cpp
)

# `lib/` so the internal `Profile/Llvm/LlvmTTIProvider.h` include path
# resolves. topo-core's TTIProvider seam header comes via the imported
# topo::core::TopoProfile target.
target_include_directories(TopoProfileLlvm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

# tomlplusplus header-only — used by TopoProfile's seam. find_path so
# we don't depend on a vcpkg-CONFIG package; vcpkg manifest installs the
# headers and this resolves them.
find_path(TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp)
if(TOMLPLUSPLUS_INCLUDE_DIR)
    target_include_directories(TopoProfileLlvm PRIVATE ${TOMLPLUSPLUS_INCLUDE_DIR})
endif()

target_link_libraries(TopoProfileLlvm
    # topo::core::TopoProfile carries the seam declaration + nlohmann;
    # the LLVM libs are this library's private implementation detail.
    PUBLIC  topo::core::TopoProfile
    PRIVATE ${PROFILE_LLVM_LIBS}
)

topo_set_llvm_flags(TopoProfileLlvm)
topo_apply_std_pch_llvm(TopoProfileLlvm)

install(TARGETS TopoProfileLlvm
    EXPORT topo-llvmTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)