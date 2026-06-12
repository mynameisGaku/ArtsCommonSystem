// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>
/// Mvvm モジュール: Observable / Binder / Command などの MVVM 基盤。ImGui アダプタ
/// (ImguiBindings) は ACS_MVVM_IMGUI_BINDINGS かつ DX12 backend のときだけ含める。
/// </summary>
public sealed class Mvvm : AcsModule
{
    public Mvvm()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[] { "Foundation", "Memory", "Container", "Math", "Threading" });

        // ImGui アダプタは条件付き。base からは除外しておく。
        ExcludeFiles.AddRange(new[] { "ImguiBindings.cpp", "ImguiBindings.h" });
        When("ACS_MVVM_IMGUI_BINDINGS AND ACS_RENDER_DX12_RAW")
            .Src("ImguiBindings.cpp")
            .Hdr("ImguiBindings.h")
            .Dep("Imgui");

        Feature("IMGUI_BINDINGS", "MVVM_IMGUI_BINDINGS",
            desc: "Provide ImGui-based binding helpers (acs::mvvm::imgui::Bind*)",
            defaultExpr: "${ACS_MVVM_IMGUI_BINDINGS}");
    }
}
