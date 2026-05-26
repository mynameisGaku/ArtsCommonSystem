// SPDX-License-Identifier: Apache-2.0
// HelloText — TTF フォント + UTF-8 テキスト描画サンプル。
//
// 学習ポイント:
//   ・Font::LoadFromFile で TTF を読み込みアトラス焼き
//   ・SpriteBatch::DrawString で UTF-8 文字列を描画
//   ・Font::MeasureWidth でテキスト幅を取って中央寄せ
//
// 注: Windows 標準フォント (meiryo.ttc) を使うので、別 OS では別パス指定が必要
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
    acs::Font        _title_font, _body_font, _small_font;
    acs::f32         _time = 0.0f;
};

} // namespace hellotext
