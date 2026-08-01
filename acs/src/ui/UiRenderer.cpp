// SPDX-License-Identifier: Apache-2.0
// CUiRenderer / CUiInput 実装
#include "ui/UiRenderer.h"
#include "platform/Input.h"
#include "foundation/Move.h"

namespace acs {

// テキスト入力欄の内容領域へ選択ハイライトを切り詰める。
FUiRect ui_detail::ComputeTextSelectionHighlightRect(const FUiRect& input_rect,
                                                     f32 prefix_start,
                                                     f32 prefix_end) noexcept {
    const f32 content_left = input_rect.x + 6.0f;
    const f32 content_right =
        input_rect.w > 12.0f ? input_rect.x + input_rect.w - 6.0f : content_left;
    const f32 raw_start = content_left + prefix_start;
    const f32 raw_end = content_left + prefix_end;
    const f32 clipped_start =
        raw_start < content_left ? content_left
      : raw_start > content_right ? content_right
                                  : raw_start;
    const f32 clipped_end =
        raw_end < content_left ? content_left
      : raw_end > content_right ? content_right
                                : raw_end;
    const f32 selection_height =
        input_rect.h > 6.0f ? input_rect.h - 6.0f : 0.0f;
    const f32 selection_width =
        clipped_end > clipped_start ? clipped_end - clipped_start : 0.0f;
    return FUiRect{
        clipped_start,
        input_rect.y + 3.0f,
        selection_width,
        selection_height,
    };
}

/** SpriteBatch を初期化し、既定フォントを設定する。 */
TResult<void> CUiRenderer::Init(IRhiDevice& device, EFormat rt_format, FFont* default_font) noexcept {
    auto r = m_Batch.Init(device, rt_format);
    if (r.IsErr()) return r;
    m_Font = default_font;
    return Ok();
}

/** GPU リソースを解放しフォント参照を切る。 */
void CUiRenderer::Shutdown() noexcept {
    m_Batch.Shutdown();
    m_Font = nullptr;
}

/** AWidget ツリーを 1 フレーム分レイアウトして描画する。 */
void CUiRenderer::Render(AWidget& root, IRhiCommandList& cmd, u32 screen_w, u32 screen_h) noexcept {
    (void)ui_detail::VisitVisibleUiRoot(
        root, static_cast<f32>(screen_w), static_cast<f32>(screen_h),
        [this, &root, &cmd, screen_w, screen_h]() noexcept {
            m_Batch.Begin(cmd, screen_w, screen_h);
            m_bFrameOpen = true;
            root.Render(*this);
            m_Batch.End();
            m_bFrameOpen = false;
        });
}

/** 塗りつぶし矩形を発行する (フレームが開いている間のみ)。 */
void CUiRenderer::DrawRect(f32 x, f32 y, f32 w, f32 h, const FVec4& color) noexcept {
    if (m_bFrameOpen) m_Batch.DrawRect(x, y, w, h, color);
}

/** 矩形の枠線を発行する (4 辺を太さ t の矩形で描く)。 */
void CUiRenderer::DrawRectOutline(f32 x, f32 y, f32 w, f32 h, const FVec4& color, f32 t) noexcept {
    if (!m_bFrameOpen) return;
    m_Batch.DrawRect(x,             y,                 w, t, color);            // top
    m_Batch.DrawRect(x,             y + h - t,         w, t, color);            // bottom
    m_Batch.DrawRect(x,             y,                 t, h, color);            // left
    m_Batch.DrawRect(x + w - t,     y,                 t, h, color);            // right
}

/** UTF-8 文字列を既定フォントで描画する。 */
void CUiRenderer::DrawText(const char* utf8, f32 x, f32 y, const FVec4& color) noexcept {
    if (!m_bFrameOpen || !m_Font || !utf8) return;
    if (!m_Font->AtlasTexture()) return;
    m_Batch.DrawString(*m_Font, utf8, x, y, color);
}

/** 既定フォントで UTF-8 文字列の描画幅を測る (フォント未設定 / null なら 0)。 */
f32 CUiRenderer::MeasureText(const char* utf8) const noexcept {
    if (!m_Font || !utf8) return 0.0f;
    return m_Font->MeasureWidth(utf8);
}

/** UTF-8 文字列の先頭 byte_count バイトを、割り当てなしで安全に測る。 */
f32 CUiRenderer::MeasureTextBytes(const char* utf8, usize byte_count) const noexcept {
    if (!m_Font || !utf8 || byte_count == 0) return 0.0f;

    usize bounded_size = 0;
    while (bounded_size < byte_count && utf8[bounded_size] != '\0') ++bounded_size;

    f32 width = 0.0f;
    usize offset = 0;
    while (offset < bounded_size) {
        const u8 b0 = static_cast<u8>(utf8[offset]);
        u32 codepoint = b0;
        usize sequence_size = 1;
        const usize remaining = bounded_size - offset;

        if (b0 >= 0xC2u && b0 <= 0xDFu && remaining >= 2u) {
            const u8 b1 = static_cast<u8>(utf8[offset + 1u]);
            if ((b1 & 0xC0u) == 0x80u) {
                codepoint = ((b0 & 0x1Fu) << 6u) | (b1 & 0x3Fu);
                sequence_size = 2;
            }
        } else if (b0 >= 0xE0u && b0 <= 0xEFu && remaining >= 3u) {
            const u8 b1 = static_cast<u8>(utf8[offset + 1u]);
            const u8 b2 = static_cast<u8>(utf8[offset + 2u]);
            const bool first_ok = b0 == 0xE0u ? (b1 >= 0xA0u && b1 <= 0xBFu)
                                : b0 == 0xEDu ? (b1 >= 0x80u && b1 <= 0x9Fu)
                                             : ((b1 & 0xC0u) == 0x80u);
            if (first_ok && (b2 & 0xC0u) == 0x80u) {
                codepoint = ((b0 & 0x0Fu) << 12u) | ((b1 & 0x3Fu) << 6u) | (b2 & 0x3Fu);
                sequence_size = 3;
            }
        } else if (b0 >= 0xF0u && b0 <= 0xF4u && remaining >= 4u) {
            const u8 b1 = static_cast<u8>(utf8[offset + 1u]);
            const u8 b2 = static_cast<u8>(utf8[offset + 2u]);
            const u8 b3 = static_cast<u8>(utf8[offset + 3u]);
            const bool first_ok = b0 == 0xF0u ? (b1 >= 0x90u && b1 <= 0xBFu)
                                : b0 == 0xF4u ? (b1 >= 0x80u && b1 <= 0x8Fu)
                                             : ((b1 & 0xC0u) == 0x80u);
            if (first_ok && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
                codepoint = ((b0 & 0x07u) << 18u) | ((b1 & 0x3Fu) << 12u) |
                            ((b2 & 0x3Fu) << 6u) | (b3 & 0x3Fu);
                sequence_size = 4;
            }
        }

        if (codepoint != '\n') {
            FGlyphInfo glyph{};
            if (m_Font->GetGlyph(codepoint, glyph)) width += glyph.x_advance;
        }
        offset += sequence_size;
    }
    return width;
}

// AWidget 派生の Render 実装 (CUiRenderer への循環依存を避けるためここに置く)。

/** ラベルのテキストを描画する。 */
void ALabel::Render(CUiRenderer& r) noexcept {
    if (!visible) return;
    const auto& C = r.Colors();
    r.DrawText(text.Get().Data(), rect.x, rect.y + 4, C.text);
}

/** ボタンの背景・枠・ラベルを描画する。 */
void AButton::Render(CUiRenderer& r) noexcept {
    if (!visible) return;
    const auto& C = r.Colors();
    const FVec4 bg = pressed ? C.button_press : (hovered ? C.button_hover : C.button_bg);
    r.DrawRect(rect.x, rect.y, rect.w, rect.h, bg);
    r.DrawRectOutline(rect.x, rect.y, rect.w, rect.h, C.panel_border);
    // 中央に近いテキスト (簡易: 左寄せ + 4px パディング)
    r.DrawText(text.Get().Data(), rect.x + 8, rect.y + (rect.h - 18) * 0.5f, C.button_text);
}

/** スライダーのトラック・値部分・つまみを描画する。 */
void ASlider::Render(CUiRenderer& r) noexcept {
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
void ACheckbox::Render(CUiRenderer& r) noexcept {
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
void ATextInput::Render(CUiRenderer& r) noexcept {
    if (!visible) return;
    NormalizeSelection();
    const auto& C = r.Colors();
    const FVec4 bg = focused ? C.input_focus : C.input_bg;
    r.DrawRect(rect.x, rect.y, rect.w, rect.h, bg);
    r.DrawRectOutline(rect.x, rect.y, rect.w, rect.h, C.panel_border);
    if (focused && m_CursorByteOffset != m_SelectionAnchorByteOffset) {
        const usize selection_start =
            m_CursorByteOffset < m_SelectionAnchorByteOffset
                ? m_CursorByteOffset
                : m_SelectionAnchorByteOffset;
        const usize selection_end =
            m_CursorByteOffset > m_SelectionAnchorByteOffset
                ? m_CursorByteOffset
                : m_SelectionAnchorByteOffset;
        const f32 prefix_start =
            r.MeasureTextBytes(text.Get().Data(), selection_start);
        const f32 prefix_end =
            r.MeasureTextBytes(text.Get().Data(), selection_end);
        const FUiRect highlight =
            ui_detail::ComputeTextSelectionHighlightRect(rect, prefix_start, prefix_end);
        if (highlight.w > 0.0f && highlight.h > 0.0f) {
            r.DrawRect(highlight.x, highlight.y, highlight.w, highlight.h,
                       C.input_selection);
        }
    }
    r.DrawText(text.Get().Data(), rect.x + 6, rect.y + 4, C.text);
    if (focused) {
        // cursor までの UTF-8 prefix だけを測り、コードポイント境界へ caret を置く。
        const f32 cx = rect.x + 6.0f +
                       r.MeasureTextBytes(text.Get().Data(), m_CursorByteOffset);
        r.DrawRect(cx, rect.y + 4.0f, 1.5f, rect.h - 8.0f, C.text);
    }
}

void CUiInput::Reset() noexcept {
    m_RootIdentity = {};
    m_HoveredIdentity = {};
    m_PressedIdentity = {};
    m_FocusedIdentity = {};
    m_ControlAOwnerIdentity = {};
}

void CUiInput::Reset(AWidget& live_root) noexcept {
    live_root.ClearInputStateRecursive_Internal();
    Reset();
}

void CUiInput::PrepareRoot(AWidget& root) noexcept {
    const FTrackedIdentity root_identity = root.InputIdentity_Internal();
    if (m_RootIdentity == root_identity) return;

    // 以前の root は既に破棄済みかもしれないため、一切参照せず ID だけを捨てる。
    Reset();
    root.ClearInputStateRecursive_Internal();
    m_RootIdentity = root_identity;
}

AWidget* CUiInput::ResolveVisible(
        AWidget& root, const FTrackedIdentity& identity) noexcept {
    AWidget* const widget = root.FindByInputIdentity_Internal(identity);
    if (!widget || !widget->IsInputVisibleFrom_Internal(root)) return nullptr;
    return widget;
}

void CUiInput::ValidateTrackedState(AWidget& root) noexcept {
    const auto validate = [&root](FTrackedIdentity& identity,
                                  bool AWidget::* state) noexcept {
        if (!identity.IsSet()) return;
        AWidget* const widget = root.FindByInputIdentity_Internal(identity);
        if (widget && widget->IsInputVisibleFrom_Internal(root)) return;
        if (widget) widget->*state = false;
        identity = {};
    };

    validate(m_HoveredIdentity, &AWidget::hovered);
    validate(m_PressedIdentity, &AWidget::pressed);
    validate(m_FocusedIdentity, &AWidget::focused);
    if (!ResolveVisible(root, m_ControlAOwnerIdentity)) {
        m_ControlAOwnerIdentity = {};
    }
}

/** 入力を読み取り、ツリーをヒットテストして該当 widget にイベントを配信する。 */
void CUiInput::Dispatch(AWidget& root) noexcept {
    PrepareRoot(root);
    ValidateTrackedState(root);

    // マウス位置取得
    const FVec2 mp = FInput::MousePos();
    const f32 mx = mp.x, my = mp.y;

    // hover 更新
    AWidget* hit = root.HitTestRecursive(mx, my);
    const FTrackedIdentity hit_identity =
        hit ? hit->InputIdentity_Internal() : FTrackedIdentity{};
    if (hit_identity != m_HoveredIdentity) {
        if (AWidget* const hovered =
                ResolveVisible(root, m_HoveredIdentity)) {
            hovered->hovered = false;
        }
        m_HoveredIdentity = hit_identity;
        if (hit) hit->hovered = true;
    }
    // hovered widget へ pointer move
    if (AWidget* const hovered =
            ResolveVisible(root, m_HoveredIdentity)) {
        hovered->OnPointerMove(mx, my);
    }
    if (m_PressedIdentity.IsSet() &&
        m_PressedIdentity != m_HoveredIdentity) {
        if (AWidget* const pressed =
                ResolveVisible(root, m_PressedIdentity)) {
            pressed->OnPointerMove(mx, my);
        }
    }

    // クリック
    if (FInput::IsMouseButtonPressed(EMouseButton::Left)) {
        // pointer callback が child 構成を変更できるため、押下直前に hit-test をやり直す。
        hit = root.HitTestRecursive(mx, my);
        const FTrackedIdentity pressed_identity =
            hit ? hit->InputIdentity_Internal() : FTrackedIdentity{};
        if (m_FocusedIdentity != pressed_identity) {
            if (AWidget* const focused =
                    ResolveVisible(root, m_FocusedIdentity)) {
                focused->focused = false;
            }
        }
        m_FocusedIdentity = pressed_identity;
        if (hit) hit->focused = true;
        m_PressedIdentity = pressed_identity;
        if (hit) hit->OnPointerDown(mx, my);
    }
    if (FInput::IsMouseButtonReleased(EMouseButton::Left)) {
        const FTrackedIdentity released_identity = m_PressedIdentity;
        m_PressedIdentity = {};
        if (AWidget* const pressed =
                ResolveVisible(root, released_identity)) {
            pressed->OnPointerUp(mx, my);
        }
    }

    // pointer callback 後に除去・非表示になった widget は以降のキー配信対象にしない。
    ValidateTrackedState(root);

    // テキスト入力 / キー (focus 中の widget に流す)
    // 問題: 以前はここで何も配信しておらず、ATextInput ウィジェットが文字も
    //       Backspace も受け取れず実質死んでいた。FInput::TextInput() の確定文字列と
    //       編集系キーを読み取り、focus 中 widget の OnTextInput/OnKey へ実配信する。
    if (m_FocusedIdentity.IsSet() || m_ControlAOwnerIdentity.IsSet()) {
        // 1) このフレームに確定した文字列 (UTF-8, IME 確定後) を codepoint 単位で配信。
        //    checked canonical decoder で正規の U+FFFD は受理し、不正入力だけを除外する。
        const char* txt = FInput::TextInput();
        if (txt) {
            const char* p = txt;
            while (*p) {
                u32 cp = 0;
                if (!TryDecodeUtf8(&p, cp)) continue;
                AWidget* const focused =
                    ResolveVisible(root, m_FocusedIdentity);
                if (!focused) {
                    ValidateTrackedState(root);
                    break;
                }
                focused->OnTextInput(cp);
            }
        }

        // 2) 文字を生まない編集系キーを OnKey へ。widget 側は慣用の制御コード
        //    (例: Backspace=0x08) で判定するため、EKey をその制御コードに対応付けて渡す。
        //    修飾キーは左右をまとめた押下中スナップショットとして同じイベントに付ける。
        const FUiKeyModifiers modifiers{
            FInput::IsKeyDown(EKey::LeftShift) ||
                FInput::IsKeyDown(EKey::RightShift),
            FInput::IsKeyDown(EKey::LeftCtrl) ||
                FInput::IsKeyDown(EKey::RightCtrl),
            FInput::IsKeyDown(EKey::LeftAlt) ||
                FInput::IsKeyDown(EKey::RightAlt),
            FInput::IsKeyDown(EKey::LeftSuper) ||
                FInput::IsKeyDown(EKey::RightSuper),
        };
        struct FKeyMap { EKey key; i32 code; };
        static const FKeyMap kEditKeys[] = {
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
            const bool key_pressed = FInput::IsKeyPressed(km.key);
            const bool key_released = FInput::IsKeyReleased(km.key);
            if (!key_pressed && !key_released) continue;

            ValidateTrackedState(root);
            if (AWidget* const focused =
                    ResolveVisible(root, m_FocusedIdentity)) {
                focused->OnKey(km.code, key_pressed, modifiers);
            }
        }

        // Ctrl+A は文字入力ではなく選択コマンドとして配信する。
        if (modifiers.bControl && !modifiers.bAlt && !modifiers.bSuper &&
            FInput::IsKeyPressed(EKey::A)) {
            ValidateTrackedState(root);
            if (AWidget* const focused =
                    ResolveVisible(root, m_FocusedIdentity)) {
                m_ControlAOwnerIdentity = m_FocusedIdentity;
                focused->OnKey(0x41, true, modifiers);
            }
        }
        if (FInput::IsKeyReleased(EKey::A)) {
            const FTrackedIdentity owner_identity = m_ControlAOwnerIdentity;
            m_ControlAOwnerIdentity = {};
            if (AWidget* const owner =
                    ResolveVisible(root, owner_identity)) {
                owner->OnKey(0x41, false, modifiers);
            }
        }
    }
}

} // namespace acs
