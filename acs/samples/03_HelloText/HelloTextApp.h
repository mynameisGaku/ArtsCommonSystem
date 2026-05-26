// SPDX-License-Identifier: Apache-2.0
// HelloText — TTF フォント + UTF-8 テキスト描画サンプル。
//
// 学習ポイント:
//   ・Sample::TryLoadDefaultUIFont で OS 標準フォントを読み込みアトラス焼き
//   ・SpriteBatch::DrawString で UTF-8 文字列を描画
//   ・Font::MeasureWidth でテキスト幅を取って中央寄せ
//
// 注: OS 標準フォント (Windows: meiryo.ttc, Mac: ヒラギノ等) に依存。
//     見つからない環境では Sample::TryLoadDefaultUIFont が Err を返す。
#pragma once

#include "app/Application.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

namespace hellotext {

class HelloTextApp : public acs::Application {
public:
    void OnStart()            noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()           noexcept override;
    void OnShutdown()         noexcept override;

private:
    acs::SpriteBatch _batch;
    acs::Font        _title_font;
    acs::Font        _body_font;
    acs::Font        _small_font;
    acs::f32         _time = 0.0f;
};

} // namespace hellotext
