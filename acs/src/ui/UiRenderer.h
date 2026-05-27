// SPDX-License-Identifier: Apache-2.0
// FUiRenderer — Widget tree を FSpriteBatch + Font で描画する
//
// 使い方:
//   FUiRenderer ur;
//   ur.Init(*dev, GetRenderer().ColorFormat(), default_font);
//
//   // 毎フレーム:
//   StackPanel root;
//   /* root.Add<...>() で子を構築 */
//   root.Layout(0, 0, screen_w, screen_h);
//   ur.Render(root, *cmd, screen_w, screen_h);
//
// 仕組み:
//   ・FSpriteBatch で矩形 + テクスチャ + 文字を発行
//   ・Font は ACS Font (TTrueType + atlas)
//   ・Widget::Render(*this) を再帰的に呼ぶ
//   ・各 widget は描画ヘルパ (DrawRect / DrawText 等) で FUiRenderer に依頼
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiTexture.h"
#include "ui/Widgets.h"

namespace acs {

class FUiRenderer {
public:
    FUiRenderer() noexcept = default;
    ~FUiRenderer() noexcept = default;

    FUiRenderer(const FUiRenderer&) = delete;
    FUiRenderer& operator=(const FUiRenderer&) = delete;

    TResult<void> Init(IRhiDevice& device, EFormat rt_format, Font* default_font) noexcept;
    void Shutdown() noexcept;

    // 1 フレーム描画
    void Render(Widget& root, IRhiCommandList& cmd, u32 screen_w, u32 screen_h) noexcept;

    // ---- Widget の Render から呼ばれるプリミティブ ----
    void DrawRect(f32 x, f32 y, f32 w, f32 h, const FVec4& color) noexcept;
    void DrawRectOutline(f32 x, f32 y, f32 w, f32 h, const FVec4& color, f32 thickness = 1.0f) noexcept;
    void DrawText(const char* utf8, f32 x, f32 y, const FVec4& color) noexcept;

    // テーマ色 (Widgets が参照)
    const FUiColors& Colors() const noexcept { return m_Colors; }
    FUiColors&       Colors()       noexcept { return m_Colors; }

    Font* DefaultFont() const noexcept { return m_Font; }

private:
    FSpriteBatch m_Batch;
    Font*       m_Font = nullptr;     // 所有しない
    FUiColors    m_Colors;
    bool        m_bFrameOpen = false;
};

// ============================================================================
// FUiInput — FWindow のマウス/キーイベントを Widget tree に配信する
// ============================================================================
class FUiInput {
public:
    // 毎フレーム呼ぶ。Input モジュールのマウス位置 / クリック / 押下キーを取り出して
    // root の hit-test → 該当 widget の On* に dispatch。
    void Dispatch(Widget& root) noexcept;

private:
    Widget* m_Hovered  = nullptr;     // 直近 hover 中 widget
    Widget* m_Pressed  = nullptr;     // pointer-down 中 widget (drag 連続)
    Widget* m_Focused  = nullptr;     // フォーカス中 (TextInput)
};

} // namespace acs
