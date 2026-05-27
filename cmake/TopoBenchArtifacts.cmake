# Plan 28 Phase 6 — topo-bench-artifacts target assembly.
#
# Provides:
#   topo_register_bench_artifact_project(<project_dir> [VANILLA_CPP] [VANILLA_JAVA])
#     Adds an `add_custom_command(OUTPUT <stamp>, ...)` that pre-builds all
#     benchmark variants for ONE project via TopoBenchArtifactsDriver.cmake.
#     Each call appends <stamp> to the global TOPO_BENCH_ARTIFACT_STAMPS list.
#
#   topo_finalize_bench_artifacts_target()
#     Creates the `topo-bench-artifacts` custom target that depends on every
#     registered stamp. Must be called after all registrations.
#
# Layout:
#   build/artifacts/<project_name>.stamp   — per-project completion marker
#
# Incremental tracking:
#   Each custom_command DEPENDS on Topo.toml / Topo-base.toml / Topo-forced.toml
#   plus all files under src/ (C++) or src/main/java/ (JVM). When any of
#   these change, CMake re-runs the driver which rebuilds all 4 variants for
#   that project. Cross-project parallelism works — CMake dispatches one
#   custom_command per project concurrently. Within a project the driver is
#   serial (toml-swap contract demands it).
#
# First-run cost: ~20min total for ~50 projects (dominated by link time).
# Incremental cost: only projects whose Topo*.toml or sources changed.

include_guard(GLOBAL)

# Reset the per-configure stamp accumulator. Using CACHE INTERNAL with FORCE
# so the reset takes effect on re-configure (otherwise the list would grow
# unboundedly across cmake runs). `include_guard(GLOBAL)` ensures this block
# runs exactly once per configure — subsequent `include(TopoBenchArtifacts.cmake)`
# calls from sibling subdirectories are no-ops and do NOT re-reset the list,
# so registrations from topo-llvm accumulate correctly with registrations
# from topo-jvm.
set(TOPO_BENCH_ARTIFACT_STAMPS "" CACHE INTERNAL
    "List of topo-bench-artifact stamp files — one per benchmark project" FORCE)

# Target name for the topo-build CLI. In the monorepo it is the locally-
# defined `topo-build` executable; in standalone topo-llvm it is the
# imported `topo::cli::topo-build` from `find_package(topo-cli)`. Callers
# may override this before including TopoBenchArtifacts.cmake.
if(NOT DEFINED TOPO_BUILD_TARGET)
    set(TOPO_BUILD_TARGET topo-build)
endif()

function(topo_register_bench_artifact_project project_dir)
    cmake_parse_arguments(PARSE_ARGV 1 _arg
        "VANILLA_CPP;VANILLA_JAVA" "" "")

    if(NOT IS_DIRECTORY "${project_dir}")
        message(FATAL_ERROR
            "topo_register_bench_artifact_project: not a directory: ${project_dir}")
    endif()
    if(NOT EXISTS "${project_dir}/Topo.toml")
        message(FATAL_ERROR
            "topo_register_bench_artifact_project: no Topo.toml in ${project_dir}")
    endif()

    get_filename_component(_name "${project_dir}" NAME)

    # Disambiguate stamp filenames: llvm and jvm benchmark trees share names
    # (adaptive, data_layout, observability, …). Derive a backend prefix from
    # the project's parent-parent directory (`topo-llvm` / `topo-jvm`) so
    # stamps do not collide.
    get_filename_component(_parent "${project_dir}" DIRECTORY)  # benchmarks dir
    get_filename_component(_backend "${_parent}" DIRECTORY)      # topo-llvm or topo-jvm
    get_filename_component(_backend_name "${_backend}" NAME)
    if(_backend_name STREQUAL "topo-llvm")
        set(_tag "llvm")
    elseif(_backend_name STREQUAL "topo-jvm")
        set(_tag "jvm")
    else()
        set(_tag "${_backend_name}")
    endif()

    set(_stamp "${CMAKE_BINARY_DIR}/artifacts/${_tag}-${_name}.stamp")

    # Gather DEPENDS — configure-time list of every file CMake should watch.
    # Missing siblings (Topo-base.toml / Topo-forced.toml) are intentionally
    # skipped: the driver silently skips the corresponding variant.
    set(_deps "${project_dir}/Topo.toml")
    if(EXISTS "${project_dir}/Topo-base.toml")
        list(APPEND _deps "${project_dir}/Topo-base.toml")
    endif()
    if(EXISTS "${project_dir}/Topo-forced.toml")
        list(APPEND _deps "${project_dir}/Topo-forced.toml")
    endif()

    # Source files — glob once at configure time. Users adding new sources
    # will need to re-configure for dependency tracking to notice them; this
    # matches CMake's standard convention around GLOB.
    if(IS_DIRECTORY "${project_dir}/src")
        file(GLOB_RECURSE _src_cpp
            "${project_dir}/src/*.cpp"
            "${project_dir}/src/*.h"
            "${project_dir}/src/*.hpp"
            "${project_dir}/src/*.rs")
        list(APPEND _deps ${_src_cpp})
    endif()
    if(IS_DIRECTORY "${project_dir}/src/main/java")
        file(GLOB_RECURSE _src_java "${project_dir}/src/main/java/*.java")
        list(APPEND _deps ${_src_java})
    endif()
    if(IS_DIRECTORY "${project_dir}/topo")
        file(GLOB_RECURSE _src_topo "${project_dir}/topo/*.topo")
        list(APPEND _deps ${_src_topo})
    endif()
    if(IS_DIRECTORY "${project_dir}/include")
        file(GLOB_RECURSE _src_inc
            "${project_dir}/include/*.h"
            "${project_dir}/include/*.hpp")
        list(APPEND _deps ${_src_inc})
    endif()

    # Driver invocation args.
    set(_driver_args
        -DPROJECT_DIR=${project_dir}
        -DTOPO_BUILD_EXE=$<TARGET_FILE:${TOPO_BUILD_TARGET}>
        -DSTAMP_FILE=${_stamp})

    if(_arg_VANILLA_CPP)
        # clang++ resolution mirrors E2eHarness::vanillaBuild — point at the
        # bundled LLVM dev toolchain.
        if(NOT DEFINED TOPO_LLVM_BINDIR)
            message(FATAL_ERROR
                "topo_register_bench_artifact_project: VANILLA_CPP requires TOPO_LLVM_BINDIR")
        endif()
        if(WIN32)
            set(_clangxx "${TOPO_LLVM_BINDIR}/clang++.exe")
        else()
            set(_clangxx "${TOPO_LLVM_BINDIR}/clang++")
        endif()
        list(APPEND _driver_args
            -DBUILD_VANILLA=ON
            -DCLANGXX=${_clangxx}
            -DLLVM_BINDIR=${TOPO_LLVM_BINDIR})
    endif()

    if(_arg_VANILLA_JAVA)
        if(DEFINED ENV{JAVA_HOME})
            list(APPEND _driver_args -DJAVA_HOME=$ENV{JAVA_HOME})
        endif()
        # The runtime jar lives next to the topo-build executable in the
        # layout produced by topo-lang-java's Gradle task.
        list(APPEND _driver_args
            -DBUILD_VANILLA_JAVA=ON
            -DRUNTIME_JAR=$<TARGET_FILE_DIR:${TOPO_BUILD_TARGET}>/topo-runtime.jar)
    endif()

    # Express the topo-build dependency as a file path (works for both
    # locally-built and imported executables — DEPENDS in
    # add_custom_command(OUTPUT) accepts file paths or target names).
    add_custom_command(
        OUTPUT  "${_stamp}"
        COMMAND ${CMAKE_COMMAND}
                ${_driver_args}
                -P "${CMAKE_SOURCE_DIR}/cmake/TopoBenchArtifactsDriver.cmake"
        DEPENDS ${_deps}
                "$<TARGET_FILE:${TOPO_BUILD_TARGET}>"
                "${CMAKE_SOURCE_DIR}/cmake/TopoBenchArtifactsDriver.cmake"
        COMMENT "Pre-building benchmark variants for ${_name}"
        VERBATIM)

    # Append stamp to global list — read-modify-write the cache variable.
    set(_current ${TOPO_BENCH_ARTIFACT_STAMPS})
    list(APPEND _current "${_stamp}")
    set(TOPO_BENCH_ARTIFACT_STAMPS "${_current}" CACHE INTERNAL
        "List of topo-bench-artifact stamp files" FORCE)
endfunction()

# Single-invocation finaliser — call AFTER all registrations across both
# topo-llvm and topo-jvm have happened. Safe to call multiple times; CMake
# target declaration is idempotent via include_guard + NOT TARGET check.
function(topo_finalize_bench_artifacts_target)
    if(TARGET topo-bench-artifacts-finalized)
        return()
    endif()

    if(TARGET topo-bench-artifacts)
        # Remove placeholder defined elsewhere — or more conservatively,
        # just add the new deps to it.
    else()
        add_custom_target(topo-bench-artifacts
            COMMENT "Plan 28 topo-bench-artifacts — pre-build all benchmark variants")
    endif()

    # Attach every stamp as a dependency. `add_custom_target(... DEPENDS ...)`
    # accepts file-level deps in CMake; the `topo-bench-artifacts-stamps`
    # helper target exists solely to own those file deps (so the user-facing
    # `topo-bench-artifacts` target stays a clean target-level alias).
    if(TOPO_BENCH_ARTIFACT_STAMPS)
        list(LENGTH TOPO_BENCH_ARTIFACT_STAMPS _stamp_count)
        add_custom_target(topo-bench-artifacts-stamps
            DEPENDS ${TOPO_BENCH_ARTIFACT_STAMPS}
            COMMENT "Building ${_stamp_count}-entry benchmark artifact set")
        # Runtime libs linked at topo-build dispatch time. A clean
        # `cmake --build build --target topo-bench-artifacts` would otherwise
        # fail with `ld: library 'topo-X' not found` before these targets are
        # built. List mirrors topo-llvm/test/e2e/CMakeLists.txt:20 plus
        # topo-containment (newer lib that was missing from both lists).
        foreach(_dep
                topo-build-llvm-mixed
                topo-parallel
                topo-jit
                topo-adaptive
                topo-arena
                topo-observe
                topo-containment)
            if(TARGET ${_dep})
                add_dependencies(topo-bench-artifacts-stamps ${_dep})
            endif()
        endforeach()
        add_dependencies(topo-bench-artifacts topo-bench-artifacts-stamps)
    endif()

    # Sentinel so re-entry is a no-op.
    add_custom_target(topo-bench-artifacts-finalized)
    add_dependencies(topo-bench-artifacts-finalized topo-bench-artifacts)
endfunction()

# Scan helper — enumerates every subdirectory under `benchmarks_root` that
# contains a Topo.toml and registers it. Optional boolean flags select
# vanilla build flavours.
function(topo_register_all_bench_artifacts benchmarks_root)
    cmake_parse_arguments(PARSE_ARGV 1 _arg
        "VANILLA_CPP;VANILLA_JAVA" "" "EXCLUDE")

    if(NOT IS_DIRECTORY "${benchmarks_root}")
        message(FATAL_ERROR
            "topo_register_all_bench_artifacts: not a directory: ${benchmarks_root}")
    endif()

    file(GLOB _candidates RELATIVE "${benchmarks_root}" "${benchmarks_root}/*")
    foreach(_child IN LISTS _candidates)
        set(_pdir "${benchmarks_root}/${_child}")
        if(NOT IS_DIRECTORY "${_pdir}")
            continue()
        endif()
        if(NOT EXISTS "${_pdir}/Topo.toml")
            continue()
        endif()
        # Skip projects with no associated TEST_F (orphan benchmark dirs).
        # Listed explicitly so the exclusion is visible at the call site.
        if(_child IN_LIST _arg_EXCLUDE)
            continue()
        endif()
        set(_forward_args "")
        if(_arg_VANILLA_CPP)
            list(APPEND _forward_args VANILLA_CPP)
        endif()
        if(_arg_VANILLA_JAVA)
            list(APPEND _forward_args VANILLA_JAVA)
        endif()
        topo_register_bench_artifact_project("${_pdir}" ${_forward_args})
    endforeach()
endfunction()
