cmake_minimum_required(VERSION 3.20)
project(LunaPackageConsumer CXX)

# Mirrors what a consumer does: bring Luau in, then find the installed luna.
set(LUNA_LUAU_SOURCE "" CACHE PATH "Luau source tree")
set(LUAU_BUILD_CLI OFF CACHE BOOL "")
set(LUAU_BUILD_TESTS OFF CACHE BOOL "")
add_subdirectory("${LUNA_LUAU_SOURCE}" _luau EXCLUDE_FROM_ALL)

find_package(luna REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE luna::luna)
