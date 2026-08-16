# topo-bench-artifacts per-project driver: builds every benchmark variant
# for one project (vanilla baseline + topo base/auto/forced).
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
#   3. topo base   — scratch copy: Topo-base.toml -> scratch Topo.toml,
#                    run topo-build in the scratch dir, copy outputs back
#   4. topo auto   — scratch copy of Topo.toml as-is, build, copy outputs back
#   5. topo forced — scratch copy: Topo-forced.toml -> scratch Topo.toml,
#                    build, copy outputs back
#
# The three topo variants run on a PRIVATE scratch copy of the project
# (under <STAMP_FILE>-scratch, i.e. inside the build tree) instead of
# swapping the committed Topo.toml in place. The real benchmark tree is
# only ever read (and receives the built artefacts back); its Topo.toml is
# never written. This removes the build-side writer from the in-place
# Topo.toml swap race that dirtied benchmark trees and flaked the
# e2e.check_clean guards under a parallel `ctest -j N` (issue
# e2e-inplace-toml-swap-race-dirties-tree — decision: ctest-side writers
# are serialised with per-project RESOURCE_LOCKs, the build-side writer
# moved to scratch copies; see the fix-batch planning doc).
#
# Include rewriting: benchmark Topo.tomls carry depth-sensitive relative
# includes ("../../../topo-lang-cpp/runtime/include", "../../runtime/
# include"). The scratch copy lives under the build dir, so those would
# resolve differently; every [build].include entry is therefore rewritten
# to an absolute path anchored at the REAL project dir before the build.
# topo-build passes absolute include dirs through unchanged
# (topo-cli Config.cpp: baseDir / <abs> == <abs>).
#
# All builds are mandatory except vanilla (which some projects skip because
# their sources use topo runtime headers that plain clang can't compile).
# On success the stamp file is touched. On failure the stamp is NOT touched
# and the build fails with a clear diagnostic; CMake will re-run the driver
# next invocation (per standard custom_command semantics). An interrupted
# run may leave the scratch dir behind; the next invocation removes it
# wholesale at prepare time, so it is self-healing and never dirties the
# source tree.
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

# Backend tools (topo-build-jvm-java / topo-build-llvm-*) are resolved by
# topo-build via its own executable dir + PATH. In a build tree they are not
# next to topo-build, so registration passes their dirs in BACKEND_TOOL_DIRS;
# prepend them to PATH so the execute_process calls below inherit it.
if(DEFINED BACKEND_TOOL_DIRS AND NOT BACKEND_TOOL_DIRS STREQUAL "")
    string(REPLACE "|" ";" _backend_dir_list "${BACKEND_TOOL_DIRS}")
    if(WIN32)
        set(_path_sep ";")
    else()
        set(_path_sep ":")
    endif()
    foreach(_bd IN LISTS _backend_dir_list)
        set(ENV{PATH} "${_bd}${_path_sep}$ENV{PATH}")
    endforeach()
endif()

set(_topo_toml       "${PROJECT_DIR}/Topo.toml")
set(_saved_toml      "${PROJECT_DIR}/Topo.toml.prebuild-saved")
set(_scratch_dir     "${STAMP_FILE}-scratch")

if(NOT EXISTS "${_topo_toml}")
    message(FATAL_ERROR "TopoBenchArtifactsDriver: Topo.toml not found in ${PROJECT_DIR}")
endif()

# ---------------------------------------------------------------------------
# Scratch-copy helpers. The scratch dir mirrors the real project's layout
# (src/, topo/, include/, Topo*.toml) so project-relative paths in the
# tomls keep working; only the depth-sensitive ../.. entries need the
# include rewrite handled by _topo_rewrite_include.
# ---------------------------------------------------------------------------

# Parse the [build].include array (single line, stable shape — the same
# assumption as _topo_vanilla_cpp's regex mirror) into a CMake list.
function(_topo_include_list toml_path out_var)
    file(READ "${toml_path}" _t)
    set(_list "")
    string(REGEX MATCH "include[ \t]*=[ \t]*\\[([^]]*)\\]" _ "${_t}")
    if(CMAKE_MATCH_1)
        string(REPLACE "\"" "" _sl "${CMAKE_MATCH_1}")
        string(REPLACE "," ";" _sl "${_sl}")
        foreach(_s IN LISTS _sl)
            string(STRIP "${_s}" _s)
            if(NOT _s STREQUAL "")
                list(APPEND _list "${_s}")
            endif()
        endforeach()
    endif()
    set(${out_var} "${_list}" PARENT_SCOPE)
endfunction()

# Rewrite the [build].include array in <toml> to absolute paths anchored at
# the REAL PROJECT_DIR. Depth-sensitive sibling-relative entries
# ("../../../topo-lang-cpp/...", "../../runtime/...") would resolve to a
# different location from the scratch dir (which lives under the build
# tree), so every entry becomes absolute. Already-absolute entries pass
# through (get_filename_component ABSOLUTE is identity for absolute inputs).
# No-op for tomls without an include array (JVM benchmarks).
function(_topo_rewrite_include toml)
    _topo_include_list("${toml}" _incs)
    if(NOT _incs)
        return()
    endif()
    set(_abs "")
    foreach(_i IN LISTS _incs)
        get_filename_component(_abs_i "${PROJECT_DIR}/${_i}" ABSOLUTE)
        file(TO_CMAKE_PATH "${_abs_i}" _abs_i)
        if(_abs STREQUAL "")
            set(_abs "\"${_abs_i}\"")
        else()
            set(_abs "${_abs}, \"${_abs_i}\"")
        endif()
    endforeach()
    file(READ "${toml}" _txt)
    string(REGEX REPLACE "include[ \t]*=[ \t]*\\[[^]]*\\]"
        "include = [${_abs}]" _new "${_txt}")
    file(WRITE "${toml}" "${_new}")
endfunction()

# (Re)create the private scratch copy of the project with depth-sensitive
# includes rewritten to absolute paths.
function(_topo_prepare_scratch)
    file(REMOVE_RECURSE "${_scratch_dir}")
    file(MAKE_DIRECTORY "${_scratch_dir}")
    foreach(_sub src topo include)
        if(IS_DIRECTORY "${PROJECT_DIR}/${_sub}")
            file(COPY "${PROJECT_DIR}/${_sub}" DESTINATION "${_scratch_dir}")
        endif()
    endforeach()
    foreach(_toml Topo.toml Topo-base.toml Topo-forced.toml)
        if(EXISTS "${PROJECT_DIR}/${_toml}")
            file(COPY "${PROJECT_DIR}/${_toml}" DESTINATION "${_scratch_dir}")
        endif()
    endforeach()
    _topo_rewrite_include("${_scratch_dir}/Topo.toml")
    foreach(_alt Topo-base.toml Topo-forced.toml)
        if(EXISTS "${_scratch_dir}/${_alt}")
            _topo_rewrite_include("${_scratch_dir}/${_alt}")
        endif()
    endforeach()
    # Pristine copy of the (rewritten) default toml. Each variant swap
    # overwrites scratch/Topo.toml; the NEXT variant restores from here so
    # it builds the right config (the auto variant needs the default back).
    configure_file("${_scratch_dir}/Topo.toml" "${_scratch_dir}/Topo.toml.default" COPYONLY)
endfunction()

# Copy the build outputs produced in the scratch dir back into the real
# project tree (the e2e harness fast path + equivalence cases look for the
# pre-built binaries and dumped .ll there). Topo*.toml files, the scratch's
# .topo-cache and its source dirs are NOT copied — the real Topo.toml is
# never touched by this driver. Root-level build outputs are copied as
# files; `build/` (vanilla-style layout, JVM jars) is copied as a whole.
function(_topo_copy_outputs_back)
    file(GLOB _scratch_root_items "${_scratch_dir}/*")
    foreach(_item IN LISTS _scratch_root_items)
        if(IS_DIRECTORY "${_item}")
            continue()
        endif()
        get_filename_component(_fname "${_item}" NAME)
        # Skip every scratch toml artefact: Topo.toml, Topo.toml.default,
        # Topo-base.toml, Topo-forced.toml (the scratch copies carry the
        # absolute-include rewrite and must never land in the real tree).
        if(_fname MATCHES "^Topo.*toml")
            continue()
        endif()
        file(COPY "${_item}" DESTINATION "${PROJECT_DIR}")
    endforeach()
    if(IS_DIRECTORY "${_scratch_dir}/build")
        file(COPY "${_scratch_dir}/build" DESTINATION "${PROJECT_DIR}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Helper: run a single topo-build invocation in <workdir>.
# Fails the driver (and thus the custom_command) on non-zero exit.
# ---------------------------------------------------------------------------
function(_topo_run_build label workdir)
    message(STATUS "[topo-bench-artifacts] ${PROJECT_DIR}: ${label}")
    # Clean .topo-cache so swapped toml doesn't reuse stale artefacts.
    file(REMOVE_RECURSE "${workdir}/.topo-cache")
    # --no-check: benchmark projects are codegen-coverage fixtures, not
    # check-clean declaration sets; conformance has its own checker suites.
    execute_process(
        COMMAND "${TOPO_BUILD_EXE}" --dump-ir --no-check
        WORKING_DIRECTORY "${workdir}"
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
# Helper: swap the SCRATCH Topo.toml <- <variant>.toml, build in scratch,
# copy the produced outputs back into the real project dir.
#
# The swap happens on the scratch copy only; the committed Topo.toml is
# never written, so no save/restore around the real file is needed — the
# scratch dir is deleted wholesale at the end of the driver run (or at the
# next prepare, after an interrupted run).
# ---------------------------------------------------------------------------
function(_topo_build_variant label variant_toml)
    if(NOT EXISTS "${variant_toml}")
        message(STATUS
            "[topo-bench-artifacts] ${PROJECT_DIR}: skip ${label} (${variant_toml} not present)")
        return()
    endif()

    # Scratch-side swap. No failure-restore path exists by design: any
    # failure below propagates via FATAL_ERROR and the scratch dir is
    # simply left for the next invocation's _topo_prepare_scratch to remove.
    configure_file("${variant_toml}" "${_scratch_dir}/Topo.toml" COPYONLY)

    _topo_run_build("${label}" "${_scratch_dir}")
    _topo_copy_outputs_back()

    # Restore the default toml so the NEXT variant (in particular the auto
    # variant, which must build the committed config as-is) does not build
    # with this variant's swap still in place.
    configure_file("${_scratch_dir}/Topo.toml.default" "${_scratch_dir}/Topo.toml" COPYONLY)
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
    # Windows ships javac.exe/jar.exe; the exists-check must probe both
    # spellings (the .exe-less probe silently FATALed every windows project
    # in the first real timing-lane run — issue
    # jvm-bench-artifacts-windows-javac-exe).
    set(_javac "${JAVA_HOME}/bin/javac")
    set(_jar   "${JAVA_HOME}/bin/jar")
    if(NOT EXISTS "${_javac}" AND EXISTS "${_javac}.exe")
        set(_javac "${_javac}.exe")
    endif()
    if(NOT EXISTS "${_jar}" AND EXISTS "${_jar}.exe")
        set(_jar "${_jar}.exe")
    endif()
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

# Guard against leftover .prebuild-saved from a prior crashed run of the
# OLD in-place-swap driver. The scratch driver never creates this file, but
# a pre-upgrade interruption may have left one — restoring it keeps the
# tree self-healing.
if(EXISTS "${_saved_toml}")
    message(WARNING
        "[topo-bench-artifacts] ${PROJECT_DIR}: found stale ${_saved_toml}; "
        "restoring it as Topo.toml before rebuild")
    configure_file("${_saved_toml}" "${_topo_toml}" COPYONLY)
    file(REMOVE "${_saved_toml}")
endif()

# Prepare the private scratch copy (copies src/topo/include + tomls,
# rewrites depth-sensitive includes to absolute paths).
_topo_prepare_scratch()

if(BUILD_VANILLA)
    _topo_vanilla_cpp()
endif()
if(BUILD_VANILLA_JAVA)
    _topo_vanilla_java()
endif()

_topo_build_variant("topo base"   "${_scratch_dir}/Topo-base.toml")

# auto: the scratch Topo.toml as-is (copy of the real Topo.toml).
_topo_run_build("topo auto" "${_scratch_dir}")
_topo_copy_outputs_back()

_topo_build_variant("topo forced" "${_scratch_dir}/Topo-forced.toml")

# Drop the scratch copy. It lives under the build dir (never in the source
# tree), but removing it eagerly keeps the build tree tidy and guarantees a
# later configure-time glob can never see it.
file(REMOVE_RECURSE "${_scratch_dir}")

# All variants succeeded — mark the stamp. Use file(TOUCH) so mtime is
# updated even when the file already exists.
get_filename_component(_stamp_dir "${STAMP_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${_stamp_dir}")
file(TOUCH "${STAMP_FILE}")
message(STATUS "[topo-bench-artifacts] ${PROJECT_DIR}: OK -> ${STAMP_FILE}")
