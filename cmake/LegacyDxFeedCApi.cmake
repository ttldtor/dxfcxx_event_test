# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0

include_guard(GLOBAL)

include(FetchContent)

set(LATENCY_DXFEED_C_API_VERSION "5.11.0")

# Adds the official dxFeed C API binary SDK as an imported shared-library target.
#
# The upstream source project changes directory-wide CMake state, so the comparison
# client consumes the pinned release archive instead. The resulting target is named
# `dxfeed-c-api::dxfeed-c-api`.
function(latency_add_legacy_dxfeed_c_api)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The legacy dxFeed C API comparison supports only 64-bit builds")
    endif()

    if(WIN32)
        set(archive_url
                "https://github.com/dxFeed/dxfeed-c-api/releases/download/${LATENCY_DXFEED_C_API_VERSION}/dxfeed-c-api-${LATENCY_DXFEED_C_API_VERSION}-windows-no-tls.zip")
        set(archive_sha256 "6745299bcb201071d40e231b3af1fc7011d6e4c0f194786ed15032997d45eed9")
    elseif(UNIX AND NOT APPLE)
        set(archive_url
                "https://github.com/dxFeed/dxfeed-c-api/releases/download/${LATENCY_DXFEED_C_API_VERSION}/dxfeed-c-api-${LATENCY_DXFEED_C_API_VERSION}-linux-no-tls.zip")
        set(archive_sha256 "6284249d376b996d459bcac30f353a740c4a46095de59ce68dd78bdf4c53623c")
    else()
        message(FATAL_ERROR "The legacy dxFeed C API comparison supports only Windows and Linux amd64")
    endif()

    FetchContent_Declare(latency_dxfeed_c_api
            URL "${archive_url}"
            URL_HASH SHA256=${archive_sha256}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            SOURCE_SUBDIR __latency_no_cmake_project)
    FetchContent_MakeAvailable(latency_dxfeed_c_api)

    set(sdk_root "${latency_dxfeed_c_api_SOURCE_DIR}")

    if(NOT EXISTS "${sdk_root}/include/DXFeed.h")
        set(sdk_root
                "${latency_dxfeed_c_api_SOURCE_DIR}/DXFeedAll-${LATENCY_DXFEED_C_API_VERSION}-x64-no-tls")
    endif()

    if(NOT EXISTS "${sdk_root}/include/DXFeed.h")
        message(FATAL_ERROR "The dxFeed C API ${LATENCY_DXFEED_C_API_VERSION} SDK layout is not recognized")
    endif()

    add_library(latency_dxfeed_c_api SHARED IMPORTED GLOBAL)
    set_target_properties(latency_dxfeed_c_api PROPERTIES
            IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
            INTERFACE_INCLUDE_DIRECTORIES "${sdk_root}/include"
            MAP_IMPORTED_CONFIG_MINSIZEREL Release
            MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)

    if(WIN32)
        set_target_properties(latency_dxfeed_c_api PROPERTIES
                IMPORTED_IMPLIB_DEBUG "${sdk_root}/bin/x64/DXFeedd_64.lib"
                IMPORTED_IMPLIB_RELEASE "${sdk_root}/bin/x64/DXFeed_64.lib"
                IMPORTED_LOCATION_DEBUG "${sdk_root}/bin/x64/DXFeedd_64.dll"
                IMPORTED_LOCATION_RELEASE "${sdk_root}/bin/x64/DXFeed_64.dll")
    else()
        set_target_properties(latency_dxfeed_c_api PROPERTIES
                IMPORTED_LOCATION_DEBUG "${sdk_root}/bin/x64/libDXFeedd_64.so"
                IMPORTED_LOCATION_RELEASE "${sdk_root}/bin/x64/libDXFeed_64.so")
    endif()

    foreach(configuration DEBUG RELEASE)
        get_target_property(runtime latency_dxfeed_c_api "IMPORTED_LOCATION_${configuration}")

        if(NOT EXISTS "${runtime}")
            message(FATAL_ERROR "Missing dxFeed C API runtime: ${runtime}")
        endif()
    endforeach()

    add_library(dxfeed-c-api::dxfeed-c-api ALIAS latency_dxfeed_c_api)
endfunction()
