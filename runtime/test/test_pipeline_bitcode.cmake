# Build-time helper: compile a tiny C++ source into LLVM bitcode, then
# convert the bitcode to a C++ byte-array header that JITRealPathTest can
# include.  The bitcode represents a demangle-able function that the JIT
# engine can locate, compile, and hand back as a function pointer.

set(_BC_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/test_pipeline_source.cpp")
set(_BC_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/test_pipeline.bc")
set(_BC_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/test_pipeline_bitcode.h")
set(_BC_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/embed_bitcode.cmake")

if(NOT TOPO_LLVM_BINDIR)
    message(FATAL_ERROR "TOPO_LLVM_BINDIR not set — JIT bitcode fixture requires bundled LLVM clang.")
endif()

# Heal a dangling cache entry (a versioned Homebrew Cellar path cached before
# an LLVM patch bump) so reconfigure re-probes instead of invoking a vanished
# clang++.
if(_TOPO_CLANGXX AND NOT EXISTS "${_TOPO_CLANGXX}")
    unset(_TOPO_CLANGXX CACHE)
endif()

find_program(_TOPO_CLANGXX clang++
    PATHS "${TOPO_LLVM_BINDIR}"
    NO_DEFAULT_PATH)
if(NOT _TOPO_CLANGXX)
    message(FATAL_ERROR "clang++ not found under ${TOPO_LLVM_BINDIR}.")
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_custom_command(
    OUTPUT "${_BC_OUTPUT}"
    COMMAND "${_TOPO_CLANGXX}"
            -std=c++17 -O2
            -emit-llvm -c
            "${_BC_SOURCE}"
            -o "${_BC_OUTPUT}"
    DEPENDS "${_BC_SOURCE}"
    COMMENT "Generating JIT test pipeline bitcode"
    VERBATIM)

add_custom_command(
    OUTPUT "${_BC_HEADER}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=${_BC_OUTPUT}
            -DOUTPUT_FILE=${_BC_HEADER}
            -DARRAY_NAME=kTestPipelineBitcode
            -P "${_BC_SCRIPT}"
    DEPENDS "${_BC_OUTPUT}" "${_BC_SCRIPT}"
    COMMENT "Embedding JIT test pipeline bitcode as C array"
    VERBATIM)

add_custom_target(topo-test-pipeline-bitcode
    DEPENDS "${_BC_HEADER}")
