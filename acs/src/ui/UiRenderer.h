// SPDX-License-Identifier: Apache-2.0
// UiRenderer — Widget tree を SpriteBatch + Font で描画する
//
// 使い方:
//   UiRenderer ur;
//   ur.Init(*dev, GetRenderer().ColorFormat(), default_font);
//
//   // 毎フレーム:
//   StackPanel root;
//   /* root.Add<...>() で子を構築 */
//   root.Layout(0, 0, screen_w, screen_h);
//   ur.Render(root, *cmd, screen_w, screen_h);
//
// 仕組み:
//   ・SpriteBatch で矩形 + テクスチャ + 文字を発行
//   ・Font は ACS Font (TrueType + atlas)
//   ・Widget::Render(*this) を再帰的に呼ぶ
//   ・各 widget は描画ヘルパ (DrawRect / DrawText 等) で UiRenderer に依頼
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiTexture.h"
#include "ui/Widgets.h"

namespace acs {

class UiRenderer {
public:
    UiRenderer() noexcept = default;
    ~UiRenderer() noexcept = default;

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    Result<void> Init(IRhiDevice& device, Format rt_format, Font* default_font) noexcept;
    void Shutdown() noexcept;

    // 1 フレーム描画
    void Render(Widget& root, IRhiCommandList& cmd, u32 screen_w, u32 screen_h) noexcept;

    // ---- Widget の Render から呼ばれるプリミティブ ----
    void DrawRect(f32 x, f32 y, f32 w, f32 h, const Vec4& color) noexcept;
    void DrawRectOutline(f32 x, f32 y, f32 w, f32 h, const Vec4& color, f32 thickness = 1.0f) noexcept;
    void DrawText(const char* utf8, f32 x, f32 y, const Vec4& color) noexcept;

    // テーマ色 (Widgets が参照)
    const UiColors& Colors() const noexcept { return _colors; }
    UiColors&       Colors()       noexcept { return _colors; }

    Font* DefaultFont() const noexcept { return _font; }

private:
    SpriteBatch _batch;
    Font*       _font = nullptr;     // 所有しない
    UiColors    _colors;
    bool        _frame_open = false;
};

// ============================================================================
// UiInput — Window のマウス/キーイベントを Widget tree に配信する
// ============================================================================
class UiInput {
public:
    // 毎フレーム呼ぶ。Input モジュールのマウス位置 / クリック / 押下キーを取り出して
    // root の hit-test → 該当 widget の On* に dispatch。
    void Dispatch(Widget& root) noexcept;

private:
    Widget* _hovered  = nullptr;     // 直近 hover 中 widget
    Widget* _pressed  = nullptr;     // pointer-down 中 widget (drag 連続)
    Widget* _focused  = nullptr;     // フォーカス中 (TextInput)
};

} // namespace acs
