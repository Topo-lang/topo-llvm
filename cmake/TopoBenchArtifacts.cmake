# topo-bench-artifacts target assembly: aggregates per-benchmark variants
# into a single buildable target.
#
# Provides:
#   topo_register_bench_artifact_project(<project_dir> [VANILLA_CPP] [VANILLA_JAVA])
#     Adds an `add_custom_command(OUTPUT <stamp>, ...)` that pre-builds all
#     benchmark variants for ONE project via TopoBenchArtifactsDriver.cmake,
#     wraps the stamp in a per-project custom target, and records that target
#     name in the GLOBAL property TOPO_BENCH_ARTIFACT_TARGETS.
#
#   topo_finalize_bench_artifacts_target()
#     Creates / extends the `topo-bench-artifacts` aggregate so it depends on
#     every per-project target recorded so far. Idempotent and cumulative —
#     call it after each package's registrations; the last call (meta root,
#     after all packages registered) wires the complete set.
#
# Layout:
#   build/artifacts/<tag>-<project_name>.stamp   — per-project completion marker
#   (tag = llvm | jvm, derived from the benchmarks tree's backend dir)
#
# Accumulator = a GLOBAL property, NOT a CACHE variable.
#   This file is duplicated across three repos (meta cmake/, topo-jvm/cmake/,
#   topo-llvm/cmake/). `include_guard(GLOBAL)` only guards re-inclusion of the
#   SAME file, so in the meta union build all three copies' top-level code
#   runs once each. A CACHE-variable accumulator reset at top level therefore
#   got CLOBBERED every time a sibling copy was included, and the once-only
#   finalize wired only whichever subset existed at the first finalize — the
#   meta `topo-bench-artifacts` ended up depending on ~12 of 48 stamps. A
#   GLOBAL property is fresh at the start of each configure (never persisted
#   across configures like a cache entry, never reset mid-configure), so every
#   copy's registrations accumulate into one list and a cumulative finalize
#   picks up the full set regardless of include / add_subdirectory order.
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

# Target name for the topo-build CLI. In the monorepo / meta union build it is
# the locally-defined `topo-build` executable; in a standalone backend repo it
# is the imported `topo::cli::topo-build` from `find_package(topo-cli)`.
# Callers may override this before including TopoBenchArtifacts.cmake.
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

    # Per-project target that owns this stamp. The aggregate depends on these
    # TARGETS (target-level deps resolve across any directory scope) rather
    # than on the stamp file paths: a file-level DEPENDS only links to its
    # producing add_custom_command when that command sits in the same-or-
    # ancestor scope, which fails when registration runs from a child
    # (benchmarks/) scope or from two scopes in the meta build. Register each
    # project exactly once — the meta registers some twice (a package's own
    # add_subdirectory(benchmarks) plus a meta-root call).
    set(_proj_target "topo-bench-artifact-${_tag}-${_name}")
    if(TARGET ${_proj_target})
        return()
    endif()

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

    # Expose the backend tools (topo-build-jvm-java / topo-build-llvm-*) to
    # topo-build via PATH. topo-build resolves them by its own executable dir +
    # PATH, but in a build tree they live in separate subdirs, not next to it.
    # Pass the dirs of whichever backend tools exist; the driver prepends them
    # to PATH before invoking topo-build (| separator — split in the driver).
    set(_backend_tool_dirs "")
    foreach(_bt topo-build-jvm-java topo-build-llvm-cpp topo-build-llvm-rust topo-build-llvm-mixed)
        if(TARGET ${_bt})
            if(_backend_tool_dirs STREQUAL "")
                set(_backend_tool_dirs "$<TARGET_FILE_DIR:${_bt}>")
            else()
                set(_backend_tool_dirs "${_backend_tool_dirs}|$<TARGET_FILE_DIR:${_bt}>")
            endif()
        endif()
    endforeach()
    if(NOT _backend_tool_dirs STREQUAL "")
        list(APPEND _driver_args "-DBACKEND_TOOL_DIRS=${_backend_tool_dirs}")
    endif()

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
        # topo-runtime.jar is staged by deploy-topo-runtime-java POST_BUILD
        # next to topo-build-jvm-java (NOT next to topo-build — that held only
        # in the monorepo where both binaries shared one tools dir). Mirror the
        # e2e harness's TOPO_RUNTIME_JAR_DIR = $<TARGET_FILE_DIR:topo-build-jvm-java>.
        if(TARGET topo-build-jvm-java)
            list(APPEND _driver_args
                -DBUILD_VANILLA_JAVA=ON
                -DRUNTIME_JAR=$<TARGET_FILE_DIR:topo-build-jvm-java>/topo-runtime.jar)
        else()
            list(APPEND _driver_args
                -DBUILD_VANILLA_JAVA=ON
                -DRUNTIME_JAR=$<TARGET_FILE_DIR:${TOPO_BUILD_TARGET}>/topo-runtime.jar)
        endif()
    endif()

    # Express the topo-build dependency as a file path (works for both
    # locally-built and imported executables — DEPENDS in
    # add_custom_command(OUTPUT) accepts file paths or target names).
    add_custom_command(
        OUTPUT  "${_stamp}"
        COMMAND ${CMAKE_COMMAND}
                ${_driver_args}
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/TopoBenchArtifactsDriver.cmake"
        DEPENDS ${_deps}
                "$<TARGET_FILE:${TOPO_BUILD_TARGET}>"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/TopoBenchArtifactsDriver.cmake"
        COMMENT "Pre-building benchmark variants for ${_name}"
        VERBATIM)

    # Own the stamp with a per-project target IN THIS SCOPE so the file-level
    # DEPENDS resolves locally; the aggregate then takes a target-level dep.
    add_custom_target(${_proj_target} DEPENDS "${_stamp}")

    # Ensure the backend tool this project dispatches to is built before its
    # driver runs — topo-build invokes it (and the driver exposes it via PATH).
    if(_tag STREQUAL "jvm")
        if(TARGET topo-build-jvm-java)
            add_dependencies(${_proj_target} topo-build-jvm-java)
        endif()
    elseif(_tag STREQUAL "llvm")
        foreach(_bt topo-build-llvm-mixed topo-build-llvm-cpp topo-build-llvm-rust)
            if(TARGET ${_bt})
                add_dependencies(${_proj_target} ${_bt})
            endif()
        endforeach()
    endif()

    # Record the TARGET NAME in the GLOBAL property (scope- and file-
    # independent, fresh each configure). Appending here, plus a cumulative
    # finalize, makes the aggregate robust to include / subdirectory order
    # across the three duplicated copies of this file.
    set_property(GLOBAL APPEND PROPERTY TOPO_BENCH_ARTIFACT_TARGETS "${_proj_target}")
endfunction()

# Cumulative finaliser — safe to call any number of times, from any scope.
# Each call (re)wires `topo-bench-artifacts` to every per-project target
# recorded so far in the GLOBAL property. The per-package benchmarks/
# CMakeLists and the meta root all call it; the meta root's call (after every
# package has registered) is what guarantees the aggregate covers the full
# union. add_dependencies / add_custom_target are idempotent under the NOT
# TARGET guards, so repeated calls neither error nor duplicate work.
function(topo_finalize_bench_artifacts_target)
    if(NOT TARGET topo-bench-artifacts)
        add_custom_target(topo-bench-artifacts
            COMMENT "topo-bench-artifacts — pre-build all benchmark variants")
    endif()
    if(NOT TARGET topo-bench-artifacts-stamps)
        add_custom_target(topo-bench-artifacts-stamps
            COMMENT "Building the benchmark artifact set")
        add_dependencies(topo-bench-artifacts topo-bench-artifacts-stamps)
    endif()

    # Depend on every per-project target recorded so far. Target-level
    # dependencies resolve across directory scopes, unlike the file-level
    # DEPENDS used previously (which silently failed when a project was
    # registered in a child scope).
    get_property(_bench_targets GLOBAL PROPERTY TOPO_BENCH_ARTIFACT_TARGETS)
    foreach(_proj_target IN LISTS _bench_targets)
        if(TARGET ${_proj_target})
            add_dependencies(topo-bench-artifacts-stamps ${_proj_target})
        endif()
    endforeach()

    # Runtime libs linked at topo-build dispatch time. A clean
    # `cmake --build build --target topo-bench-artifacts` would otherwise fail
    # with `ld: library 'topo-X' not found` before these targets are built.
    # List mirrors topo-llvm/test/e2e/CMakeLists.txt plus topo-containment.
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
