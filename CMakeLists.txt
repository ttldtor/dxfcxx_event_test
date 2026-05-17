cmake_minimum_required(VERSION 4.2)
project(dxfcxx_event_test)

set(CMAKE_CXX_STANDARD 23)

add_executable(dxfcxx_event_test main.cpp)

include(FetchContent)

FetchContent_Declare(dxFeedGraalCxxApi GIT_REPOSITORY "https://github.com/dxFeed/dxfeed-graal-cxx-api.git" GIT_TAG v5.0.0)
FetchContent_MakeAvailable(dxFeedGraalCxxApi)

target_link_libraries(dxfcxx_event_test dxfcxx)

add_custom_command(TARGET dxfcxx_event_test POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:dxfcxx::graal>
        $<TARGET_FILE:dxfcxx>
        $<TARGET_FILE_DIR:dxfcxx_event_test>)

