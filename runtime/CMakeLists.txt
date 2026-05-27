# C ABI runtime libraries — targets of LLVM Pass-generated calls.
# Zero LLVM dependency (topo-jit-engine is the only LLVM-touching runtime
# component, and lives under lib/JitEngine/).
#
# Standalone install layout: runtime/include/topo/rt/<lib>_rt.h is
# installed by the top-level CMakeLists.txt (topo-llvm-runtime-headers
# interface target); each STATIC archive below is installed with
# EXPORT topo-llvmTargets so downstream find_package picks them up as
# topo::llvm::topo-arena etc.

# TOPO_LANG_CPP_RUNTIME_INCLUDE is resolved by the top-level CMakeLists
# via find_path; below, the runtime libs use it where #include'd.

# --- topo-parallel: stage parallelism runtime ---
add_library(topo-parallel STATIC topo_parallel.cpp topo_cost.cpp)
add_library(topo::llvm::topo-parallel ALIAS topo-parallel)
target_include_directories(topo-parallel PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
if(TOPO_LANG_CPP_RUNTIME_INCLUDE)
    target_include_directories(topo-parallel PRIVATE ${TOPO_LANG_CPP_RUNTIME_INCLUDE})
endif()
target_compile_features(topo-parallel PUBLIC cxx_std_17)
set_target_properties(topo-parallel PROPERTIES CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
target_link_libraries(topo-parallel PRIVATE Threads::Threads)

# --- topo-jit-api: Lightweight JIT API stub, NO LLVM ---
# Dynamically loads topo-jit-engine at runtime via SharedLibrary.
add_library(topo-jit-api STATIC topo_jit_api.cpp)
add_library(topo::llvm::topo-jit-api ALIAS topo-jit-api)
target_include_directories(topo-jit-api PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
if(TOPO_LANG_CPP_RUNTIME_INCLUDE)
    target_include_directories(topo-jit-api PRIVATE ${TOPO_LANG_CPP_RUNTIME_INCLUDE})
endif()
target_link_libraries(topo-jit-api
    PUBLIC  topo-parallel
    PRIVATE topo::core::TopoPlatform
)
target_compile_features(topo-jit-api PUBLIC cxx_std_17)
set_target_properties(topo-jit-api PROPERTIES CXX_EXTENSIONS OFF OUTPUT_NAME topo-jit)

# Backward-compatible alias: "topo-jit" resolves to "topo-jit-api"
add_library(topo-jit ALIAS topo-jit-api)

# --- topo-pass-event: reusable Pass-event NDJSON wire ---
add_library(topo-pass-event STATIC topo_pass_event.cpp)
add_library(topo::llvm::topo-pass-event ALIAS topo-pass-event)
target_include_directories(topo-pass-event PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(topo-pass-event PUBLIC cxx_std_17)
set_target_properties(topo-pass-event PROPERTIES CXX_EXTENSIONS OFF)

# topo-parallel PUBLIC-uses pass-event for task spawn/join sites.
target_link_libraries(topo-parallel PUBLIC topo-pass-event)

# --- topo-adaptive: Adaptive JIT monitoring runtime ---
add_library(topo-adaptive STATIC topo_adaptive.cpp topo_adaptive_register.cpp)
add_library(topo::llvm::topo-adaptive ALIAS topo-adaptive)
target_include_directories(topo-adaptive PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
if(TOPO_LANG_CPP_RUNTIME_INCLUDE)
    target_include_directories(topo-adaptive PRIVATE ${TOPO_LANG_CPP_RUNTIME_INCLUDE})
endif()
target_link_libraries(topo-adaptive
    PUBLIC  topo-jit-api topo-pass-event
)
target_compile_features(topo-adaptive PUBLIC cxx_std_17)
set_target_properties(topo-adaptive PROPERTIES CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
target_link_libraries(topo-adaptive PRIVATE Threads::Threads)

# --- topo-observe: Observability tracing runtime ---
add_library(topo-observe STATIC topo_observe.cpp)
add_library(topo::llvm::topo-observe ALIAS topo-observe)
target_include_directories(topo-observe PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(topo-observe PUBLIC cxx_std_17)
set_target_properties(topo-observe PROPERTIES CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
target_link_libraries(topo-observe PRIVATE Threads::Threads)

# --- topo-arena: Bump allocator for lifetime scoping ---
add_library(topo-arena STATIC topo_arena.cpp)
add_library(topo::llvm::topo-arena ALIAS topo-arena)
target_include_directories(topo-arena PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(topo-arena PUBLIC topo-pass-event)
target_compile_features(topo-arena PUBLIC cxx_std_17)
set_target_properties(topo-arena PROPERTIES CXX_EXTENSIONS OFF)

# --- topo-containment: Containment violation recording runtime ---
add_library(topo-containment STATIC topo_containment.cpp)
add_library(topo::llvm::topo-containment ALIAS topo-containment)
target_include_directories(topo-containment PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(topo-containment PUBLIC cxx_std_17)
set_target_properties(topo-containment PROPERTIES CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
target_link_libraries(topo-containment PRIVATE Threads::Threads)

install(TARGETS
    topo-parallel topo-jit-api topo-pass-event topo-adaptive
    topo-observe topo-arena topo-containment
    EXPORT topo-llvmTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
