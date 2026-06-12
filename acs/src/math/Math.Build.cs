// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>Math モジュール: ベクトル/行列/クォータニオン + SIMD ディスパッチ。</summary>
public sealed class Math : AcsModule
{
    public Math()
    {
        Type = ModuleType.Runtime;
        PublicDeps.AddRange(new[] { "Foundation", "Threading" });

        Feature("DIRECTXMATH", "MATH_DIRECTXMATH", def: true,
            desc: "Use DirectXMath as the SIMD backend (recommended on Windows)");
        Feature("AVX2", "MATH_AVX2", def: true,
            desc: "Compile AVX2 fast paths for batch math operations");
        Feature("RUNTIME_DISPATCH", "MATH_RUNTIME_DISPATCH", def: true,
            desc: "Use runtime CPU detection to select SIMD path (vs static)");
    }
}
