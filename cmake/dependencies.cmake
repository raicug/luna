include(FetchContent)

set(LUNA_LUAU_VERSION "0.730" CACHE STRING "Pinned Luau revision")

# Allow an already-populated Luau checkout to be used instead of a network
# fetch. This keeps offline builds working and lets a consumer share one copy.
set(LUNA_LUAU_SOURCE "" CACHE PATH "Existing Luau source tree to build against")

# Luau builds a CLI and a test harness by default; luna wants neither.
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_SHARED OFF CACHE BOOL "" FORCE)

if(LUNA_LUAU_SOURCE)
    if(NOT EXISTS "${LUNA_LUAU_SOURCE}/CMakeLists.txt")
        message(FATAL_ERROR
            "LUNA_LUAU_SOURCE is set to '${LUNA_LUAU_SOURCE}' but no "
            "Luau CMakeLists.txt exists there")
    endif()
    add_subdirectory("${LUNA_LUAU_SOURCE}" "${CMAKE_BINARY_DIR}/_luau"
                     EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(
        Luau
        GIT_REPOSITORY https://github.com/luau-lang/luau.git
        GIT_TAG ${LUNA_LUAU_VERSION}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Luau)
endif()

foreach(_luna_luau_target Luau.VM Luau.Compiler)
    if(NOT TARGET ${_luna_luau_target})
        message(FATAL_ERROR
            "${_luna_luau_target} was not created by the Luau build")
    endif()
    get_target_property(_luna_luau_type ${_luna_luau_target} TYPE)
    if(NOT _luna_luau_type STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR
            "${_luna_luau_target} is ${_luna_luau_type}; luna links Luau "
            "statically")
    endif()
endforeach()

# luna's public headers name lua_State, so every consumer needs Luau's include
# directories. They are exported as SYSTEM so /W4 and -Wall stay pointed at
# luna's own code.
get_target_property(_luna_luau_vm_include Luau.VM INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(_luna_luau_cc_include Luau.Compiler INTERFACE_INCLUDE_DIRECTORIES)

set(LUNA_LUAU_INCLUDE_DIRS "")
if(_luna_luau_vm_include)
    list(APPEND LUNA_LUAU_INCLUDE_DIRS ${_luna_luau_vm_include})
endif()
if(_luna_luau_cc_include)
    list(APPEND LUNA_LUAU_INCLUDE_DIRS ${_luna_luau_cc_include})
endif()
list(REMOVE_DUPLICATES LUNA_LUAU_INCLUDE_DIRS)
