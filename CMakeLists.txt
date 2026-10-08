cmake_minimum_required(VERSION 3.16)
project(bpa_discord LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BPA_DISCORD_FETCH_DPP "Download and build D++ if no installed copy is found" ON)

# 1) Preinstalled D++ (vcpkg manifest from vcpkg.json, distro package, CMAKE_PREFIX_PATH, ...)
find_package(dpp CONFIG QUIET)

# 2) Otherwise build D++ ourselves: static on Linux/macOS (no libdpp runtime dependency),
#    a DLL on Windows (D++ has no static Windows build).
if(NOT dpp_FOUND)
    if(NOT BPA_DISCORD_FETCH_DPP)
        message(FATAL_ERROR "D++ not found. Install it (vcpkg install dpp), point CMAKE_PREFIX_PATH at it, "
                            "or enable -DBPA_DISCORD_FETCH_DPP=ON.")
    endif()
    message(STATUS "D++ not found; fetching it with FetchContent (requires OpenSSL)")
    find_package(OpenSSL REQUIRED)
    include(FetchContent)
    FetchContent_Declare(
        dpp
        GIT_REPOSITORY https://github.com/brainboxdotcc/DPP.git
        GIT_TAG        v10.1.3 # same version as Betrock++
        GIT_SHALLOW    TRUE
    )
    set(DPP_BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(BUILD_VOICE_SUPPORT OFF CACHE BOOL "" FORCE)
    set(_bpa_saved_shared ${BUILD_SHARED_LIBS})

    if(UNIX)
        # D++ built statically forces the *static* zlib/OpenSSL archives (.a). Distros ship those
        # without -fPIC, so they can't be linked into a shared module. Resolve the shared
        # libraries ourselves first and pin them in the cache; D++'s own lookups then keep them.
        find_package(ZLIB REQUIRED)
        foreach(_var ZLIB_LIBRARY ZLIB_INCLUDE_DIR OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY OPENSSL_INCLUDE_DIR)
            if(${_var})
                set(${_var} "${${_var}}" CACHE STRING "" FORCE)
            endif()
        endforeach()
        set(BUILD_SHARED_LIBS OFF)
    else()
        # D++ doesn't support static builds on Windows; build it as a DLL.
        set(BUILD_SHARED_LIBS ON)
    endif()

    FetchContent_MakeAvailable(dpp)
    set(BUILD_SHARED_LIBS ${_bpa_saved_shared})
endif()

add_library(bpa_discord MODULE bpa_discord.cpp)
if(TARGET dpp::dpp)
    target_link_libraries(bpa_discord PRIVATE dpp::dpp)
else()
    target_link_libraries(bpa_discord PRIVATE dpp)
endif()
target_compile_options(bpa_discord PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fvisibility=hidden>)
set_target_properties(bpa_discord PROPERTIES PREFIX "" BUILD_RPATH_USE_ORIGIN ON INSTALL_RPATH "$ORIGIN")
