# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED PROJECT_ROOT)
    get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

if(NOT DEFINED MODE OR NOT MODE MATCHES "^(FORMAT|CHECK)$")
    message(FATAL_ERROR "MODE must be FORMAT or CHECK")
endif()

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format-20 clang-format REQUIRED)
find_program(UNCRUSTIFY_EXECUTABLE NAMES uncrustify REQUIRED)

execute_process(
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" --version
        OUTPUT_VARIABLE CLANG_FORMAT_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)

if(NOT CLANG_FORMAT_VERSION MATCHES "20\\.1\\.8")
    message(FATAL_ERROR "clang-format 20.1.8 is required, found: ${CLANG_FORMAT_VERSION}")
endif()

execute_process(
        COMMAND "${UNCRUSTIFY_EXECUTABLE}" --version
        OUTPUT_VARIABLE UNCRUSTIFY_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)

if(NOT UNCRUSTIFY_VERSION MATCHES "0\\.83\\.0")
    message(FATAL_ERROR "Uncrustify 0.83.0 is required, found: ${UNCRUSTIFY_VERSION}")
endif()

file(GLOB_RECURSE FORMAT_SOURCES LIST_DIRECTORIES FALSE
        "${PROJECT_ROOT}/src/*.cpp"
        "${PROJECT_ROOT}/include/*.h"
        "${PROJECT_ROOT}/include/*.hpp"
        "${PROJECT_ROOT}/tests/*.cpp"
        "${PROJECT_ROOT}/tests/*.h"
        "${PROJECT_ROOT}/tests/*.hpp")
list(SORT FORMAT_SOURCES)

if(NOT FORMAT_SOURCES)
    message(FATAL_ERROR "No first-party C++ sources found")
endif()

set(CLANG_FORMAT_STYLE "-style=file:${PROJECT_ROOT}/.clang-format")

if(MODE STREQUAL "FORMAT")
    execute_process(
            COMMAND "${UNCRUSTIFY_EXECUTABLE}" --no-backup -q -c "${PROJECT_ROOT}/.uncrustify.cfg" ${FORMAT_SOURCES}
            COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
            COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i "${CLANG_FORMAT_STYLE}" ${FORMAT_SOURCES}
            COMMAND_ERROR_IS_FATAL ANY)
else()
    if(DEFINED ENV{TEMP})
        set(TEMP_ROOT "$ENV{TEMP}")
    elseif(DEFINED ENV{TMPDIR})
        set(TEMP_ROOT "$ENV{TMPDIR}")
    else()
        set(TEMP_ROOT "/tmp")
    endif()

    string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef CHECK_SUFFIX)
    set(CHECK_ROOT "${TEMP_ROOT}/dxfcxx-format-${CHECK_SUFFIX}")
    set(CHECK_SOURCES)

    foreach(SOURCE IN LISTS FORMAT_SOURCES)
        file(RELATIVE_PATH RELATIVE_SOURCE "${PROJECT_ROOT}" "${SOURCE}")
        set(CHECK_SOURCE "${CHECK_ROOT}/${RELATIVE_SOURCE}")
        get_filename_component(CHECK_DIRECTORY "${CHECK_SOURCE}" DIRECTORY)
        file(MAKE_DIRECTORY "${CHECK_DIRECTORY}")
        file(COPY_FILE "${SOURCE}" "${CHECK_SOURCE}")
        list(APPEND CHECK_SOURCES "${CHECK_SOURCE}")
    endforeach()

    execute_process(
            COMMAND "${UNCRUSTIFY_EXECUTABLE}" --no-backup -q -c "${PROJECT_ROOT}/.uncrustify.cfg" ${CHECK_SOURCES}
            RESULT_VARIABLE UNCRUSTIFY_RESULT)

    if(NOT UNCRUSTIFY_RESULT EQUAL 0)
        file(REMOVE_RECURSE "${CHECK_ROOT}")
        message(FATAL_ERROR "Uncrustify failed with exit code ${UNCRUSTIFY_RESULT}")
    endif()

    execute_process(
            COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i "${CLANG_FORMAT_STYLE}" ${CHECK_SOURCES}
            RESULT_VARIABLE CLANG_FORMAT_RESULT)

    if(NOT CLANG_FORMAT_RESULT EQUAL 0)
        file(REMOVE_RECURSE "${CHECK_ROOT}")
        message(FATAL_ERROR "clang-format failed with exit code ${CLANG_FORMAT_RESULT}")
    endif()

    set(CHANGED_SOURCES)
    list(LENGTH FORMAT_SOURCES SOURCE_COUNT)
    math(EXPR LAST_SOURCE_INDEX "${SOURCE_COUNT} - 1")

    foreach(SOURCE_INDEX RANGE ${LAST_SOURCE_INDEX})
        list(GET FORMAT_SOURCES ${SOURCE_INDEX} SOURCE)
        list(GET CHECK_SOURCES ${SOURCE_INDEX} CHECK_SOURCE)
        execute_process(
                COMMAND ${CMAKE_COMMAND} -E compare_files "${SOURCE}" "${CHECK_SOURCE}"
                RESULT_VARIABLE COMPARE_RESULT)

        if(NOT COMPARE_RESULT EQUAL 0)
            file(RELATIVE_PATH RELATIVE_SOURCE "${PROJECT_ROOT}" "${SOURCE}")
            list(APPEND CHANGED_SOURCES "${RELATIVE_SOURCE}")
        endif()
    endforeach()

    file(REMOVE_RECURSE "${CHECK_ROOT}")

    if(CHANGED_SOURCES)
        list(JOIN CHANGED_SOURCES "\n  " CHANGED_SOURCE_LIST)
        message(FATAL_ERROR "Formatting changes are required in:\n  ${CHANGED_SOURCE_LIST}")
    endif()
endif()
