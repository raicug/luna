cmake_minimum_required(VERSION 3.20)

project(Luna VERSION 0.1.0 LANGUAGES C CXX)

option(LUNA_BUILD_TESTING "Build the luna test suite" ON)
option(LUNA_BUILD_APP "Build the smoke application" ON)
option(LUNA_BUILD_BENCH "Build the performance gates" ON)

# The demo uses the Win32 and Direct3D 11 ImGui backends, so it only exists on
# Windows. Everywhere else there is nothing to switch on.
if(WIN32)
    option(LUNA_BUILD_DEMO "Build the interactive ImGui demonstration" ON)
else()
    option(LUNA_BUILD_DEMO "Build the interactive ImGui demonstration" OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

include(cmake/dependencies.cmake)

add_library(luna STATIC
    src/state.cpp
    src/model.cpp
    src/registry.cpp
    src/userdata.cpp
    src/convert.cpp
    src/docs.cpp
    src/tasklib.cpp
    src/signals.cpp
)

add_library(luna::luna ALIAS luna)

target_include_directories(luna
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_features(luna PUBLIC cxx_std_20)

# luna's public headers name lua_State, so Luau is an interface dependency: a
# consumer needs the include directories but never names a Luau type itself.
# The headers are exported as SYSTEM, which keeps /W4 and -Wall pointed at
# luna's own code.
target_link_libraries(luna PUBLIC Luau.VM Luau.Compiler)
target_include_directories(luna SYSTEM
    PRIVATE ${LUNA_LUAU_INCLUDE_DIRS}
    INTERFACE ${LUNA_LUAU_INCLUDE_DIRS}
)

set_target_properties(luna PROPERTIES
    CXX_EXTENSIONS OFF
    FOLDER luna
)

if(MSVC)
    target_compile_options(luna PRIVATE /W4 /EHsc /GR-)
else()
    target_compile_options(luna PRIVATE -Wall -Wextra -fno-rtti)
endif()

if(LUNA_BUILD_APP)
    add_executable(LunaApp app/src/main.cpp)
    target_link_libraries(LunaApp PRIVATE luna::luna)
    target_compile_features(LunaApp PRIVATE cxx_std_20)
    set_target_properties(LunaApp PROPERTIES CXX_EXTENSIONS OFF FOLDER luna)
endif()

if(LUNA_BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

if(LUNA_BUILD_BENCH)
    add_executable(LunaBench bench/bench.cpp)
    target_link_libraries(LunaBench PRIVATE luna::luna)
    target_compile_features(LunaBench PRIVATE cxx_std_20)
    set_target_properties(LunaBench PROPERTIES CXX_EXTENSIONS OFF FOLDER luna)
    # Benchmarks are only meaningful with inlining intact.
    if(MSVC)
        # /O2 conflicts with the Debug runtime checks, so it is left off there.
        target_compile_options(LunaBench PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2 /Ob2>)
        target_link_options(LunaBench PRIVATE /OPT:REF /OPT:ICF)
    else()
        target_compile_options(LunaBench PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O2>)
    endif()
endif()

if(LUNA_BUILD_DEMO)
    if(NOT WIN32)
        message(FATAL_ERROR
            "LUNA_BUILD_DEMO uses the Win32 and Direct3D 11 backends and is "
            "Windows-only")
    endif()

    include(cmake/imgui.cmake)

    # The headless self-test first: it is the target CI can run, and it shares
    # the harness with the windowed build so the two cannot drift.
    add_executable(LunaDemoSelfTest
        demo/src/harness.cpp
        demo/src/selftest.cpp
    )
    target_link_libraries(LunaDemoSelfTest PRIVATE luna::luna luna::imgui)
    target_include_directories(LunaDemoSelfTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/demo/src")
    target_compile_definitions(LunaDemoSelfTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_features(LunaDemoSelfTest PRIVATE cxx_std_20)
    set_target_properties(LunaDemoSelfTest PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER luna)

    add_executable(LunaDemo WIN32
        demo/src/main.cpp
        demo/src/harness.cpp
    )
    target_link_libraries(LunaDemo PRIVATE
        luna::luna
        luna::imgui
        luna::imgui_backends
    )
    target_include_directories(LunaDemo PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/demo/src")
    target_compile_definitions(LunaDemo PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
    target_compile_features(LunaDemo PRIVATE cxx_std_20)
    set_target_properties(LunaDemo PROPERTIES CXX_EXTENSIONS OFF FOLDER luna)

    if(MSVC)
        target_compile_options(LunaDemo PRIVATE /W4 /EHsc)
        target_compile_options(LunaDemoSelfTest PRIVATE /W4 /EHsc)
    endif()

    if(LUNA_BUILD_TESTING)
        add_test(NAME luna_demo COMMAND LunaDemoSelfTest)
    endif()
endif()
