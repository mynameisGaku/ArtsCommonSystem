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
    if (Input::IsKeyPressed(EKey::U)) app.m_UseAutoExposure = !app.m_UseAutoExposure;

    if (app.m_UseAutoExposure) {
        // 露出は GPU が実測輝度から算出。Q/E は目標平均輝度 (key) を動かして
        // 全体の明暗を補正する (EV compensation 相当)。
        if (Input::IsKeyDown(EKey::E)) app.m_AutoKey += dt * 0.3f;
        if (Input::IsKeyDown(EKey::Q)) app.m_AutoKey -= dt * 0.3f;
        if (app.m_AutoKey < 0.1f) app.m_AutoKey = 0.1f;
        if (app.m_AutoKey > 2.0f) app.m_AutoKey = 2.0f;
        app.m_PostParams.auto_exposure_enabled = true;
        app.m_PostParams.auto_exposure_key     = app.m_AutoKey;
        app.m_PostParams.exposure              = 1.0f;   // 露出は GPU 側で適用済み
    } else {
        // 手動の露出目標 + CPU eye adaptation。Q/E で目標を動かし、
        // m_AdaptedExposure が dt 補間で追従する。
        if (Input::IsKeyDown(EKey::E)) app.m_ExposureTarget += dt * 0.5f;
        if (Input::IsKeyDown(EKey::Q)) app.m_ExposureTarget -= dt * 0.5f;
        if (app.m_ExposureTarget < 0.1f) app.m_ExposureTarget = 0.1f;
        if (app.m_ExposureTarget > 4.0f) app.m_ExposureTarget = 4.0f;
        f32 k = dt * 2.5f;
        if (k > 1.0f) k = 1.0f;
        app.m_AdaptedExposure += (app.m_ExposureTarget - app.m_AdaptedExposure) * k;
        app.m_PostParams.auto_exposure_enabled = false;
        app.m_PostParams.exposure              = app.m_AdaptedExposure;
    }
    // auto-exposure の eye adaptation 補間に使うフレーム時間を渡す
    app.m_PostParams.delta_time = dt;
}

} // namespace helloibl
