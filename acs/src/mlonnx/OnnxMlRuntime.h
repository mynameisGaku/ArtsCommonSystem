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

} // namespace acs::mlonnx
