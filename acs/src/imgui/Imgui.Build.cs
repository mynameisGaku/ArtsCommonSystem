// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>
/// Imgui モジュール: ImGui (docking) を FetchContent で取得し静的 target として構築、
/// その上に薄い acs::ImGuiContext を載せる。ImGui_ImplDX12_* を直叩きするため DX12 backend
/// 専用 (DX12 OFF ビルドでは module ごと skip)。
/// </summary>
public sealed class Imgui : AcsModule
{
    public Imgui()
    {
        Type = ModuleType.Runtime;
        Guard = "ACS_RENDER_DX12_RAW";   // OFF のときは module を skip

        // ImGui 本体 (外部 target) を取得 + 構築する生 CMake。
        Preamble = @"include(FetchContent)

# ImGui を取得（公式リポジトリから docking ブランチ）
FetchContent_Declare(
    imgui_src
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui_src)
_acs_install_runtime_license(
    ""${imgui_src_SOURCE_DIR}/LICENSE.txt"" ""DearImGui-License.txt"")

# ImGui 本体を静的ライブラリとして構築 (公式リポジトリは CMakeLists を持たない)。
# Backend (Win32 / DX12) は ACS_RENDER_DX12_RAW のときだけ追加。
if(NOT TARGET imgui)
    set(_imgui_sources
        ${imgui_src_SOURCE_DIR}/imgui.cpp
        ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
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
endif()";

        PublicDeps.AddRange(new[] { "Foundation", "Memory", "Container", "Platform", "Render" });
        PublicLibs.Add("imgui");
    }
}
