# Per-project RESOURCE_LOCK injection for split e2e gtest suites.
#
# Background: e2e binaries operate directly on the source-tree project
# directories under benchmarks/<project>/ — they mutate Topo.toml (swap
# RAII) and .topo-cache during each TEST_F. Two TEST_F on the SAME project
# must serialise; two TEST_F on DIFFERENT projects do not race and can run
# in parallel. The legacy single-binary registration used a global lock
# "topo_e2e_build" which serialised everything; per-project locks recover
# the cross-project parallelism.
#
# Usage:
#   topo_inject_e2e_project_locks(
#       SOURCE       <TEST_F source file>
#       FIXTURE      <gtest fixture class name>
#       PREFIX       <lock-name prefix, e.g. topo_e2e_jvm>
#       BUILD_CALLS  <name1> <name2> ...     # helpers like topoBuild(...) where project is the first string-literal arg
#       [ASSERT_CALLS <name1> <name2> ...]   # helpers like assertBaseEquivalence(this, "project", ...) where project is the FIRST string-literal arg after `this,`
#   )
#
# For each TEST_F(<FIXTURE>, <Name>) in <SOURCE>, the helper scans the body
# for the FIRST match of any BUILD_CALL or ASSERT_CALL pattern and takes
# the captured string as the project name. The ctest entry
# <FIXTURE>.<Name> gets RESOURCE_LOCK "<PREFIX>_<project>".
# Cases without any matching call get a unique self-lock
# "<PREFIX>_unscoped_<Name>" — no parallelism gain, no race.
#
# Runs at configure time; emits a fragment that ctest loads after
# gtest_discover_tests via TEST_INCLUDE_FILES.

function(topo_inject_e2e_project_locks)
    cmake_parse_arguments(ARG "" "SOURCE;FIXTURE;PREFIX" "BUILD_CALLS;ASSERT_CALLS" ${ARGN})
    foreach(_required IN ITEMS SOURCE FIXTURE PREFIX)
        if(NOT ARG_${_required})
            message(FATAL_ERROR "topo_inject_e2e_project_locks: ${_required} is required")
        endif()
    endforeach()
    if(NOT ARG_BUILD_CALLS AND NOT ARG_ASSERT_CALLS)
        message(FATAL_ERROR
            "topo_inject_e2e_project_locks: at least one of BUILD_CALLS or "
            "ASSERT_CALLS must be provided")
    endif()

    # Build per-call regex list. BUILD_CALLS match `<name>("project")`;
    # ASSERT_CALLS match `<name>(this, "project"` (any whitespace).
    # In every case the project is captured as CMAKE_MATCH_1.
    set(_patterns "")
    foreach(_call IN LISTS ARG_BUILD_CALLS)
        # Don't anchor on the closing `)` — the call may take additional
        # args after the project (e.g. assertPassFired("proj", "out", "PassName")).
        list(APPEND _patterns "${_call}\\([ \t\n]*\"([A-Za-z_][A-Za-z0-9_]*)\"")
    endforeach()
    foreach(_call IN LISTS ARG_ASSERT_CALLS)
        list(APPEND _patterns "${_call}\\(this,[ \t\n]*\"([A-Za-z_][A-Za-z0-9_]*)\"")
    endforeach()

    file(STRINGS "${ARG_SOURCE}" _lines)

    set(_entries "")
    set(_seen_names "")
    set(_in_body FALSE)
    set(_depth 0)
    set(_cur_name "")
    set(_cur_body "")
    # Non-empty while we are inside a C++ raw string R"TAG(...)TAG" that
    # opened on a previous line. Brace counting must skip raw-string body
    # content — JS/JSON code embedded as raw strings often has unbalanced
    # `{`/`}` w.r.t. the C++ surroundings and would otherwise corrupt the
    # depth counter and silently swallow subsequent TEST_F entries.
    set(_raw_tag "")

    set(_open_marker_regex "^[ \t]*TEST_F\\(${ARG_FIXTURE},[ \t]*([A-Za-z_][A-Za-z0-9_]*)\\)")

    foreach(_line IN LISTS _lines)
        if(NOT _in_body)
            if(_line MATCHES "${_open_marker_regex}")
                set(_cur_name "${CMAKE_MATCH_1}")
                set(_cur_body "")
                set(_depth 0)
                set(_raw_tag "")
                set(_in_body TRUE)
            else()
                continue()
            endif()
        endif()

        # We are tracking a TEST_F body. Append the line (regex search later
        # only cares about the helper-call shape, not the brace structure).
        set(_cur_body "${_cur_body}\n${_line}")

        # Build a brace-counting version of the line with C++ string
        # literals stripped:
        #   1. If we are mid-way through a multi-line raw string, drop
        #      everything up to and including its `)TAG"` terminator on
        #      this line (or drop the whole line if no terminator here).
        #   2. Iteratively strip same-line raw strings R"TAG(...)TAG".
        #   3. If a raw string opens but doesn't close, enter raw mode
        #      and drop the opener-to-EOL.
        #   4. Strip ordinary "..." literals so a line like
        #      `f << "{broken"` doesn't leak `{` into the counter.
        set(_count_part "${_line}")
        if(_raw_tag)
            string(FIND "${_count_part}" ")${_raw_tag}\"" _close_idx)
            if(_close_idx GREATER_EQUAL 0)
                string(LENGTH ")${_raw_tag}\"" _close_len)
                math(EXPR _after "${_close_idx} + ${_close_len}")
                string(SUBSTRING "${_count_part}" ${_after} -1 _count_part)
                set(_raw_tag "")
            else()
                set(_count_part "")
            endif()
        endif()
        # Same-line raw strings. CMake regex lacks non-greedy match, so
        # use FIND-based slicing in a loop.
        set(_keep_scanning TRUE)
        while(_keep_scanning)
            set(_keep_scanning FALSE)
            if(_count_part MATCHES "R\"([A-Za-z_]*)\\(")
                set(_tag "${CMAKE_MATCH_1}")
                string(FIND "${_count_part}" "R\"${_tag}(" _open_idx)
                string(FIND "${_count_part}" ")${_tag}\"" _close_idx)
                if(_close_idx GREATER _open_idx)
                    string(LENGTH ")${_tag}\"" _close_len)
                    math(EXPR _after "${_close_idx} + ${_close_len}")
                    string(SUBSTRING "${_count_part}" 0 ${_open_idx} _pre)
                    string(SUBSTRING "${_count_part}" ${_after} -1 _post)
                    set(_count_part "${_pre}${_post}")
                    set(_keep_scanning TRUE)
                else()
                    string(SUBSTRING "${_count_part}" 0 ${_open_idx} _count_part)
                    set(_raw_tag "${_tag}")
                endif()
            endif()
        endwhile()
        string(REGEX REPLACE "\"[^\"]*\"" "" _line_no_strings "${_count_part}")
        # Also strip single-quoted C++ char literals — bodies that compare
        # against literal braces ('{' / '}') for text-scanning logic would
        # otherwise leak unbalanced braces into the depth counter.
        string(REGEX REPLACE "'[^']*'" "" _line_no_strings "${_line_no_strings}")
        string(REGEX REPLACE "[^{]" "" _opens "${_line_no_strings}")
        string(REGEX REPLACE "[^}]" "" _closes "${_line_no_strings}")
        string(LENGTH "${_opens}" _opens_n)
        string(LENGTH "${_closes}" _closes_n)
        math(EXPR _depth "${_depth} + ${_opens_n} - ${_closes_n}")

        if(_depth LESS_EQUAL 0 AND _opens_n GREATER 0)
            _topo_finalise_test_entry()
            set(_in_body FALSE)
        elseif(_depth EQUAL 0 AND _opens_n EQUAL 0)
            _topo_finalise_test_entry()
            set(_in_body FALSE)
        endif()
    endforeach()

    if(NOT _entries)
        message(WARNING
            "topo_inject_e2e_project_locks: no TEST_F(${ARG_FIXTURE}, ...) "
            "found in ${ARG_SOURCE}")
        return()
    endif()

    get_filename_component(_src_name "${ARG_SOURCE}" NAME_WE)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/${_src_name}_project_locks.cmake")
    set(_body "# Generated by topo_inject_e2e_project_locks — do not edit\n")
    foreach(_e IN LISTS _entries)
        string(APPEND _body "${_e}\n")
    endforeach()
    file(WRITE "${_out}" "${_body}")

    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${_out}")
endfunction()

# Helper macro (macro so it sees the caller's loop variables) — finds the
# first BUILD/ASSERT pattern match in _cur_body and appends a
# set_tests_properties line to _entries.
macro(_topo_finalise_test_entry)
    set(_project "")
    foreach(_pat IN LISTS _patterns)
        if(_cur_body MATCHES "${_pat}")
            set(_project "${CMAKE_MATCH_1}")
            break()
        endif()
    endforeach()
    if(_project)
        set(_lock "${ARG_PREFIX}_${_project}")
    else()
        set(_lock "${ARG_PREFIX}_unscoped_${_cur_name}")
    endif()
    set(_test_name "${ARG_FIXTURE}.${_cur_name}")
    if(NOT "${_test_name}" IN_LIST _seen_names)
        list(APPEND _seen_names "${_test_name}")
        list(APPEND _entries
            "set_tests_properties([=[${_test_name}]=] PROPERTIES RESOURCE_LOCK [=[${_lock}]=])")
    endif()
endmacro()
