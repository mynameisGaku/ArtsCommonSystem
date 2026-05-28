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

    acs::TResult<acs::game::MlModelHandle> LoadModel(const char* model_path) noexcept override;
    acs::TResult<void> UnloadModel(acs::game::MlModelHandle h) noexcept override;

    acs::TResult<void> RunInference(acs::game::MlModelHandle h,
                                    const acs::f32* inputs, acs::u32 in_count,
                                    acs::f32* outputs, acs::u32 out_count) noexcept override;

private:
    struct Impl;
    Impl* m_Impl = nullptr;
};

} // namespace acs::mlonnx
