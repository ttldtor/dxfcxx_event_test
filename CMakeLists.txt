cmake_minimum_required(VERSION 4.2)
project(dxfcxx_event_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(dxFeedGraalCxxApi
        GIT_REPOSITORY "https://github.com/dxFeed/dxfeed-graal-cxx-api.git"
        GIT_TAG v7.0.0)
FetchContent_MakeAvailable(dxFeedGraalCxxApi)

add_library(latency_core src/core.cpp)
target_include_directories(latency_core PUBLIC include)

add_executable(latency_server src/server.cpp)
target_link_libraries(latency_server PRIVATE latency_core dxfcxx::static)

add_executable(latency_client src/client.cpp)
target_link_libraries(latency_client PRIVATE latency_core dxfcxx::static)

foreach(target latency_server latency_client)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:dxfcxx::graal> $<TARGET_FILE_DIR:${target}>)
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_executable(latency_tests tests/core_tests.cpp)
    target_link_libraries(latency_tests PRIVATE latency_core)
    add_test(NAME latency_tests COMMAND latency_tests)
endif()
