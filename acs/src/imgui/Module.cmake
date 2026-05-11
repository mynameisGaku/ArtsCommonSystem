include(FetchContent)

# acs_imgui module は ImGuiContext で ImGui_ImplDX12_* を直叩きしているため
# DX12 raw backend が無効なビルドでは module 全体を skip する。
# Phase 30+ で Diligent backend 用の ImGui integration を追加したら、ここの
# ガードを「DX12 OR Diligent ImGui」に広げる予定。
if(NOT ACS_RENDER_DX12_RAW)
    message(STATUS "acs_imgui: ACS_RENDER_DX12_RAW=OFF のため module を skip")
    return()
endif()

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
# Backend (Win32 / DX12) は ACS_RENDER_DX12_RAW のときだけ追加。
# Diligent や別バックエンドの場合は ACS 側で別の Backend を組む想定。
if(NOT TARGET imgui)
    set(_imgui_sources
        ${imgui_src_SOURCE_DIR}/imgui.cpp
        ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_src_SOURCE_DIR}/imgui_demo.cpp
    )
    if(WIN32)
        list(APPEND _imgui_sources ${imgui_src_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
    endif()
    if(ACS_RENDER_DX12_RAW)
        list(APPEND _imgui_sources ${imgui_src_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
    endif()
    add_library(imgui STATIC ${_imgui_sources})
    target_include_directories(imgui PUBLIC
        ${imgui_src_SOURCE_DIR}
        ${imgui_src_SOURCE_DIR}/backends
    )
    if(ACS_RENDER_DX12_RAW)
        target_link_libraries(imgui PUBLIC d3d12 dxgi)
    endif()
    if(MSVC)
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
