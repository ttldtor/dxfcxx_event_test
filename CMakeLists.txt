cmake_minimum_required(VERSION 4.2)
project(dxfcxx_event_test)

set(CMAKE_CXX_STANDARD 23)

add_executable(${PROJECT_NAME} main.cpp)

include(FetchContent)

FetchContent_Declare(dxFeedGraalCxxApi
        GIT_REPOSITORY "https://github.com/dxFeed/dxfeed-graal-cxx-api.git"
        GIT_TAG v7.0.0
)
FetchContent_MakeAvailable(dxFeedGraalCxxApi)

#target_compile_definitions(${PROJECT_NAME} PRIVATE DXFCPP_USE_DLLS)
#target_link_libraries(${PROJECT_NAME} dxfcxx)
target_link_libraries(${PROJECT_NAME} dxfcxx::static)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:dxfcxx::graal>
#        $<TARGET_FILE:dxfcxx>
        $<TARGET_FILE_DIR:${PROJECT_NAME}>)

