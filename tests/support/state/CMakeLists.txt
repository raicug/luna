add_library(LunaStateTestSupport INTERFACE)
add_library(Luna::StateTestSupport ALIAS LunaStateTestSupport)

target_compile_features(LunaStateTestSupport INTERFACE cxx_std_20)
target_include_directories(LunaStateTestSupport INTERFACE
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(LunaStateTestSupport INTERFACE Luna::TestSupport)

set_target_properties(LunaStateTestSupport PROPERTIES
    FOLDER "luna/tests/support/state"
)
