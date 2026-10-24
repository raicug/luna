add_executable(luna_tests tests.cpp)
target_link_libraries(luna_tests PRIVATE luna::luna)
target_compile_features(luna_tests PRIVATE cxx_std_20)
set_target_properties(luna_tests PROPERTIES CXX_EXTENSIONS OFF)

add_test(NAME luna_tests COMMAND luna_tests)

# Boundary check: every public header must compile on its own, and no public
# header may include a Luau header. This replaces Luna's 55 boundary test files
# with one translation unit and one configure-time check.
add_executable(luna_boundary boundary.cpp)
target_link_libraries(luna_boundary PRIVATE luna::luna)
target_compile_features(luna_boundary PRIVATE cxx_std_20)

add_test(NAME luna_boundary COMMAND luna_boundary)
