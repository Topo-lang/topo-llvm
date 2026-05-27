# TopoLLVMCompilerFlags.cmake — standalone compiler-flag helpers for
# topo-llvm. Mirrors the monorepo cmake/TopoCompilerFlags.cmake but
# scoped to the LLVM-touching layers.

# RPATH configuration for Unix shared library builds.
if(NOT WIN32)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(APPLE)
        set(CMAKE_MACOSX_RPATH ON)
    endif()
endif()

# When using bundled LLVM clang++, use system SDK's libc++ headers to
# avoid ABI mismatch (bundled libc++ ahead of system libc++.dylib).
if(APPLE AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../llvm-dev/lib/libc++.dylib")
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE _TOPO_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_TOPO_SDK_PATH AND EXISTS "${_TOPO_SDK_PATH}/usr/include/c++/v1")
        add_compile_options(
            "-nostdinc++"
            "-isystem" "${_TOPO_SDK_PATH}/usr/include/c++/v1"
        )
    endif()
endif()

# ── Sanitizer support ────────────────────────────────────
set(TOPO_LLVM_SANITIZER "" CACHE STRING
    "Enable sanitizers (address, undefined, thread, memory)")

function(topo_llvm_apply_sanitizer target)
    if(NOT TOPO_LLVM_SANITIZER)
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target}
            PRIVATE -fsanitize=${TOPO_LLVM_SANITIZER} -fno-omit-frame-pointer)
        target_link_options(${target}
            PRIVATE -fsanitize=${TOPO_LLVM_SANITIZER})
    endif()
endfunction()

# Compiler flag base — used by zero-LLVM C-ABI runtime libs and helper
# E2E tests.
function(topo_set_compiler_flags target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
    topo_llvm_apply_sanitizer(${target})
endfunction()

# Compiler flags for LLVM-linked targets: matches LLVM's RTTI flag and
# adds the LLVM headers as SYSTEM includes (suppresses upstream warnings).
function(topo_set_llvm_flags target)
    topo_set_compiler_flags(${target})

    # LLVM is typically built with -fno-rtti; linked code must match.
    if(DEFINED LLVM_ENABLE_RTTI AND NOT LLVM_ENABLE_RTTI)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${target} PRIVATE -fno-rtti)
        elseif(MSVC)
            target_compile_options(${target} PRIVATE /GR-)
        endif()
    endif()

    if(DEFINED LLVM_INCLUDE_DIRS)
        target_include_directories(${target} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    endif()
    if(DEFINED LLVM_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${LLVM_DEFINITIONS})
    endif()
endfunction()

# PCH helpers — no-ops in standalone (no project-wide PCH host).
function(topo_apply_std_pch target)
endfunction()
function(topo_apply_std_pch_llvm target)
endfunction()

# Resolve LLVM component names to a link-library list. When LLVM was
# built with LLVM_BUILD_LLVM_DYLIB=ON (Homebrew, shared libLLVM.dylib),
# link the single shared lib instead of dozens of static archives.
function(topo_llvm_libs out_var)
    if(LLVM_LINK_LLVM_DYLIB AND TARGET LLVM)
        set(${out_var} LLVM PARENT_SCOPE)
    else()
        llvm_map_components_to_libnames(_libs ${ARGN})
        set(${out_var} ${_libs} PARENT_SCOPE)
    endif()
endfunction()
