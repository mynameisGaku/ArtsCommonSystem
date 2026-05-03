include(FetchContent)

# ImGui を取得（公式リポジトリから docking ブランチ）
FetchContent_Declare(
    imgui_src
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui_src)

# ImGui 本体を静的ライブラリとして構築
# 公式リポジトリは CMakeLists を持たないため自前で add_library
if(NOT TARGET imgui)
    add_library(imgui STATIC
        ${imgui_src_SOURCE_DIR}/imgui.cpp
        ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_src_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_src_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_src_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    )
    target_include_directories(imgui PUBLIC
        ${imgui_src_SOURCE_DIR}
        ${imgui_src_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui PUBLIC d3d12 dxgi)
    if(MSVC)
        # ImGui 自体は ACS の厳格な警告レベルに合わないので緩める
        target_compile_options(imgui PRIVATE /W3 /wd4244 /wd4267)
    endif()
endif()

acs_module(
    NAME    Imgui
    TYPE    Runtime
    SOURCES
        ImGuiContext.cpp
    HEADERS
        ImGuiContext.h
    PUBLIC_DEPS
        Foundation
        Memory
        Container
        Platform
        Render
    LINK_PUBLIC
        imgui
)

acs_module_feature(MODULE Imgui NAME DEMO_WINDOW
    DEFINE IMGUI_DEMO
    DESCRIPTION "Include the ImGui demo window code"
    DEFAULT ON)
