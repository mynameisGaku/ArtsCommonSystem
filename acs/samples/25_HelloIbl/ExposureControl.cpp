// SPDX-License-Identifier: Apache-2.0
// HelloIbl — 露出 (exposure) 制御の実装。
//
// 2 つのモードを切替可能 ('U' キー):
//   auto モード: GPU が実測したシーン平均輝度から露出を算出 (eye adaptation も GPU 側)。
//                Q/E は EV compensation 相当の目標 key 値を動かす。
//   manual モード: CPU で 目標→実露出 を dt 補間 (~0.4 秒で順応)。Q/E は目標値を動かす。
#include "HelloIblApp.h"

#include "platform/Input.h"

using namespace acs;

namespace helloibl {

void UpdateExposureControls(HelloIblApp& app, f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::U)) app._use_auto_exposure = !app._use_auto_exposure;

    if (app._use_auto_exposure) {
        // 露出は GPU が実測輝度から算出。Q/E は目標平均輝度 (key) を動かして
        // 全体の明暗を補正する (EV compensation 相当)。
        if (Input::IsKeyDown(EKey::E)) app._auto_key += dt * 0.3f;
        if (Input::IsKeyDown(EKey::Q)) app._auto_key -= dt * 0.3f;
        if (app._auto_key < 0.1f) app._auto_key = 0.1f;
        if (app._auto_key > 2.0f) app._auto_key = 2.0f;
        app._post_params.auto_exposure_enabled = true;
        app._post_params.auto_exposure_key     = app._auto_key;
        app._post_params.exposure              = 1.0f;   // 露出は GPU 側で適用済み
    } else {
        // 手動の露出目標 + CPU eye adaptation。Q/E で目標を動かし、
        // _adapted_exposure が dt 補間で追従する。
        if (Input::IsKeyDown(EKey::E)) app._exposure_target += dt * 0.5f;
        if (Input::IsKeyDown(EKey::Q)) app._exposure_target -= dt * 0.5f;
        if (app._exposure_target < 0.1f) app._exposure_target = 0.1f;
        if (app._exposure_target > 4.0f) app._exposure_target = 4.0f;
        f32 k = dt * 2.5f;
        if (k > 1.0f) k = 1.0f;
        app._adapted_exposure += (app._exposure_target - app._adapted_exposure) * k;
        app._post_params.auto_exposure_enabled = false;
        app._post_params.exposure              = app._adapted_exposure;
    }
    // auto-exposure の eye adaptation 補間に使うフレーム時間を渡す
    app._post_params.delta_time = dt;
}

} // namespace helloibl
