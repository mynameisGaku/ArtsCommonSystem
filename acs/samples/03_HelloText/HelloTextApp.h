// SPDX-License-Identifier: Apache-2.0
// HelloText — TTF フォント + UTF-8 テキスト描画サンプル。
//
// 学習ポイント:
//   ・Sample::TryLoadDefaultUIFont で OS 標準フォントを読み込みアトラス焼き
//   ・FSpriteBatch::DrawString で UTF-8 文字列を描画
//   ・FFont::MeasureWidth でテキスト幅を取って中央寄せ
//
// 注: OS 標準フォント (Windows: meiryo.ttc, Mac: ヒラギノ等) に依存。
//     見つからない環境では Sample::TryLoadDefaultUIFont が Err を返す。
#pragma once

#include "app/Application.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

namespace hellotext {

class HelloTextApp : public acs::FApplication {
public:
    void OnStart()            noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()           noexcept override;
    void OnShutdown()         noexcept override;

private:
    acs::FSpriteBatch _batch;
    acs::FFont        _title_font;
    acs::FFont        _body_font;
    acs::FFont        _small_font;
    acs::f32         _time = 0.0f;
};

} // namespace hellotext
