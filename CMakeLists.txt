cmake_minimum_required(VERSION 3.21)
project(dxfcxx_event_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(dxFeedGraalCxxApi
        URL "https://github.com/dxFeed/dxfeed-graal-cxx-api/archive/refs/tags/v7.0.0.tar.gz"
        URL_HASH SHA256=7762ed3f95bf42a264060bee54dcc6fcc7c9d07fe71a59b547ec91c4407e0bc0
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(dxFeedGraalCxxApi)

if(MSVC AND DXFCXX_LINK_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(latency_core src/core.cpp src/monitoring.cpp)
target_include_directories(latency_core PUBLIC include)

add_executable(latency_server src/server.cpp)
target_link_libraries(latency_server PRIVATE latency_core dxfcxx::static)

add_executable(latency_client src/client.cpp)
target_link_libraries(latency_client PRIVATE latency_core dxfcxx::static)

add_executable(latency_analyzer src/analyzer.cpp)
target_link_libraries(latency_analyzer PRIVATE latency_core)

foreach(target latency_server latency_client)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:dxfcxx::graal> $<TARGET_FILE_DIR:${target}>)
endforeach()

if(APPLE)
    set_target_properties(latency_server latency_client PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
    set_target_properties(latency_server latency_client PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()

install(TARGETS latency_server latency_client latency_analyzer RUNTIME DESTINATION bin)
install(FILES $<TARGET_FILE:dxfcxx::graal> DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_executable(latency_tests tests/core_tests.cpp)
    target_link_libraries(latency_tests PRIVATE latency_core)
    add_test(NAME latency_tests COMMAND latency_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/monitoring-run)
    add_test(NAME latency_analyzer_help COMMAND latency_analyzer --help)
endif()
