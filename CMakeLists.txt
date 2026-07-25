cmake_minimum_required(VERSION 3.25)

project(Luna VERSION 0.1.0 LANGUAGES CXX)

option(LUNA_BUILD_IMGUI_DEMO
    "Build the interactive ImGui Luau editor demo"
    OFF
)

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

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
