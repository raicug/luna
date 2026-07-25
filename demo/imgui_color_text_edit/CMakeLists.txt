find_package(OpenGL REQUIRED)

add_executable(LunaImGuiDemo
    src/main.cpp
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
    "${imgui_color_text_edit_SOURCE_DIR}/TextEditor.cpp"
)

target_compile_features(LunaImGuiDemo PRIVATE cxx_std_20)
target_include_directories(LunaImGuiDemo PRIVATE
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
    "${imgui_color_text_edit_SOURCE_DIR}"
)
target_link_libraries(LunaImGuiDemo PRIVATE
    Luna::Luna
    glfw
    OpenGL::GL
)

set_target_properties(LunaImGuiDemo PROPERTIES
    CXX_EXTENSIONS OFF
    FOLDER "luna/demos"
)
