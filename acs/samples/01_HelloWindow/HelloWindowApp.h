// SPDX-License-Identifier: Apache-2.0
// HelloWindow — ウィンドウを開いて背景色で塗るだけの最小サンプル。
//
// 学習ポイント:
//   ・acs::CApplication を継承して OnStart / OnUpdate / OnRender を上書き
//   ・CInput::IsKeyPressed / IsKeyDown でキー入力を取る
//   ・SetClearColor で背景色を切り替える
//   ・GetWindow().SetTitle で実行時にタイトルバーを更新
#pragma once

#include "app/Application.h"
#include "foundation/Types.h"

namespace hellowin {

class CHelloWindowApp : public acs::CApplication {
public:
    void OnStart()           noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()          noexcept override;
    void OnShutdown()        noexcept override;
    void OnEvent(const acs::FEvent& e) noexcept override;

private:
    acs::f32 m_R = 0.1f;
    acs::f32 m_G = 0.12f;
    acs::f32 m_B = 0.16f;
};

} // namespace hellowin
