// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>
/// Render モジュール: RHI 抽象 (IRhi*) + 共通レンダリング。Dx12 / Diligent バックエンドの
/// ソースとリンクは条件付き (Dx12/ Diligent/ サブディレクトリは条件側で追加、base からは除外)。
/// バックエンド内部ヘッダ (Dx12/*.h, Diligent/*.h) は HEADERS に出さない (base から除外される)。
/// </summary>
public sealed class Render : AcsModule
{
    public Render()
    {
        Type = ModuleType.Runtime;
        // stb (truetype) はフォントアトラス焼きで使う。Diligent は ON のときだけ fetch。
        Preamble = @"# stb (truetype) はフォントのアトラス焼きで使うので Render でも fetch する
acs_third_party_stb()

# トップレベルの ACS_RENDER_DILIGENT が ON のときだけ Diligent を fetch する。
if(ACS_RENDER_DILIGENT)
    acs_third_party_diligent()
endif()";

        PublicDeps.AddRange(new[] { "Foundation", "Memory", "Container", "Math", "Platform", "Asset" });
        // DX12 / Diligent ライブラリは PRIVATE にして外向きに leak させない。
        PrivateLibs.Add("acs_third_party::stb");

        When("ACS_RENDER_DX12_RAW")
            .SubdirSrc("Dx12")
            .LinkPrivate("d3d12", "dxgi", "d3dcompiler", "dxguid");

        When("ACS_RENDER_DILIGENT")
            .SubdirSrc("Diligent")
            .LinkPrivate("acs_third_party::diligent_core");

        // Live-object 診断用 Windows SDK ライブラリは Windows ビルドだけへ閉じる。
        When("ACS_RENDER_DILIGENT AND WIN32")
            .LinkPrivate("d3d12", "dxgi", "dxguid");

        Feature("DX12_RAW", "RENDER_DX12_RAW",
            desc: "Use raw DirectX 12 backend", defaultExpr: "${ACS_RENDER_DX12_RAW}");
        Feature("DILIGENT", "RENDER_DILIGENT",
            desc: "Use Diligent Engine as RHI backend (DX12/Vulkan/Metal cross-platform)",
            defaultExpr: "${ACS_RENDER_DILIGENT}");
        Feature("DILIGENT_VULKAN", "RENDER_DILIGENT_VULKAN",
            desc: "Build Diligent Vulkan backend (only meaningful with ACS_RENDER_DILIGENT=ON)",
            defaultExpr: "${ACS_DILIGENT_VULKAN}");
    }
}
