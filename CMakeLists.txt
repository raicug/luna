cmake_minimum_required(VERSION 3.25)

project(Luna VERSION 0.1.0 LANGUAGES CXX)

option(LUNA_BUILD_IMGUI_DEMO
    "Build the interactive ImGui Luau editor demo"
    OFF
)

option(LUNA_BUILD_BENCHMARKS
    "Build the separate Luna benchmark targets outside correctness CTest"
    OFF
)

option(LUNA_ENABLE_BENCHMARK_REGRESSION_TESTS
    "Register bounded Luna benchmark regression checks with CTest"
    OFF
)

option(LUNA_BUILD_RAW_LUAU_BASELINES
    "Build opt-in raw Luau C API benchmark baselines"
    OFF
)

if(LUNA_ENABLE_BENCHMARK_REGRESSION_TESTS AND NOT LUNA_BUILD_BENCHMARKS)
    message(FATAL_ERROR
        "LUNA_ENABLE_BENCHMARK_REGRESSION_TESTS requires LUNA_BUILD_BENCHMARKS"
    )
endif()

if(LUNA_BUILD_RAW_LUAU_BASELINES AND NOT LUNA_BUILD_BENCHMARKS)
    message(FATAL_ERROR
        "LUNA_BUILD_RAW_LUAU_BASELINES requires LUNA_BUILD_BENCHMARKS"
    )
endif()

include(CTest)
include(cmake/dependencies.cmake)

file(GLOB_RECURSE LUNA_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/luna/*.hpp"
)

file(GLOB_RECURSE LUNA_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_library(Luna STATIC
    ${LUNA_HEADERS}
    ${LUNA_SOURCES}
)

add_library(Luna::Luna ALIAS Luna)

target_compile_features(Luna PUBLIC cxx_std_20)

target_include_directories(Luna
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)

target_link_libraries(Luna
    PRIVATE
        Luau.VM
        Luau.Compiler
)

set_target_properties(Luna PROPERTIES
    CXX_EXTENSIONS OFF
    FOLDER luna
)

add_executable(LunaTestApp app/src/main.cpp)

target_compile_features(LunaTestApp PRIVATE cxx_std_20)
target_link_libraries(LunaTestApp PRIVATE Luna::Luna)

set_target_properties(LunaTestApp PROPERTIES
    CXX_EXTENSIONS OFF
    FOLDER luna
)

if(LUNA_BUILD_IMGUI_DEMO)
    add_subdirectory(demo/imgui_color_text_edit)
endif()

if(LUNA_BUILD_BENCHMARKS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CMakeLists.txt")
        message(FATAL_ERROR
            "LUNA_BUILD_BENCHMARKS is enabled but no benchmarks directory exists yet"
        )
    endif()
    add_subdirectory(benchmarks)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
