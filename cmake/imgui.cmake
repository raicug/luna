# ImGui is only needed by the demo harness, so it is configured here rather than
# in dependencies.cmake: the library itself has no UI dependency.
#
# Two targets, because the split matters: `imgui` is the portable core, and
# `imgui_backends` adds the Win32 and Direct3D 11 platform layers. A headless
# self-test can link the core without dragging in a graphics API.
#
# Two sources are supported. A local checkout keeps the build offline; otherwise
# the pinned revision is fetched.

include(FetchContent)

set(LUNA_IMGUI_VERSION "v1.92.1" CACHE STRING "Pinned ImGui revision")
set(LUNA_IMGUI_SOURCE "" CACHE PATH "Existing ImGui checkout to build against")

if(TARGET imgui)
    return()
endif()

set(_luna_imgui_root "")

if(LUNA_IMGUI_SOURCE)
    if(NOT EXISTS "${LUNA_IMGUI_SOURCE}/imgui.cpp")
        message(FATAL_ERROR
            "LUNA_IMGUI_SOURCE is set to '${LUNA_IMGUI_SOURCE}' but there is no "
            "imgui.cpp there")
    endif()
    set(_luna_imgui_root "${LUNA_IMGUI_SOURCE}")
else()
    # ImGui has no CMakeLists.txt upstream, so FetchContent_MakeAvailable cannot
    # be used: it would try to add_subdirectory a directory that has no project.
    # The single-argument populate form declares and fetches in one step, which
    # is the supported route for a source-only dependency.
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()

    FetchContent_Populate(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG ${LUNA_IMGUI_VERSION}
        GIT_SHALLOW TRUE
    )
    set(_luna_imgui_root "${imgui_SOURCE_DIR}")
endif()

if(NOT EXISTS "${_luna_imgui_root}/imgui.cpp")
    message(FATAL_ERROR
        "ImGui sources were not found in '${_luna_imgui_root}'. Set "
        "LUNA_IMGUI_SOURCE to an ImGui checkout to build offline.")
endif()

# --- core: no platform code ------------------------------------------------

add_library(imgui STATIC
    "${_luna_imgui_root}/imgui.cpp"
    "${_luna_imgui_root}/imgui_draw.cpp"
    "${_luna_imgui_root}/imgui_tables.cpp"
    "${_luna_imgui_root}/imgui_widgets.cpp"
    "${_luna_imgui_root}/misc/cpp/imgui_stdlib.cpp"
)

add_library(luna::imgui ALIAS imgui)

# SYSTEM, so /W4 and -Wall stay pointed at luna's own code rather than at a
# third-party tree nobody here modifies.
target_include_directories(imgui SYSTEM PUBLIC
    "${_luna_imgui_root}"
    "${_luna_imgui_root}/misc/cpp"
)

target_compile_features(imgui PUBLIC cxx_std_17)
set_target_properties(imgui PROPERTIES CXX_EXTENSIONS OFF FOLDER luna)

# --- backends: Win32 + Direct3D 11 ----------------------------------------

add_library(imgui_backends STATIC
    "${_luna_imgui_root}/backends/imgui_impl_win32.cpp"
    "${_luna_imgui_root}/backends/imgui_impl_dx11.cpp"
)

add_library(luna::imgui_backends ALIAS imgui_backends)

target_link_libraries(imgui_backends PUBLIC imgui PRIVATE d3d11 dxgi)
target_include_directories(imgui_backends SYSTEM PUBLIC
    "${_luna_imgui_root}/backends"
)
target_compile_definitions(imgui_backends PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_features(imgui_backends PUBLIC cxx_std_17)
set_target_properties(imgui_backends PROPERTIES CXX_EXTENSIONS OFF FOLDER luna)
