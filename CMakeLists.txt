cmake_minimum_required(VERSION 3.16)
project(bpa_discord LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(dpp CONFIG REQUIRED)

add_library(bpa_discord MODULE bpa_discord.cpp)
target_link_libraries(bpa_discord PRIVATE dpp::dpp)
target_compile_options(bpa_discord PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fvisibility=hidden>)
set_target_properties(bpa_discord PROPERTIES PREFIX "" BUILD_RPATH_USE_ORIGIN ON INSTALL_RPATH "$ORIGIN")
