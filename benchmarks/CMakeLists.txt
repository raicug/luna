# Luna benchmarks.
#
# These targets exist only when LUNA_BUILD_BENCHMARKS is enabled, are separate
# executables from the single LunaTests correctness executable and the LunaTestApp
# smoke executable, and are not registered with CTest unless
# LUNA_ENABLE_BENCHMARK_REGRESSION_TESTS explicitly asks for bounded regression
# checks. A benchmark result is only evidence together with the configuration
# that produced it, so every target compiles that configuration in and every
# result line prints it.

get_property(LUNA_MULTI_CONFIG_GENERATOR GLOBAL
    PROPERTY GENERATOR_IS_MULTI_CONFIG
)
if(NOT LUNA_MULTI_CONFIG_GENERATOR AND NOT CMAKE_BUILD_TYPE)
    message(FATAL_ERROR
        "Luna benchmarks require an explicit CMAKE_BUILD_TYPE so every "
        "measurement records the build it was taken in"
    )
endif()

if(NOT LUNA_LUAU_VERSION)
    message(FATAL_ERROR
        "Luna benchmarks require a recorded Luau version; cmake/dependencies.cmake "
        "must define LUNA_LUAU_VERSION"
    )
endif()

math(EXPR LUNA_BENCHMARK_POINTER_BITS "${CMAKE_SIZEOF_VOID_P} * 8")
set(LUNA_BENCHMARK_COMPILER
    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
)
set(LUNA_BENCHMARK_ARCHITECTURE
    "${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}/${LUNA_BENCHMARK_POINTER_BITS}-bit"
)

add_library(LunaBenchmarkSupport STATIC support/harness.cpp)
add_library(Luna::BenchmarkSupport ALIAS LunaBenchmarkSupport)
target_compile_features(LunaBenchmarkSupport PUBLIC cxx_std_20)
target_include_directories(LunaBenchmarkSupport PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(LunaBenchmarkSupport PUBLIC
    "LUNA_BENCHMARK_BUILD_TYPE=\"$<CONFIG>\""
    "LUNA_BENCHMARK_COMPILER=\"${LUNA_BENCHMARK_COMPILER}\""
    "LUNA_BENCHMARK_ARCHITECTURE=\"${LUNA_BENCHMARK_ARCHITECTURE}\""
    "LUNA_BENCHMARK_LUAU_VERSION=\"${LUNA_LUAU_VERSION}\""
)
target_link_libraries(LunaBenchmarkSupport PUBLIC Luna::Luna)
set_target_properties(LunaBenchmarkSupport PROPERTIES
    CXX_EXTENSIONS OFF
    FOLDER "luna/benchmarks"
)

set(LUNA_BENCHMARK_TARGETS)

# One opt-in benchmark executable per measured area.
function(luna_add_benchmark BENCHMARK_NAME BENCHMARK_SOURCE)
    set(BENCHMARK_TARGET "LunaBenchmark${BENCHMARK_NAME}")
    add_executable(${BENCHMARK_TARGET} "${BENCHMARK_SOURCE}")
    target_compile_features(${BENCHMARK_TARGET} PRIVATE cxx_std_20)
    target_link_libraries(${BENCHMARK_TARGET} PRIVATE Luna::BenchmarkSupport)
    set_target_properties(${BENCHMARK_TARGET} PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER "luna/benchmarks"
    )

    if(LUNA_ENABLE_BENCHMARK_REGRESSION_TESTS)
        add_test(NAME "LunaBenchmarkRegression.${BENCHMARK_NAME}"
            COMMAND "$<TARGET_FILE:${BENCHMARK_TARGET}>"
                --warmup=1
                --samples=3
        )
        set_tests_properties("LunaBenchmarkRegression.${BENCHMARK_NAME}"
            PROPERTIES LABELS "benchmark;regression" RUN_SERIAL TRUE
        )
    endif()

    set(LUNA_BENCHMARK_TARGETS ${LUNA_BENCHMARK_TARGETS} ${BENCHMARK_TARGET}
        PARENT_SCOPE
    )
endfunction()

luna_add_benchmark(Registration src/registration.cpp)
luna_add_benchmark(Overload src/overload.cpp)
luna_add_benchmark(Conversion src/conversion.cpp)
luna_add_benchmark(Invocation src/invocation.cpp)
luna_add_benchmark(Reflection src/reflection.cpp)
luna_add_benchmark(Userdata src/userdata.cpp)

# Build every benchmark, without running anything.
add_custom_target(LunaBenchmarks DEPENDS ${LUNA_BENCHMARK_TARGETS})
set_target_properties(LunaBenchmarks PROPERTIES FOLDER "luna/benchmarks")

# Run every benchmark in one repeatable pass. This stays a build target rather
# than a CTest entry, so correctness runs never execute benchmarks.
set(LUNA_BENCHMARK_RUN_COMMANDS)
foreach(LUNA_BENCHMARK_TARGET IN LISTS LUNA_BENCHMARK_TARGETS)
    list(APPEND LUNA_BENCHMARK_RUN_COMMANDS
        COMMAND "$<TARGET_FILE:${LUNA_BENCHMARK_TARGET}>"
    )
endforeach()
add_custom_target(LunaBenchmarksRun
    ${LUNA_BENCHMARK_RUN_COMMANDS}
    DEPENDS ${LUNA_BENCHMARK_TARGETS}
    COMMENT "Running Luna benchmarks with their recorded configuration"
    VERBATIM
)
set_target_properties(LunaBenchmarksRun PROPERTIES FOLDER "luna/benchmarks")
