// SPDX-License-Identifier: Apache-2.0
// Real ONNX Runtime backend for acs::game::IMlRuntime.
#pragma once

#include "gameframework/MlRuntime.h"

namespace acs::mlonnx {

class FOnnxMlRuntime final : public acs::game::IMlRuntime {
public:
    FOnnxMlRuntime() noexcept;
    ~FOnnxMlRuntime() noexcept override;

    acs::TResult<void> Init() noexcept override;
    void Shutdown() noexcept override;

    acs::TResult<acs::game::MlModelHandle> LoadModel(const char* ModelPath) noexcept override;
    acs::TResult<void> UnloadModel(acs::game::MlModelHandle h) noexcept override;

    acs::TResult<void> RunInference(acs::game::MlModelHandle h,
                                    const acs::f32* Inputs, acs::u32 InCount,
                                    acs::f32* Outputs, acs::u32 OutCount) noexcept override;

private:
    struct FImpl;
    FImpl* m_Impl = nullptr;
};

// =============================================================================
// 既定 MlRuntime 結線ヘルパ (gameframework の provider へ FOnnxMlRuntime を登録)
// -----------------------------------------------------------------------------
// アプリ起動時に一度だけ `InstallOnnxAsDefault()` を呼ぶと、以降
// `acs::game::GetDefaultMlRuntime()` が本物の ONNX Runtime backend (プロセス共有
// singleton) を返すようになる。これにより上位コードは backend 非依存に
// `GetDefaultMlRuntime()` だけで実 runtime を取得できる (`#if WITH_ACS_ML_ONNX`
// は この Install 呼び出し 1 箇所だけで済む)。
// =============================================================================

// プロセス共有の既定 FOnnxMlRuntime singleton を返す (provider の実体)。
acs::game::IMlRuntime& GetDefaultOnnxMlRuntime() noexcept;

// gameframework の MlRuntime provider に GetDefaultOnnxMlRuntime を登録する。
void InstallOnnxAsDefault() noexcept;

} // namespace acs::mlonnx
