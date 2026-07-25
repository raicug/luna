include(FetchContent)

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    Luau
    GIT_REPOSITORY https://github.com/luau-lang/luau.git
    GIT_TAG 0.730
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(Luau)

if(BUILD_TESTING)
    set(RC_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(RC_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        RapidCheck
        GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
        GIT_TAG b2d9ed2dddefc4b84318d664b4f221eb792d89c7
    )

    FetchContent_MakeAvailable(RapidCheck)
endif()

if(LUNA_BUILD_IMGUI_DEMO)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG a74efa0d5628b74adc0426af4c5710e287fa7c2c
    )

    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG 8936b58fe26e8c3da834b8f60b06511d537b4c63
    )

    FetchContent_Declare(
        imgui_color_text_edit
        GIT_REPOSITORY https://github.com/BalazsJako/ImGuiColorTextEdit.git
        GIT_TAG ca2f9f1462e3b60e56351bc466acda448c5ea50d
    )

    FetchContent_MakeAvailable(glfw imgui imgui_color_text_edit)
endif()
