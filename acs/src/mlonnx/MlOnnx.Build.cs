// SPDX-License-Identifier: Apache-2.0
using Acs.Build;
namespace Acs.Modules;

/// <summary>MlOnnx モジュール: ONNX Runtime バックエンド (acs::game::IMlRuntime、gated)。</summary>
public sealed class MlOnnx : AcsModule
{
    public MlOnnx()
    {
        Type = ModuleType.Runtime;
        Guard = "ACS_BUILD_ML_ONNX";
        Preamble = @"include(ACSThirdParty)
acs_third_party_onnxruntime()";
        PublicDeps.AddRange(new[] { "Foundation", "GameFramework" });
        PublicLibs.Add("acs_third_party_onnxruntime");
    }
}
