add_library(LunaTestSupport INTERFACE)
add_library(Luna::TestSupport ALIAS LunaTestSupport)
target_compile_features(LunaTestSupport INTERFACE cxx_std_20)
target_link_libraries(LunaTestSupport INTERFACE Luna::Luna)
set_target_properties(LunaTestSupport PROPERTIES FOLDER "luna/tests")

add_library(LunaPropertyTestSupport INTERFACE)
add_library(Luna::PropertyTestSupport ALIAS LunaPropertyTestSupport)
target_compile_features(LunaPropertyTestSupport INTERFACE cxx_std_20)
target_link_libraries(LunaPropertyTestSupport INTERFACE
    Luna::TestSupport
    rapidcheck
)
set_target_properties(LunaPropertyTestSupport PROPERTIES FOLDER "luna/tests")

add_subdirectory(support/state)

set(LUNA_PUBLIC_INCLUDE_DIRECTORY "${PROJECT_SOURCE_DIR}/include")
file(GLOB_RECURSE LUNA_PUBLIC_HEADERS_FOR_COMPILE_TEST
    CONFIGURE_DEPENDS
    RELATIVE "${LUNA_PUBLIC_INCLUDE_DIRECTORY}"
    "${LUNA_PUBLIC_INCLUDE_DIRECTORY}/luna/*.hpp"
)
list(SORT LUNA_PUBLIC_HEADERS_FOR_COMPILE_TEST)

set(LUNA_STANDALONE_PUBLIC_HEADER_SOURCES)
foreach(LUNA_PUBLIC_HEADER IN LISTS LUNA_PUBLIC_HEADERS_FOR_COMPILE_TEST)
    string(REGEX REPLACE "\\.hpp$" ".cpp"
        LUNA_STANDALONE_RELATIVE_SOURCE "${LUNA_PUBLIC_HEADER}"
    )
    set(LUNA_STANDALONE_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/compile/standalone/${LUNA_STANDALONE_RELATIVE_SOURCE}"
    )
    if(NOT EXISTS "${LUNA_STANDALONE_SOURCE}")
        message(FATAL_ERROR
            "Missing standalone compile test for ${LUNA_PUBLIC_HEADER}: ${LUNA_STANDALONE_SOURCE}"
        )
    endif()
    list(APPEND LUNA_STANDALONE_PUBLIC_HEADER_SOURCES
        "${LUNA_STANDALONE_SOURCE}"
    )
endforeach()

add_library(LunaStandalonePublicHeaderCompileChecks OBJECT
    ${LUNA_STANDALONE_PUBLIC_HEADER_SOURCES}
)
target_compile_features(LunaStandalonePublicHeaderCompileChecks PRIVATE cxx_std_20)
target_include_directories(LunaStandalonePublicHeaderCompileChecks PRIVATE
    "${LUNA_PUBLIC_INCLUDE_DIRECTORY}"
)
set_target_properties(LunaStandalonePublicHeaderCompileChecks PROPERTIES
    CXX_EXTENSIONS OFF
    FOLDER "luna/tests/compile/standalone"
)

add_library(LunaConsumerBoundaryCompileCheck OBJECT
    compile/consumer/boundary.cpp
    compile/consumer/freeze.cpp
    compile/consumer/generation.cpp
    compile/consumer/lifecycle.cpp
)
target_compile_features(LunaConsumerBoundaryCompileCheck PRIVATE cxx_std_20)
target_include_directories(LunaConsumerBoundaryCompileCheck PRIVATE
    "${LUNA_PUBLIC_INCLUDE_DIRECTORY}"
)
set_target_properties(LunaConsumerBoundaryCompileCheck PROPERTIES
    CXX_EXTENSIONS OFF
    FOLDER "luna/tests/compile/consumer"
)

set(LUNA_TEST_CATEGORY_DIRECTORIES
    unit
    property
    integration
    fault
    generation
)

set(LUNA_TEST_SOURCES)
foreach(LUNA_TEST_CATEGORY IN LISTS LUNA_TEST_CATEGORY_DIRECTORIES)
    file(GLOB_RECURSE LUNA_TEST_CATEGORY_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${LUNA_TEST_CATEGORY}/*.cpp"
    )
    list(APPEND LUNA_TEST_SOURCES ${LUNA_TEST_CATEGORY_SOURCES})
endforeach()
list(REMOVE_DUPLICATES LUNA_TEST_SOURCES)
if(NOT LUNA_TEST_SOURCES)
    message(FATAL_ERROR
        "No correctness test sources were discovered under tests/"
    )
endif()

set(LUNA_GENERATION_GOLDEN_DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/generation/golden"
)

add_executable(LunaTests
    src/main.cpp
    ${LUNA_TEST_SOURCES}
    $<TARGET_OBJECTS:LunaStandalonePublicHeaderCompileChecks>
    $<TARGET_OBJECTS:LunaConsumerBoundaryCompileCheck>
)
target_compile_features(LunaTests PRIVATE cxx_std_20)
target_compile_definitions(LunaTests PRIVATE
    "LUNA_GENERATION_GOLDEN_DIRECTORY=\"${LUNA_GENERATION_GOLDEN_DIRECTORY}\""
)
target_link_libraries(LunaTests PRIVATE
    Luna::PropertyTestSupport
    Luna::StateTestSupport
)
set_target_properties(LunaTests PROPERTIES
    CXX_EXTENSIONS OFF
    FOLDER "luna/tests"
)

set(LUNA_CXX20_TARGETS
    Luna
    LunaTestApp
    LunaStandalonePublicHeaderCompileChecks
    LunaConsumerBoundaryCompileCheck
    LunaTests
)
if(TARGET LunaImGuiDemo)
    list(APPEND LUNA_CXX20_TARGETS LunaImGuiDemo)
endif()
foreach(LUNA_CXX20_TARGET IN LISTS LUNA_CXX20_TARGETS)
    get_target_property(LUNA_COMPILE_FEATURES
        ${LUNA_CXX20_TARGET} COMPILE_FEATURES
    )
    if(NOT "cxx_std_20" IN_LIST LUNA_COMPILE_FEATURES)
        message(FATAL_ERROR
            "${LUNA_CXX20_TARGET} must explicitly compile as C++20"
        )
    endif()

    get_target_property(LUNA_CXX_EXTENSIONS
        ${LUNA_CXX20_TARGET} CXX_EXTENSIONS
    )
    if(NOT LUNA_CXX_EXTENSIONS STREQUAL "OFF")
        message(FATAL_ERROR
            "${LUNA_CXX20_TARGET} must disable C++ extensions"
        )
    endif()
endforeach()

get_target_property(LUNA_PRIVATE_LINK_LIBRARIES Luna LINK_LIBRARIES)
get_target_property(LUNA_INTERFACE_LINK_LIBRARIES Luna INTERFACE_LINK_LIBRARIES)
foreach(LUNA_LUAU_TARGET IN ITEMS Luau.VM Luau.Compiler)
    if(NOT LUNA_LUAU_TARGET IN_LIST LUNA_PRIVATE_LINK_LIBRARIES)
        message(FATAL_ERROR "Luna must link ${LUNA_LUAU_TARGET} privately")
    endif()
    if(LUNA_LUAU_TARGET IN_LIST LUNA_INTERFACE_LINK_LIBRARIES)
        message(FATAL_ERROR
            "Luna must not expose ${LUNA_LUAU_TARGET} as a public link dependency"
        )
    endif()
    set(LUNA_LINK_ONLY_DEPENDENCY "$<LINK_ONLY:${LUNA_LUAU_TARGET}>")
    if(NOT LUNA_LINK_ONLY_DEPENDENCY IN_LIST LUNA_INTERFACE_LINK_LIBRARIES)
        message(FATAL_ERROR
            "Luna's static-library interface must keep ${LUNA_LUAU_TARGET} link-only"
        )
    endif()
endforeach()

get_target_property(LUNA_APP_LINK_LIBRARIES LunaTestApp LINK_LIBRARIES)
if(NOT LUNA_APP_LINK_LIBRARIES STREQUAL "Luna::Luna")
    message(FATAL_ERROR "LunaTestApp must link only to Luna::Luna")
endif()

foreach(LUNA_BOUNDARY_TARGET IN ITEMS
        LunaStandalonePublicHeaderCompileChecks
        LunaConsumerBoundaryCompileCheck)
    get_target_property(LUNA_BOUNDARY_INCLUDE_DIRECTORIES
        ${LUNA_BOUNDARY_TARGET} INCLUDE_DIRECTORIES
    )
    if(NOT LUNA_BOUNDARY_INCLUDE_DIRECTORIES STREQUAL
       "${LUNA_PUBLIC_INCLUDE_DIRECTORY}")
        message(FATAL_ERROR
            "${LUNA_BOUNDARY_TARGET} must compile with only Luna's public include path"
        )
    endif()
endforeach()
get_target_property(LUNA_CONSUMER_LINK_LIBRARIES
    LunaConsumerBoundaryCompileCheck LINK_LIBRARIES
)
if(LUNA_CONSUMER_LINK_LIBRARIES)
    message(FATAL_ERROR
        "The consumer boundary compile check must not link a Luau dependency"
    )
endif()
get_target_property(LUNA_CONSUMER_COMPILE_DEFINITIONS
    LunaConsumerBoundaryCompileCheck COMPILE_DEFINITIONS
)
if(LUNA_CONSUMER_COMPILE_DEFINITIONS)
    message(FATAL_ERROR
        "The consumer boundary compile check must not require a consumer macro"
    )
endif()

get_target_property(LUNA_REGISTERED_PRODUCTION_FILES Luna SOURCES)
set(LUNA_EXPECTED_PRODUCTION_FILES ${LUNA_HEADERS} ${LUNA_SOURCES})
list(SORT LUNA_REGISTERED_PRODUCTION_FILES)
list(SORT LUNA_EXPECTED_PRODUCTION_FILES)
if(NOT LUNA_REGISTERED_PRODUCTION_FILES STREQUAL LUNA_EXPECTED_PRODUCTION_FILES)
    message(FATAL_ERROR
        "Luna must register every public header and private implementation source"
    )
endif()

function(luna_collect_directory_targets DIRECTORY_PATH OUTPUT_VARIABLE)
    get_property(LUNA_LOCAL_TARGETS
        DIRECTORY "${DIRECTORY_PATH}" PROPERTY BUILDSYSTEM_TARGETS
    )
    set(LUNA_COLLECTED_TARGETS ${LUNA_LOCAL_TARGETS})
    get_property(LUNA_CHILD_DIRECTORIES
        DIRECTORY "${DIRECTORY_PATH}" PROPERTY SUBDIRECTORIES
    )
    foreach(LUNA_CHILD_DIRECTORY IN LISTS LUNA_CHILD_DIRECTORIES)
        luna_collect_directory_targets(
            "${LUNA_CHILD_DIRECTORY}" LUNA_CHILD_TARGETS
        )
        list(APPEND LUNA_COLLECTED_TARGETS ${LUNA_CHILD_TARGETS})
    endforeach()
    set(${OUTPUT_VARIABLE} "${LUNA_COLLECTED_TARGETS}" PARENT_SCOPE)
endfunction()

luna_collect_directory_targets("${CMAKE_CURRENT_SOURCE_DIR}" LUNA_ALL_TEST_TARGETS)
set(LUNA_TEST_EXECUTABLES)
foreach(LUNA_TEST_TARGET IN LISTS LUNA_ALL_TEST_TARGETS)
    get_target_property(LUNA_TEST_TARGET_TYPE ${LUNA_TEST_TARGET} TYPE)
    if(LUNA_TEST_TARGET_TYPE STREQUAL "EXECUTABLE")
        list(APPEND LUNA_TEST_EXECUTABLES ${LUNA_TEST_TARGET})
    endif()
endforeach()
if(NOT LUNA_TEST_EXECUTABLES STREQUAL "LunaTests")
    message(FATAL_ERROR
        "Tests must use exactly one unified LunaTests executable; found: ${LUNA_TEST_EXECUTABLES}"
    )
endif()

add_test(NAME LunaTests COMMAND LunaTests)
set_tests_properties(LunaTests PROPERTIES
    LABELS "unit;property;compile;integration;fault;generation"
    ENVIRONMENT "RC_PARAMS=max_success=100 verbose_shrinking=1"
)
get_test_property(LunaTests ENVIRONMENT LUNA_TEST_ENVIRONMENT)

add_test(NAME LunaBuildPolicy
    COMMAND "${CMAKE_COMMAND}"
        -P "${CMAKE_CURRENT_BINARY_DIR}/verify_build_policy.cmake"
)
set_tests_properties(LunaBuildPolicy PROPERTIES LABELS "build;policy")

add_test(NAME LunaTestApp.Build
    COMMAND "${CMAKE_COMMAND}"
        --build "${PROJECT_BINARY_DIR}"
        --target LunaTestApp
)
set_tests_properties(LunaTestApp.Build PROPERTIES
    FIXTURES_SETUP LunaTestAppBuilt
    LABELS "build;smoke"
    RUN_SERIAL TRUE
)

add_test(NAME LunaTestApp COMMAND "$<TARGET_FILE:LunaTestApp>")
set_tests_properties(LunaTestApp PROPERTIES
    FIXTURES_REQUIRED LunaTestAppBuilt
    LABELS "smoke"
)

function(luna_collect_directory_tests DIRECTORY_PATH OUTPUT_VARIABLE)
    get_property(LUNA_LOCAL_TESTS
        DIRECTORY "${DIRECTORY_PATH}" PROPERTY TESTS
    )
    set(LUNA_COLLECTED_TESTS ${LUNA_LOCAL_TESTS})
    get_property(LUNA_CHILD_DIRECTORIES
        DIRECTORY "${DIRECTORY_PATH}" PROPERTY SUBDIRECTORIES
    )
    foreach(LUNA_CHILD_DIRECTORY IN LISTS LUNA_CHILD_DIRECTORIES)
        luna_collect_directory_tests(
            "${LUNA_CHILD_DIRECTORY}" LUNA_CHILD_TESTS
        )
        list(APPEND LUNA_COLLECTED_TESTS ${LUNA_CHILD_TESTS})
    endforeach()
    set(${OUTPUT_VARIABLE} "${LUNA_COLLECTED_TESTS}" PARENT_SCOPE)
endfunction()

luna_collect_directory_tests("${PROJECT_SOURCE_DIR}" LUNA_REGISTERED_TESTS)
set(LUNA_ALLOWED_TESTS
    LunaTests
    LunaBuildPolicy
    LunaTestApp.Build
    LunaTestApp
)
foreach(LUNA_REQUIRED_TEST IN LISTS LUNA_ALLOWED_TESTS)
    if(NOT LUNA_REQUIRED_TEST IN_LIST LUNA_REGISTERED_TESTS)
        message(FATAL_ERROR
            "CTest must keep registering ${LUNA_REQUIRED_TEST}"
        )
    endif()
endforeach()
foreach(LUNA_REGISTERED_TEST IN LISTS LUNA_REGISTERED_TESTS)
    if(LUNA_REGISTERED_TEST IN_LIST LUNA_ALLOWED_TESTS)
        continue()
    endif()
    if(LUNA_ENABLE_BENCHMARK_REGRESSION_TESTS
       AND LUNA_REGISTERED_TEST MATCHES "^LunaBenchmarkRegression\\.")
        continue()
    endif()
    message(FATAL_ERROR
        "CTest registered an unexpected test: ${LUNA_REGISTERED_TEST}; "
        "interactive demos must stay outside CTest and benchmark targets stay "
        "separate unless LUNA_ENABLE_BENCHMARK_REGRESSION_TESTS is enabled"
    )
endforeach()

foreach(LUNA_NON_CTEST_TARGET IN ITEMS LunaImGuiDemo)
    if(TARGET ${LUNA_NON_CTEST_TARGET}
       AND ${LUNA_NON_CTEST_TARGET} IN_LIST LUNA_REGISTERED_TESTS)
        message(FATAL_ERROR
            "${LUNA_NON_CTEST_TARGET} must never be registered with CTest"
        )
    endif()
endforeach()

configure_file(
    cmake/verify_build_policy.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/verify_build_policy.cmake"
    @ONLY
)
