cmake_minimum_required(VERSION 3.20)

project(Luna VERSION 0.3.0 LANGUAGES C CXX)

option(LUNA_BUILD_TESTING "Build the luna test suite" ON)
option(LUNA_BUILD_APP "Build the smoke application" ON)
option(LUNA_BUILD_BENCH "Build the performance gates" ON)
option(LUNA_BUNDLE_LUAU "Also produce one static library holding luna and Luau" ON)
option(LUNA_INSTALL "Generate install and export rules" ${PROJECT_IS_TOP_LEVEL})
set(LUNA_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined")

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

# Sanitizers apply to every target in the build, Luau included, so they agree
# on instrumentation. MSVC only offers address; request it with "address".
if(LUNA_SANITIZE)
    if(MSVC)
        if(LUNA_SANITIZE STREQUAL "address")
            add_compile_options(/fsanitize=address)
        else()
            message(FATAL_ERROR "MSVC supports LUNA_SANITIZE=address only")
        endif()
    else()
        add_compile_options(-fsanitize=${LUNA_SANITIZE} -fno-omit-frame-pointer)
        # luna builds with -fno-rtti, which the vptr check cannot see through.
        if(LUNA_SANITIZE MATCHES "undefined")
            add_compile_options(-fno-sanitize=vptr)
            add_link_options(-fno-sanitize=vptr)
        endif()
        add_link_options(-fsanitize=${LUNA_SANITIZE})
    endif()
endif()

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
    "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_features(luna PUBLIC cxx_std_20)

# luna's public headers name lua_State, so Luau is an interface dependency: a
# consumer needs the include directories but never names a Luau type itself.
# The headers are exported as SYSTEM, which keeps /W4 and -Wall pointed at
# luna's own code.
# Luau is not part of the export set, so the installed package links it back in
# from the consumer's own targets (see cmake/lunaConfig.cmake.in).
target_link_libraries(luna PUBLIC
    "$<BUILD_INTERFACE:Luau.VM>" "$<BUILD_INTERFACE:Luau.Compiler>")
target_include_directories(luna SYSTEM
    PRIVATE ${LUNA_LUAU_INCLUDE_DIRS}
    INTERFACE "$<BUILD_INTERFACE:${LUNA_LUAU_INCLUDE_DIRS}>"
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


# One archive with luna and every Luau library it needs, so a consumer links a
# single file and includes a single directory (the Luau headers the public
# headers use are installed next to luna's).
if(LUNA_BUNDLE_LUAU)
    set(_luna_bundle_targets luna)
    set(_luna_bundle_queue Luau.VM Luau.Compiler)
    while(_luna_bundle_queue)
        list(POP_FRONT _luna_bundle_queue _t)
        if(NOT TARGET ${_t} OR _t IN_LIST _luna_bundle_targets)
            continue()
        endif()
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "STATIC_LIBRARY")
            list(APPEND _luna_bundle_targets ${_t})
        endif()
        get_target_property(_deps ${_t} INTERFACE_LINK_LIBRARIES)
        if(_deps)
            foreach(_d ${_deps})
                if(TARGET ${_d})
                    list(APPEND _luna_bundle_queue ${_d})
                endif()
            endforeach()
        endif()
    endwhile()

    set(_luna_bundle_inputs "")
    foreach(_t ${_luna_bundle_targets})
        list(APPEND _luna_bundle_inputs "$<TARGET_FILE:${_t}>")
    endforeach()

    set(LUNA_BUNDLE_FILE
        "${CMAKE_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}luna_bundled${CMAKE_STATIC_LIBRARY_SUFFIX}")
    add_custom_command(
        OUTPUT "${LUNA_BUNDLE_FILE}"
        COMMAND ${CMAKE_COMMAND} "-DOUT=${LUNA_BUNDLE_FILE}" "-DAR=${CMAKE_AR}"
                "-DINPUTS=${_luna_bundle_inputs}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bundle.cmake"
        DEPENDS ${_luna_bundle_targets} cmake/bundle.cmake
        COMMENT "Bundling luna and Luau into one library"
        VERBATIM)
    add_custom_target(luna_bundle ALL DEPENDS "${LUNA_BUNDLE_FILE}")
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
    # Exits non-zero when a gate fails. Timing is meaningless in Debug or under
    # a sanitizer, so it only registers for optimised configurations.
    if(LUNA_BUILD_TESTING AND NOT LUNA_SANITIZE)
        add_test(NAME luna_bench
            COMMAND LunaBench --json "${CMAKE_BINARY_DIR}/bench-results.json"
            CONFIGURATIONS Release RelWithDebInfo MinSizeRel)
        set_tests_properties(luna_bench PROPERTIES LABELS bench)
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

if(LUNA_INSTALL)
    include(cmake/install.cmake)
endif()
