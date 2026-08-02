// SPDX-License-Identifier: Apache-2.0
// HelloText — TTF フォント + UTF-8 テキスト描画サンプル。
//
// 学習ポイント:
//   ・FSample::TryLoadDefaultUIFont で OS 標準フォントを読み込みアトラス焼き
//   ・CSpriteBatch::DrawString で UTF-8 文字列を描画
//   ・FFont::MeasureWidth でテキスト幅を取って中央寄せ
//
// 注: OS 標準フォント (Windows: meiryo.ttc, Mac: ヒラギノ等) に依存。
//     見つからない環境では FSample::TryLoadDefaultUIFont が Err を返す。
#pragma once

#include "app/Application.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

namespace hellotext {

class CHelloTextApp : public acs::CApplication {
public:
    void OnStart()            noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()           noexcept override;
    void OnShutdown()         noexcept override;

private:
    acs::CSpriteBatch m_Batch;
    acs::FFont        m_TitleFont;
    acs::FFont        m_BodyFont;
    acs::FFont        m_SmallFont;
    acs::f32         m_Time = 0.0f;
};

} // namespace hellotext
