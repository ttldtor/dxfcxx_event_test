# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(dxfcxx_event_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_custom_target(format
        COMMAND ${CMAKE_COMMAND} -DPROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR} -DMODE=FORMAT
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/format.cmake
        COMMENT "Formatting first-party C++ sources"
        VERBATIM)
add_custom_target(check-format
        COMMAND ${CMAKE_COMMAND} -DPROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR} -DMODE=CHECK
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/format.cmake
        COMMENT "Checking first-party C++ source formatting"
        VERBATIM)

include(FetchContent)

set(LATENCY_DXFCXX_RELEASE "v7.0.0" CACHE STRING "dxFeed Graal CXX API release used by the benchmark")
set_property(CACHE LATENCY_DXFCXX_RELEASE PROPERTY STRINGS v5.0.0 v7.0.0)

if(LATENCY_DXFCXX_RELEASE STREQUAL "v5.0.0")
    set(LATENCY_DXFCXX_ARCHIVE_SHA256 "7e903e48be8359df7665a30c1b9ac0d7a03431283065c9f60e520da2473f0ee2")
    set(LATENCY_QD_VERSION "3.342")
elseif(LATENCY_DXFCXX_RELEASE STREQUAL "v7.0.0")
    set(LATENCY_DXFCXX_ARCHIVE_SHA256 "7762ed3f95bf42a264060bee54dcc6fcc7c9d07fe71a59b547ec91c4407e0bc0")
    set(LATENCY_QD_VERSION "3.347")
else()
    message(FATAL_ERROR "Unsupported LATENCY_DXFCXX_RELEASE: ${LATENCY_DXFCXX_RELEASE}")
endif()

FetchContent_Declare(dxFeedGraalCxxApi
        URL "https://github.com/dxFeed/dxfeed-graal-cxx-api/archive/refs/tags/${LATENCY_DXFCXX_RELEASE}.tar.gz"
        URL_HASH SHA256=${LATENCY_DXFCXX_ARCHIVE_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(dxFeedGraalCxxApi)

configure_file(dxfeed.system.properties dxfeed.system.properties COPYONLY)

if(MSVC AND DXFCXX_LINK_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(latency_core src/core.cpp src/monitoring.cpp src/runner.cpp)
target_include_directories(latency_core PUBLIC include)
target_compile_definitions(latency_core PRIVATE
        LATENCY_CXX_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
        LATENCY_CXX_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
        LATENCY_DXFCXX_VERSION="${DXFCXX_VERSION}"
        LATENCY_DXFG_VERSION="${DXFEED_GRAAL_NATIVE_SDK_VERSION}"
        LATENCY_QD_VERSION="${LATENCY_QD_VERSION}")

add_executable(latency_server src/server.cpp)
target_link_libraries(latency_server PRIVATE latency_core dxfcxx::static)

add_executable(latency_client src/client.cpp)
target_link_libraries(latency_client PRIVATE latency_core dxfcxx::static)

add_executable(latency_analyzer src/analyzer.cpp)
target_link_libraries(latency_analyzer PRIVATE latency_core)

add_executable(latency_runner src/runner_main.cpp)
target_link_libraries(latency_runner PRIVATE latency_core)

foreach(target latency_server latency_client)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:dxfcxx::graal> $<TARGET_FILE_DIR:${target}>)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_BINARY_DIR}/dxfeed.system.properties $<TARGET_FILE_DIR:${target}>)
endforeach()

if(APPLE)
    set_target_properties(latency_server latency_client PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
    set_target_properties(latency_server latency_client PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()

install(TARGETS latency_server latency_client latency_analyzer latency_runner RUNTIME DESTINATION bin)
install(FILES $<TARGET_FILE:dxfcxx::graal> DESTINATION bin)
install(FILES dxfeed.system.properties DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    FetchContent_Declare(doctest
            URL "https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz"
            URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(doctest)

    add_executable(latency_tests tests/core_tests.cpp)
    target_compile_definitions(latency_tests PRIVATE
            LATENCY_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/monitoring-run")
    target_link_libraries(latency_tests PRIVATE latency_core dxfcxx::static doctest::doctest_with_main)
    add_custom_command(TARGET latency_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:dxfcxx::graal> $<TARGET_FILE_DIR:latency_tests>)
    add_custom_command(TARGET latency_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_BINARY_DIR}/dxfeed.system.properties $<TARGET_FILE_DIR:latency_tests>)
    add_test(NAME latency_tests COMMAND latency_tests)
    add_test(NAME latency_analyzer_help COMMAND latency_analyzer --help)
    add_test(NAME latency_runner_help COMMAND latency_runner --help)
endif()
