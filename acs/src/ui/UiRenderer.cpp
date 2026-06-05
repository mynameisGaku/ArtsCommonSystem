// SPDX-License-Identifier: Apache-2.0
// FUiRenderer / FUiInput 実装
#include "ui/UiRenderer.h"
#include "platform/Input.h"
#include "foundation/Move.h"

namespace acs {

/** SpriteBatch を初期化し、既定フォントを設定する。 */
TResult<void> FUiRenderer::Init(IRhiDevice& device, EFormat rt_format, Font* default_font) noexcept {
    auto r = m_Batch.Init(device, rt_format);
    if (r.IsErr()) return r;
    m_Font = default_font;
    return Ok();
}

/** GPU リソースを解放しフォント参照を切る。 */
void FUiRenderer::Shutdown() noexcept {
    m_Batch.Shutdown();
    m_Font = nullptr;
}

/** Widget ツリーを 1 フレーム分レイアウトして描画する。 */
void FUiRenderer::Render(Widget& root, IRhiCommandList& cmd, u32 screen_w, u32 screen_h) noexcept {
    root.Layout(0.0f, 0.0f, static_cast<f32>(screen_w), static_cast<f32>(screen_h));
    m_Batch.Begin(cmd, screen_w, screen_h);
    m_bFrameOpen = true;
    root.Render(*this);
    m_Batch.End();
    m_bFrameOpen = false;
}

/** 塗りつぶし矩形を発行する (フレームが開いている間のみ)。 */
void FUiRenderer::DrawRect(f32 x, f32 y, f32 w, f32 h, const FVec4& color) noexcept {
    if (m_bFrameOpen) m_Batch.DrawRect(x, y, w, h, color);
}

/** 矩形の枠線を発行する (4 辺を太さ t の矩形で描く)。 */
void FUiRenderer::DrawRectOutline(f32 x, f32 y, f32 w, f32 h, const FVec4& color, f32 t) noexcept {
    if (!m_bFrameOpen) return;
    m_Batch.DrawRect(x,             y,                 w, t, color);            // top
    m_Batch.DrawRect(x,             y + h - t,         w, t, color);            // bottom
    m_Batch.DrawRect(x,             y,                 t, h, color);            // left
    m_Batch.DrawRect(x + w - t,     y,                 t, h, color);            // right
}

/** UTF-8 文字列を既定フォントで描画する。 */
void FUiRenderer::DrawText(const char* utf8, f32 x, f32 y, const FVec4& color) noexcept {
    if (!m_bFrameOpen || !m_Font || !utf8) return;
    if (!m_Font->AtlasTexture()) return;
    m_Batch.DrawString(*m_Font, utf8, x, y, color);
}

// Widget 派生の Render 実装 (FUiRenderer への循環依存を避けるためここに置く)。

/** ラベルのテキストを描画する。 */
void Label::Render(FUiRenderer& r) noexcept {
    if (!visible) return;
    const auto& C = r.Colors();
    r.DrawText(text.Get().Data(), rect.x, rect.y + 4, C.text);
}

/** ボタンの背景・枠・ラベルを描画する。 */
void Button::Render(FUiRenderer& r) noexcept {
    if (!visible) return;
    const auto& C = r.Colors();
    const FVec4 bg = pressed ? C.button_press : (hovered ? C.button_hover : C.button_bg);
    r.DrawRect(rect.x, rect.y, rect.w, rect.h, bg);
    r.DrawRectOutline(rect.x, rect.y, rect.w, rect.h, C.panel_border);
    // 中央に近いテキスト (簡易: 左寄せ + 4px パディング)
    r.DrawText(text.Get().Data(), rect.x + 8, rect.y + (rect.h - 18) * 0.5f, C.button_text);
}

/** スライダーのトラック・値部分・つまみを描画する。 */
void Slider::Render(FUiRenderer& r) noexcept {
    if (!visible) return;
    const auto& C = r.Colors();
    // トラック
    const f32 ty = rect.y + rect.h * 0.5f - 3;
    r.DrawRect(rect.x, ty, rect.w, 6, C.slider_track);
    // 値
    f32 t = (value.Get() - min_value) / (max_value > min_value ? (max_value - min_value) : 1.0f);
    if (t < 0) t = 0; if (t > 1) t = 1;
    r.DrawRect(rect.x, ty, rect.w * t, 6, C.slider_fill);
    // つまみ
    const f32 kx = rect.x + rect.w * t - 6;
    r.DrawRect(kx, rect.y + 2, 12, rect.h - 4, C.slider_knob);
}

/** チェックボックスの箱・枠・チェックマーク・ラベルを描画する。 */
void Checkbox::Render(FUiRenderer& r) noexcept {
    if (!visible) return;
    const auto& C = r.Colors();
    const f32 box = 18.0f;
    const f32 by = rect.y + (rect.h - box) * 0.5f;
    r.DrawRect(rect.x, by, box, box, C.check_box);
    r.DrawRectOutline(rect.x, by, box, box, C.panel_border);
    if (checked.Get()) {
        r.DrawRect(rect.x + 4, by + 4, box - 8, box - 8, C.check_mark);
    }
    r.DrawText(text.Get().Data(), rect.x + box + 8, by, C.text);
}

/** テキスト入力欄の背景・枠・文字列を描画する。 */
void TextInput::Render(FUiRenderer& r) noexcept {
    if (!visible) return;
    const auto& C = r.Colors();
    const FVec4 bg = focused ? C.input_focus : C.input_bg;
    r.DrawRect(rect.x, rect.y, rect.w, rect.h, bg);
    r.DrawRectOutline(rect.x, rect.y, rect.w, rect.h, C.panel_border);
    r.DrawText(text.Get().Data(), rect.x + 6, rect.y + 4, C.text);
    if (focused) {
        // caret は未描画 (Font の文字幅計測 API が無く正確な右端を出せないため)。
    }
}

/** 入力を読み取り、ツリーをヒットテストして該当 widget にイベントを配信する。 */
void FUiInput::Dispatch(Widget& root) noexcept {
    // マウス位置取得
    const FVec2 mp = Input::MousePos();
    const f32 mx = mp.x, my = mp.y;

    // hover 更新
    Widget* const hit = root.HitTestRecursive(mx, my);
    if (hit != m_Hovered) {
        if (m_Hovered) m_Hovered->hovered = false;
        m_Hovered = hit;
        if (m_Hovered) m_Hovered->hovered = true;
    }
    // hovered widget へ pointer move
    if (m_Hovered) m_Hovered->OnPointerMove(mx, my);
    if (m_Pressed && m_Pressed != m_Hovered) m_Pressed->OnPointerMove(mx, my);

    // クリック
    if (Input::IsMouseButtonPressed(EMouseButton::Left)) {
        if (m_Focused && m_Focused != hit) m_Focused->focused = false;
        m_Focused = hit;
        if (m_Focused) m_Focused->focused = true;
        m_Pressed = hit;
        if (m_Pressed) m_Pressed->OnPointerDown(mx, my);
    }
    if (Input::IsMouseButtonReleased(EMouseButton::Left)) {
        if (m_Pressed) m_Pressed->OnPointerUp(mx, my);
        m_Pressed = nullptr;
    }

    // テキスト入力 / キー (focus 中の widget に流す)
    // 問題: 以前はここで何も配信しておらず、TextInput ウィジェットが文字も
    //       Backspace も受け取れず実質死んでいた。Input::TextInput() の確定文字列と
    //       編集系キーを読み取り、focus 中 widget の OnTextInput/OnKey へ実配信する。
    if (m_Focused) {
        // 1) このフレームに確定した文字列 (UTF-8, IME 確定後) を codepoint 単位で配信。
        //    DecodeUtf8 は render/Font.h の正規デコーダ (SpriteBatch 等と同一実装)。
        const char* txt = Input::TextInput();
        if (txt) {
            const char* p = txt;
            while (*p) {
                const u32 cp = DecodeUtf8(&p);
                if (cp == 0) break;            // 終端 / デコード失敗で停止
                if (cp == 0xFFFD) continue;     // 不正バイトはスキップ
                m_Focused->OnTextInput(cp);
            }
        }

        // 2) 文字を生まない編集系キーを OnKey へ。widget 側は慣用の制御コード
        //    (例: Backspace=0x08) で判定するため、EKey をその制御コードに対応付けて渡す。
        struct KeyMap { EKey key; i32 code; };
        static const KeyMap kEditKeys[] = {
            { EKey::Backspace, 0x08 },  // BS
            { EKey::Delete,    0x7F },  // DEL
            { EKey::Enter,     0x0D },  // CR
            { EKey::Tab,       0x09 },  // HT
            { EKey::Escape,    0x1B },  // ESC
            { EKey::Left,      0x25 },  // VK_LEFT  相当
            { EKey::Right,     0x27 },  // VK_RIGHT 相当
            { EKey::Home,      0x24 },  // VK_HOME  相当
            { EKey::End,       0x23 },  // VK_END   相当
        };
        for (const auto& km : kEditKeys) {
            if (Input::IsKeyPressed(km.key)) m_Focused->OnKey(km.code, true);
        }
    }
}

} // namespace acs
