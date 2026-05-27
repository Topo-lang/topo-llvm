# Plan 28 Phase 6 — topo-bench-artifacts per-project driver.
#
# Invoked as:
#   cmake -P TopoBenchArtifactsDriver.cmake
#     -DPROJECT_DIR=<abs/path/to/benchmark/project>
#     -DTOPO_BUILD_EXE=<abs/path/to/topo-build>
#     -DSTAMP_FILE=<abs/path/to/.../<project>.stamp>
#     [-DBUILD_VANILLA=ON]           # C++ only — vanilla clang++ -O2 baseline
#     [-DBUILD_VANILLA_JAVA=ON]      # JVM only — javac + jar baseline
#     [-DCLANGXX=<abs/path>]         # required when BUILD_VANILLA=ON
#     [-DSTANDARD=c++17]             # vanilla C++ standard (default c++17)
#     [-DLLVM_BINDIR=<abs/path>]     # llvm tool dir (for vanilla C++)
#     [-DJAVA_HOME=<abs/path>]       # for vanilla JAR builds
#     [-DRUNTIME_JAR=<abs/path>]     # topo-runtime.jar (for vanilla JAR)
#
# Runs all benchmark variants for ONE project sequentially:
#   1. (C++ only, optional) vanilla  — clang++ -O2 -o build/baseline
#   2. (JVM only, optional) vanilla  — javac + jar -> build/vanilla.jar
#   3. topo base   — swap Topo-base.toml   -> Topo.toml, run topo-build, restore
#   4. topo auto   — run topo-build on Topo.toml as-is
#   5. topo forced — swap Topo-forced.toml -> Topo.toml, run topo-build, restore
#
# All builds are mandatory except vanilla (which some projects skip because
# their sources use topo runtime headers that plain clang can't compile).
# On success the stamp file is touched. On failure the stamp is NOT touched
# and the build fails with a clear diagnostic; CMake will re-run the driver
# next invocation (per standard custom_command semantics).
#
# Incremental semantics: CMake tracks staleness via the stamp file's mtime
# relative to the DEPENDS list in the upstream `add_custom_command(OUTPUT ...)`.
# When any Topo*.toml or source file changes, the stamp becomes stale and the
# driver runs again. Serialisation within a project is automatic — one driver
# invocation does all 4 variants sequentially, so inter-variant races are
# impossible by construction.

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PROJECT_DIR)
    message(FATAL_ERROR "TopoBenchArtifactsDriver: PROJECT_DIR not set")
endif()
if(NOT DEFINED TOPO_BUILD_EXE)
    message(FATAL_ERROR "TopoBenchArtifactsDriver: TOPO_BUILD_EXE not set")
endif()
if(NOT DEFINED STAMP_FILE)
    message(FATAL_ERROR "TopoBenchArtifactsDriver: STAMP_FILE not set")
endif()

if(NOT IS_DIRECTORY "${PROJECT_DIR}")
    message(FATAL_ERROR "TopoBenchArtifactsDriver: PROJECT_DIR does not exist: ${PROJECT_DIR}")
endif()
if(NOT EXISTS "${TOPO_BUILD_EXE}")
    message(FATAL_ERROR "TopoBenchArtifactsDriver: topo-build not found: ${TOPO_BUILD_EXE}")
endif()

set(_topo_toml       "${PROJECT_DIR}/Topo.toml")
set(_base_toml       "${PROJECT_DIR}/Topo-base.toml")
set(_forced_toml     "${PROJECT_DIR}/Topo-forced.toml")
set(_saved_toml      "${PROJECT_DIR}/Topo.toml.prebuild-saved")
set(_cache_dir       "${PROJECT_DIR}/.topo-cache")

if(NOT EXISTS "${_topo_toml}")
    message(FATAL_ERROR "TopoBenchArtifactsDriver: Topo.toml not found in ${PROJECT_DIR}")
endif()

# ---------------------------------------------------------------------------
# Helper: run a single topo-build invocation in PROJECT_DIR.
# Fails the driver (and thus the custom_command) on non-zero exit.
# ---------------------------------------------------------------------------
function(_topo_run_build label)
    message(STATUS "[topo-bench-artifacts] ${PROJECT_DIR}: ${label}")
    # Clean .topo-cache so swapped toml doesn't reuse stale artefacts.
    file(REMOVE_RECURSE "${_cache_dir}")
    execute_process(
        COMMAND "${TOPO_BUILD_EXE}" --dump-ir
        WORKING_DIRECTORY "${PROJECT_DIR}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE  _err
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "[topo-bench-artifacts] ${PROJECT_DIR}: ${label} FAILED (rc=${_rc})\n"
            "stdout:\n${_out}\n"
            "stderr:\n${_err}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Helper: swap Topo.toml <- <variant>.toml, invoke _topo_run_build, restore.
# ---------------------------------------------------------------------------
function(_topo_build_variant label variant_toml)
    if(NOT EXISTS "${variant_toml}")
        message(STATUS
            "[topo-bench-artifacts] ${PROJECT_DIR}: skip ${label} (${variant_toml} not present)")
        return()
    endif()

    configure_file("${_topo_toml}" "${_saved_toml}" COPYONLY)
    configure_file("${variant_toml}" "${_topo_toml}" COPYONLY)

    # Wrap in a pseudo-try: on build failure we MUST restore the toml before
    # propagating the error, otherwise a retry sees a broken tree. CMake
    # lacks try/finally, so we capture the error state ourselves.
    set(_build_failed FALSE)
    message(STATUS "[topo-bench-artifacts] ${PROJECT_DIR}: ${label}")
    file(REMOVE_RECURSE "${_cache_dir}")
    execute_process(
        COMMAND "${TOPO_BUILD_EXE}" --dump-ir
        WORKING_DIRECTORY "${PROJECT_DIR}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE  _err
    )
    if(NOT _rc EQUAL 0)
        set(_build_failed TRUE)
    endif()

    # Restore original Topo.toml — always, even on failure.
    configure_file("${_saved_toml}" "${_topo_toml}" COPYONLY)
    file(REMOVE "${_saved_toml}")

    if(_build_failed)
        message(FATAL_ERROR
            "[topo-bench-artifacts] ${PROJECT_DIR}: ${label} FAILED (rc=${_rc})\n"
            "stdout:\n${_out}\n"
            "stderr:\n${_err}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Vanilla C++ baseline (optional — some projects use topo/* headers that
# plain clang cannot compile).
# ---------------------------------------------------------------------------
function(_topo_vanilla_cpp)
    if(NOT DEFINED CLANGXX OR NOT EXISTS "${CLANGXX}")
        message(FATAL_ERROR
            "[topo-bench-artifacts] BUILD_VANILLA=ON requires CLANGXX=<path to clang++>")
    endif()
    if(NOT DEFINED STANDARD)
        set(STANDARD "c++17")
    endif()

    # Minimal Topo.toml parse — extract sources + include + standard + output.
    # The full parser lives in topo-core; this one-shot regex mirror is
    # enough because benchmark tomls have a stable shape (no nested tables
    # in the fields we read).
    file(READ "${_topo_toml}" _toml_txt)

    # Extract `sources = [ ... ]` (single line in all benchmark tomls). We
    # match lazily up to the first `]`; in CMake regex, [^]]* expresses
    # "any chars except ]" (CMake treats the first `]` after `[^` as the
    # closing bracket of the class, not a literal). Using bracket-exprs
    # here avoids the double-backslash escaping of a more explicit form.
    set(_sources_list "")
    string(REGEX MATCH "sources[ \t]*=[ \t]*\\[([^]]*)\\]" _ "${_toml_txt}")
    if(CMAKE_MATCH_1)
        string(REPLACE "\"" "" _sl "${CMAKE_MATCH_1}")
        string(REPLACE "," ";" _sl "${_sl}")
        foreach(_s IN LISTS _sl)
            string(STRIP "${_s}" _s)
            if(NOT _s STREQUAL "")
                list(APPEND _sources_list "${_s}")
            endif()
        endforeach()
    endif()

    set(_include_list "")
    string(REGEX MATCH "include[ \t]*=[ \t]*\\[([^]]*)\\]" _ "${_toml_txt}")
    if(CMAKE_MATCH_1)
        string(REPLACE "\"" "" _il "${CMAKE_MATCH_1}")
        string(REPLACE "," ";" _il "${_il}")
        foreach(_i IN LISTS _il)
            string(STRIP "${_i}" _i)
            if(NOT _i STREQUAL "")
                list(APPEND _include_list "${_i}")
            endif()
        endforeach()
    endif()

    # Extract standard override if present.
    set(_std "${STANDARD}")
    string(REGEX MATCH "standard[ \t]*=[ \t]*\"([^\"]+)\"" _ "${_toml_txt}")
    if(CMAKE_MATCH_1)
        set(_std "${CMAKE_MATCH_1}")
    endif()

    # Expand glob patterns (only simple `dir/*.ext` supported — matches
    # E2eHarness's vanillaBuild parser).
    set(_expanded_sources "")
    foreach(_pat IN LISTS _sources_list)
        if(_pat MATCHES "\\*")
            get_filename_component(_pat_dir "${_pat}" DIRECTORY)
            get_filename_component(_pat_ext "${_pat}" EXT)
            file(GLOB _glob_hits
                "${PROJECT_DIR}/${_pat_dir}/*${_pat_ext}")
            foreach(_h IN LISTS _glob_hits)
                list(APPEND _expanded_sources "${_h}")
            endforeach()
        else()
            list(APPEND _expanded_sources "${PROJECT_DIR}/${_pat}")
        endif()
    endforeach()

    if(NOT _expanded_sources)
        message(FATAL_ERROR
            "[topo-bench-artifacts] ${PROJECT_DIR}: vanilla build found no sources")
    endif()

    # Build args.
    set(_args -O2 -std=${_std})
    if(APPLE)
        execute_process(
            COMMAND xcrun --show-sdk-path
            OUTPUT_VARIABLE _sdk
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _sdk_rc)
        if(_sdk_rc EQUAL 0 AND _sdk)
            list(APPEND _args -isysroot "${_sdk}")
        endif()
    endif()
    foreach(_i IN LISTS _include_list)
        if(IS_ABSOLUTE "${_i}")
            list(APPEND _args "-I${_i}")
        else()
            list(APPEND _args "-I${PROJECT_DIR}/${_i}")
        endif()
    endforeach()
    foreach(_s IN LISTS _expanded_sources)
        list(APPEND _args "${_s}")
    endforeach()

    file(MAKE_DIRECTORY "${PROJECT_DIR}/build")
    if(WIN32)
        set(_out "${PROJECT_DIR}/build/baseline.exe")
    else()
        set(_out "${PROJECT_DIR}/build/baseline")
    endif()
    list(APPEND _args -o "${_out}")

    message(STATUS "[topo-bench-artifacts] ${PROJECT_DIR}: vanilla C++ -> ${_out}")
    execute_process(
        COMMAND "${CLANGXX}" ${_args}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE  _stderr)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "[topo-bench-artifacts] ${PROJECT_DIR}: vanilla C++ FAILED (rc=${_rc})\n"
            "stdout:\n${_stdout}\n"
            "stderr:\n${_stderr}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Vanilla Java baseline (JVM).
# ---------------------------------------------------------------------------
function(_topo_vanilla_java)
    if(NOT DEFINED JAVA_HOME OR NOT IS_DIRECTORY "${JAVA_HOME}")
        message(FATAL_ERROR
            "[topo-bench-artifacts] BUILD_VANILLA_JAVA=ON requires JAVA_HOME=<path>")
    endif()
    set(_javac "${JAVA_HOME}/bin/javac")
    set(_jar   "${JAVA_HOME}/bin/jar")
    if(NOT EXISTS "${_javac}")
        message(FATAL_ERROR "[topo-bench-artifacts] javac not found at ${_javac}")
    endif()

    set(_class_dir "${PROJECT_DIR}/build/vanilla-classes")
    file(REMOVE_RECURSE "${_class_dir}")
    file(MAKE_DIRECTORY "${_class_dir}")

    file(GLOB_RECURSE _java_files
        "${PROJECT_DIR}/src/main/java/*.java")
    if(NOT _java_files)
        message(FATAL_ERROR
            "[topo-bench-artifacts] ${PROJECT_DIR}: no .java under src/main/java")
    endif()

    set(_javac_args -d "${_class_dir}" --release 21)
    if(DEFINED RUNTIME_JAR AND EXISTS "${RUNTIME_JAR}")
        list(APPEND _javac_args -classpath "${RUNTIME_JAR}")
    endif()
    foreach(_f IN LISTS _java_files)
        list(APPEND _javac_args "${_f}")
    endforeach()

    message(STATUS "[topo-bench-artifacts] ${PROJECT_DIR}: vanilla Java javac")
    execute_process(
        COMMAND "${_javac}" ${_javac_args}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_VARIABLE _e)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "[topo-bench-artifacts] ${PROJECT_DIR}: javac FAILED (rc=${_rc})\n"
            "${_o}\n${_e}")
    endif()

    # Extract runtime classes into class dir so the JAR is self-contained
    # (mirrors topo-build-jvm-java's bundling step).
    if(DEFINED RUNTIME_JAR AND EXISTS "${RUNTIME_JAR}")
        execute_process(
            COMMAND "${_jar}" xf "${RUNTIME_JAR}"
            WORKING_DIRECTORY "${_class_dir}"
            RESULT_VARIABLE _rc)
    endif()

    file(MAKE_DIRECTORY "${PROJECT_DIR}/build")
    set(_jar_out "${PROJECT_DIR}/build/vanilla.jar")
    message(STATUS "[topo-bench-artifacts] ${PROJECT_DIR}: vanilla Java jar -> ${_jar_out}")
    execute_process(
        COMMAND "${_jar}" --create --file "${_jar_out}"
                --main-class app.Main -C "${_class_dir}" .
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_VARIABLE _e)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
            "[topo-bench-artifacts] ${PROJECT_DIR}: jar FAILED (rc=${_rc})\n"
            "${_o}\n${_e}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Main flow.
# ---------------------------------------------------------------------------

# Guard against leftover .prebuild-saved from a prior crashed run.
if(EXISTS "${_saved_toml}")
    message(WARNING
        "[topo-bench-artifacts] ${PROJECT_DIR}: found stale ${_saved_toml}; "
        "restoring it as Topo.toml before rebuild")
    configure_file("${_saved_toml}" "${_topo_toml}" COPYONLY)
    file(REMOVE "${_saved_toml}")
endif()

if(BUILD_VANILLA)
    _topo_vanilla_cpp()
endif()
if(BUILD_VANILLA_JAVA)
    _topo_vanilla_java()
endif()

_topo_build_variant("topo base"   "${_base_toml}")

# auto: use Topo.toml as-is; skip if the project happens to lack one (defensive)
_topo_run_build("topo auto")

_topo_build_variant("topo forced" "${_forced_toml}")

# All variants succeeded — mark the stamp. Use file(TOUCH) so mtime is
# updated even when the file already exists.
get_filename_component(_stamp_dir "${STAMP_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${_stamp_dir}")
file(TOUCH "${STAMP_FILE}")
message(STATUS "[topo-bench-artifacts] ${PROJECT_DIR}: OK -> ${STAMP_FILE}")
