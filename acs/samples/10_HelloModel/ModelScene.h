// SPDX-License-Identifier: Apache-2.0
// HelloModel — シーン本体。
// プリミティブ (球 / プレーン / キューブ) を保持し、カメラ入力 +
// FStandardShader を使った描画を担当する。
//
// FApplication 派生はリソース所有 (FStandardShader / 非同期ロード) を担当し、
// 毎フレームの update / render を ModelScene に委譲する。
#pragma once

#include "Types.h"

#include "asset/AssetFuture.h"
#include "render/StandardShader.h"
#include "render/RenderAssets.h"
#include "render/IRhiCommandList.h"

#include "math/Camera.h"
#include "math/Vec.h"

namespace hellomodel {

class ModelScene {
public:
    // プリミティブを GPU にアップロードしてカメラを構成する。
    // dev は OnStart の `GetRenderer().Device()` を渡す。失敗時 false。
    bool Init(acs::IRhiDevice& dev, acs::f32 aspect) noexcept;

    void Shutdown() noexcept;

    // カメラ入力 + 自転 + 非同期ロードのポーリング。
    void Update(acs::f32 dt, acs::FAssetFuture& async_mesh, bool& async_loaded) noexcept;

    // FStandardShader で全オブジェクトを描画。
    void Render(acs::FStandardShader& shader, acs::IRhiCommandList& cl) noexcept;

private:
    acs::FGpuMesh _gm_sphere, _gm_plane, _gm_cube;

    acs::FCamera _camera;
    acs::FVec3   _cam_pos{0, 1.5f, -5.0f};
    acs::f32    _cam_yaw   = 0.0f;
    acs::f32    _cam_pitch = 0.0f;
    acs::f32    _angle     = 0.0f;
};

} // namespace hellomodel
