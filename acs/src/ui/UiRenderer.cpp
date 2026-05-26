// SPDX-License-Identifier: Apache-2.0
// FUiRenderer / FUiInput 実装
#include "ui/UiRenderer.h"
#include "platform/Input.h"
#include "foundation/Move.h"

namespace acs {

// ============================================================================
// FUiRenderer
// ============================================================================
TResult<void> FUiRenderer::Init(IRhiDevice& device, EFormat rt_format, FFont* default_font) noexcept {
    auto r = _batch.Init(device, rt_format);
    if (r.IsErr()) return r;
    _font = default_font;
    return Ok();
}

void FUiRenderer::Shutdown() noexcept {
    _batch.Shutdown();
    _font = nullptr;
}

void FUiRenderer::Render(FWidget& root, IRhiCommandList& cmd, u32 screen_w, u32 screen_h) noexcept {
    root.Layout(0.0f, 0.0f, static_cast<f32>(screen_w), static_cast<f32>(screen_h));
    _batch.Begin(cmd, screen_w, screen_h);
    _frame_open = true;
    root.Render(*this);
    _batch.End();
    _frame_open = false;
}

void FUiRenderer::DrawRect(f32 x, f32 y, f32 w, f32 h, const FVec4& color) noexcept {
    if (_frame_open) _batch.DrawRect(x, y, w, h, color);
}

void FUiRenderer::DrawRectOutline(f32 x, f32 y, f32 w, f32 h, const FVec4& color, f32 t) noexcept {
    if (!_frame_open) return;
    _batch.DrawRect(x,             y,                 w, t, color);            // top
    _batch.DrawRect(x,             y + h - t,         w, t, color);            // bottom
    _batch.DrawRect(x,             y,                 t, h, color);            // left
    _batch.DrawRect(x + w - t,     y,                 t, h, color);            // right
}

void FUiRenderer::DrawText(const char* utf8, f32 x, f32 y, const FVec4& color) noexcept {
    if (!_frame_open || !_font || !utf8) return;
    if (!_font->AtlasTexture()) return;
    _batch.DrawString(*_font, utf8, x, y, color);
}

// ============================================================================
// FWidget 派生の Render 実装 (循環依存のためここに置く)
// ============================================================================
void FLabel::Render(FUiRenderer& r) noexcept {
    if (!visible) return;
    const auto& C = r.Colors();
    r.DrawText(text.Get().Data(), rect.x, rect.y + 4, C.text);
}

void FButton::Render(FUiRenderer& r) noexcept {
    if (!visible) return;
    const auto& C = r.Colors();
    FVec4 bg = pressed ? C.button_press : (hovered ? C.button_hover : C.button_bg);
    r.DrawRect(rect.x, rect.y, rect.w, rect.h, bg);
    r.DrawRectOutline(rect.x, rect.y, rect.w, rect.h, C.panel_border);
    // 中央に近いテキスト (簡易: 左寄せ + 4px パディング)
    r.DrawText(text.Get().Data(), rect.x + 8, rect.y + (rect.h - 18) * 0.5f, C.button_text);
}

void FSlider::Render(FUiRenderer& r) noexcept {
    if (!visible) return;
    const auto& C = r.Colors();
    // トラック
    f32 ty = rect.y + rect.h * 0.5f - 3;
    r.DrawRect(rect.x, ty, rect.w, 6, C.slider_track);
    // 値
    f32 t = (value.Get() - min_value) / (max_value > min_value ? (max_value - min_value) : 1.0f);
    if (t < 0) t = 0; if (t > 1) t = 1;
    r.DrawRect(rect.x, ty, rect.w * t, 6, C.slider_fill);
    // つまみ
    f32 kx = rect.x + rect.w * t - 6;
    r.DrawRect(kx, rect.y + 2, 12, rect.h - 4, C.slider_knob);
}

void FCheckbox::Render(FUiRenderer& r) noexcept {
    if (!visible) return;
    const auto& C = r.Colors();
    f32 box = 18.0f;
    f32 by = rect.y + (rect.h - box) * 0.5f;
    r.DrawRect(rect.x, by, box, box, C.check_box);
    r.DrawRectOutline(rect.x, by, box, box, C.panel_border);
    if (checked.Get()) {
        r.DrawRect(rect.x + 4, by + 4, box - 8, box - 8, C.check_mark);
    }
    r.DrawText(text.Get().Data(), rect.x + box + 8, by, C.text);
}

void FTextInput::Render(FUiRenderer& r) noexcept {
    if (!visible) return;
    const auto& C = r.Colors();
    FVec4 bg = focused ? C.input_focus : C.input_bg;
    r.DrawRect(rect.x, rect.y, rect.w, rect.h, bg);
    r.DrawRectOutline(rect.x, rect.y, rect.w, rect.h, C.panel_border);
    r.DrawText(text.Get().Data(), rect.x + 6, rect.y + 4, C.text);
    if (focused) {
        // 簡易 caret (右端固定、blink なし)
        // FFont 幅取得が無いので、入力後の右端推定で代用
        // 一旦省略 — TODO: FFont::MeasureText 経由で正確に
    }
}

// ============================================================================
// FUiInput
// ============================================================================
void FUiInput::Dispatch(FWidget& root) noexcept {
    // マウス位置取得
    FVec2 mp = FInput::MousePos();
    f32 mx = mp.x, my = mp.y;

    // hover 更新
    FWidget* hit = root.HitTestRecursive(mx, my);
    if (hit != _hovered) {
        if (_hovered) _hovered->hovered = false;
        _hovered = hit;
        if (_hovered) _hovered->hovered = true;
    }
    // hovered widget へ pointer move
    if (_hovered) _hovered->OnPointerMove(mx, my);
    if (_pressed && _pressed != _hovered) _pressed->OnPointerMove(mx, my);

    // クリック
    if (FInput::IsMouseButtonPressed(EMouseButton::Left)) {
        if (_focused && _focused != hit) _focused->focused = false;
        _focused = hit;
        if (_focused) _focused->focused = true;
        _pressed = hit;
        if (_pressed) _pressed->OnPointerDown(mx, my);
    }
    if (FInput::IsMouseButtonReleased(EMouseButton::Left)) {
        if (_pressed) _pressed->OnPointerUp(mx, my);
        _pressed = nullptr;
    }

    // テキスト入力 / キー (focus 中の widget に流す)
    // 注: ACS FInput がテキスト入力イベントを保持しているなら配信する。
    // 現状の FInput API で取得できない場合はコメントアウトでスキップ。
}

} // namespace acs
