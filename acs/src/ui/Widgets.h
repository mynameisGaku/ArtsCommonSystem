// SPDX-License-Identifier: Apache-2.0
// 標準ウィジェット — FLabel / FButton / FSlider / FCheckbox / FTextInput
//
// すべての widget は TObservable<T> プロパティで状態を公開しているので、
// MVVM の Bind* で FViewModel と直結できる:
//
//   class FPlayerViewModel : public FViewModel { TObservable<f32> hp{100}; };
//   FPlayerViewModel vm;
//   FStackPanel root;
//   auto* sl = root.Add<FSlider>(0.0f, 100.0f);
//   auto bind = MakeTwoWayBind(vm.hp, sl->value);   // 双方向同期
#pragma once

#include "ui/Widget.h"
#include "container/String.h"
#include "mvvm/Observable.h"
#include "mvvm/Binder.h"   // Bind / MakeBind / MakeTwoWayBind を ui/ ユーザに直送

namespace acs {

class FFont;

/**
 * UI ウィジェット共通の配色テーマ (簡易カラーセット)。
 *
 * @details FUiRenderer が保持し、各 widget の Render が色名で参照する。
 */
struct FUiColors {
    /** パネル背景色。 */
    FVec4 panel_bg     = { 0.16f, 0.18f, 0.24f, 0.95f };

    /** パネル・widget 枠線の色。 */
    FVec4 panel_border = { 0.40f, 0.45f, 0.55f, 0.80f };

    /** ボタン通常時の背景色。 */
    FVec4 button_bg    = { 0.25f, 0.40f, 0.65f, 1.0f };

    /** ボタン hover 時の背景色。 */
    FVec4 button_hover = { 0.35f, 0.55f, 0.85f, 1.0f };

    /** ボタン押下時の背景色。 */
    FVec4 button_press = { 0.18f, 0.30f, 0.50f, 1.0f };

    /** ボタンのラベル文字色。 */
    FVec4 button_text  = { 1.0f, 1.0f, 1.0f, 1.0f };

    /** スライダーのトラック色。 */
    FVec4 slider_track = { 0.20f, 0.20f, 0.25f, 1.0f };

    /** スライダーの値部分 (fill) の色。 */
    FVec4 slider_fill  = { 0.45f, 0.65f, 0.95f, 1.0f };

    /** スライダーのつまみの色。 */
    FVec4 slider_knob  = { 0.85f, 0.90f, 1.0f, 1.0f };

    /** チェックボックスの箱の色。 */
    FVec4 check_box    = { 0.20f, 0.22f, 0.28f, 1.0f };

    /** チェックボックスのチェックマークの色。 */
    FVec4 check_mark   = { 0.45f, 0.85f, 0.45f, 1.0f };

    /** 標準テキスト色。 */
    FVec4 text         = { 1.0f, 1.0f, 1.0f, 1.0f };

    /** 抑えめのテキスト色 (補助表示用)。 */
    FVec4 text_dim     = { 0.7f, 0.75f, 0.85f, 1.0f };

    /** テキスト入力欄の背景色。 */
    FVec4 input_bg     = { 0.10f, 0.12f, 0.16f, 1.0f };

    /** テキスト入力欄のフォーカス時背景色。 */
    FVec4 input_focus  = { 0.20f, 0.30f, 0.50f, 1.0f };

    /** テキスト入力欄の選択範囲背景色。 */
    FVec4 input_selection = { 0.25f, 0.52f, 0.88f, 0.72f };
};

/**
 * プロセス共有の既定 FUiColors への参照を返す。
 *
 * @return 静的に確保された既定カラーセットへの参照。
 */
inline FUiColors& DefaultUiColors() noexcept {
    static FUiColors s;
    return s;
}

/**
 * 静的テキストを表示するラベル widget。
 */
class FLabel : public FWidget {
public:
    /**
     * 初期テキストを指定して構築する。
     *
     * @param initial 初期表示文字列 (既定は空文字列)。
     */
    explicit FLabel(const char* initial = "") noexcept : text(FString{initial}) {
        requested.h = 22.0f;
    }

    /** 表示する文字列 (双方向 Bind 可能)。 */
    TObservable<FString> text;

    /**
     * テキストを描画する。
     *
     * @param r 描画ヘルパとテーマ色を提供する UI レンダラ。
     */
    void Render(FUiRenderer& r) noexcept override;
};

/**
 * クリックできるボタン widget。
 */
class FButton : public FWidget {
public:
    /**
     * ラベル文字列を指定して構築する。
     *
     * @param label ボタンに表示する文字列 (既定 "Button")。
     */
    explicit FButton(const char* label = "Button") noexcept : text(FString{label}) {
        requested.h = 32.0f;
    }

    /** ボタンのラベル文字列 (双方向 Bind 可能)。 */
    TObservable<FString> text;

    /**
     * クリック完了を通知する pulse TObservable。
     *
     * @details
     * Down → Up が widget 内で完結したとき true を Set した直後に false へ戻す。
     * Subscribe 側は値が true のときだけ反応する。
     */
    TObservable<bool> clicked{ false };

    /**
     * ボタンの背景・枠・ラベルを描画する。
     *
     * @param r 描画ヘルパとテーマ色を提供する UI レンダラ。
     */
    void Render(FUiRenderer& r) noexcept override;

    /**
     * 押下状態に入る。
     *
     * @param px 押下点の X 座標 (未使用)。
     * @param py 押下点の Y 座標 (未使用)。
     */
    void OnPointerDown(f32 /*px*/, f32 /*py*/) noexcept override { pressed = true; }

    /**
     * 解放時、widget 内で押下が完結していれば clicked を pulse する。
     *
     * @param px 解放点の X 座標。
     * @param py 解放点の Y 座標。
     */
    void OnPointerUp  (f32 px, f32 py) noexcept override {
        if (pressed && rect.Contains(px, py)) {
            pressed = false;
            ui_detail::FWidgetCallbackLifetimeGuard lifetime(*this);
            clicked.Set(true);
            if (!lifetime.IsAlive()) return;
            clicked.Set(false);  // pulse 仕様 (Subscribe 側は true のときのみ反応)
            return;
        }
        pressed = false;
    }
};

/**
 * 範囲付きの値をドラッグで編集するスライダー widget。
 */
class FSlider : public FWidget {
public:
    /**
     * 値域を指定して構築する。
     *
     * @param min_v 取りうる最小値 (既定 0)。
     * @param max_v 取りうる最大値 (既定 1)。
     */
    FSlider(f32 min_v = 0, f32 max_v = 1) noexcept : min_value(min_v), max_value(max_v) {
        requested.h = 24.0f;
    }

    /** 現在値 (双方向 Bind 可能)。 */
    TObservable<f32> value{ 0.0f };

    /** 取りうる最小値。 */
    f32 min_value = 0.0f;

    /** 取りうる最大値。 */
    f32 max_value = 1.0f;

    /**
     * トラック・値部分・つまみを描画する。
     *
     * @param r 描画ヘルパとテーマ色を提供する UI レンダラ。
     */
    void Render(FUiRenderer& r) noexcept override;

    /**
     * 押下して、その X 位置から値を更新する。
     *
     * @param px 押下点の X 座標。
     * @param py 押下点の Y 座標 (未使用)。
     */
    void OnPointerDown(f32 px, f32 /*py*/) noexcept override { pressed = true; UpdateFromMouse(px); }

    /**
     * ドラッグ中なら X 位置から値を更新する。
     *
     * @param px 現在の X 座標。
     * @param py 現在の Y 座標 (未使用)。
     */
    void OnPointerMove(f32 px, f32 /*py*/) noexcept override { if (pressed) UpdateFromMouse(px); }

    /**
     * ドラッグを終了する。
     *
     * @param px 解放点の X 座標 (未使用)。
     * @param py 解放点の Y 座標 (未使用)。
     */
    void OnPointerUp  (f32 /*px*/, f32 /*py*/) noexcept override { pressed = false; }

private:
    /**
     * マウス X 座標を rect 内の比率 [0,1] に変換して value を更新する。
     *
     * @param px マウスの X 座標。
     */
    void UpdateFromMouse(f32 px) noexcept {
        if (rect.w <= 0) return;
        f32 t = (px - rect.x) / rect.w;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        value.Set(min_value + t * (max_value - min_value));
    }
};

/**
 * bool 値をトグルするチェックボックス widget。
 */
class FCheckbox : public FWidget {
public:
    /**
     * ラベル文字列を指定して構築する。
     *
     * @param label チェックボックス横に表示する文字列 (既定は空文字列)。
     */
    explicit FCheckbox(const char* label = "") noexcept : text(FString{label}) {
        requested.h = 24.0f;
    }

    /** チェック状態 (双方向 Bind 可能)。 */
    TObservable<bool>   checked{ false };

    /** 横に表示するラベル文字列 (双方向 Bind 可能)。 */
    TObservable<FString> text;

    /**
     * 箱・枠・チェックマーク・ラベルを描画する。
     *
     * @param r 描画ヘルパとテーマ色を提供する UI レンダラ。
     */
    void Render(FUiRenderer& r) noexcept override;

    /**
     * 矩形内で解放されたら checked を反転する。
     *
     * @param px 解放点の X 座標。
     * @param py 解放点の Y 座標。
     */
    void OnPointerUp(f32 px, f32 py) noexcept override {
        if (rect.Contains(px, py)) checked.Set(!checked.Get());
    }
};

/**
 * UTF-8 の 1 行テキストをコードポイント境界で編集する入力 widget。
 *
 * @details
 * cursor は UTF-8 のバイトオフセットとして保持するが、公開操作の前後で必ずコードポイント
 * 境界へ正規化する。編集は一時 FString 上で完了してから observable へ move するため、
 * 容量確保に失敗しても文字列と cursor は変更されない。
 */
class FTextInput : public FWidget {
public:
    /** 既定の入力上限 (NUL を含まない UTF-8 バイト数)。 */
    static constexpr usize kDefaultMaxTextBytes = 4096u;

    /** 誤設定による無制限確保を防ぐ設定可能上限。 */
    static constexpr usize kHardMaxTextBytes = 1024u * 1024u;

    /** 空文字列で構築する。 */
    FTextInput() noexcept {
        requested.h = 28.0f;
    }

    /** 編集中の文字列 (双方向 Bind 可能)。 */
    TObservable<FString> text{ FString{} };

    /**
     * 入力欄の背景・枠・文字列と現在の cursor を描画する。
     *
     * @param r 描画ヘルパとテーマ色を提供する UI レンダラ。
     */
    void Render(FUiRenderer& r) noexcept override;

    /**
     * クリックでフォーカスを得て cursor を末尾へ移動する。
     *
     * @details 現在の pointer API から glyph hit 情報を取得できないため、クリック位置への
     * cursor 配置ではなく従来互換の末尾配置を行う。
     * @param px 押下点の X 座標 (未使用)。
     * @param py 押下点の Y 座標 (未使用)。
     */
    void OnPointerDown(f32 /*px*/, f32 /*py*/) noexcept override {
        focused = true;
        MoveCursorToEnd();
    }

    /**
     * フォーカス中なら Unicode scalar value を現在の cursor に挿入する。
     *
     * @details C0/C1 制御文字、DEL、surrogate、Unicode 範囲外、改行 separator は拒否する。
     * @param codepoint 入力された Unicode コードポイント。
     */
    void OnTextInput(u32 codepoint) noexcept override {
        if (focused) (void)TryInsertCodepoint(codepoint);
    }

    /**
     * Unicode scalar value を現在の cursor に挿入する。
     *
     * @details 選択中は範囲を置換する。無効な値、上限超過、確保失敗では false を返し、
     * 文字列、cursor、選択範囲を変更しない。
     * @param codepoint 挿入する Unicode コードポイント。
     * @return 挿入できた場合 true。
     */
    bool TryInsertCodepoint(u32 codepoint) noexcept {
        char encoded[4]{};
        const usize encoded_size = EncodeCodepoint(codepoint, encoded);
        if (encoded_size == 0) return false;

        const FString& current = text.Get();
        const usize current_size = current.Size();
        const usize cursor = NormalizedCursorOffset();
        const usize anchor = NormalizedSelectionAnchorOffset();
        const usize replace_begin = cursor < anchor ? cursor : anchor;
        const usize replace_end = cursor < anchor ? anchor : cursor;
        const usize retained_size = current_size - (replace_end - replace_begin);
        if (retained_size > m_MaxTextBytes ||
            encoded_size > m_MaxTextBytes - retained_size) {
            return false;
        }

        FString staged(*current.GetAllocator());
        const usize new_size = retained_size + encoded_size;
        if (!staged.TryReserve(new_size)) return false;
        if (!staged.TryAppend(FStringView(current.Data(), replace_begin))) return false;
        if (!staged.TryAppend(FStringView(encoded, encoded_size))) return false;
        if (!staged.TryAppend(FStringView(current.Data() + replace_end,
                                          current_size - replace_end))) {
            return false;
        }

        const usize new_cursor = replace_begin + encoded_size;
        m_CursorByteOffset = new_cursor;
        m_CursorFollowsEnd = (new_cursor == new_size);
        m_SelectionAnchorByteOffset = new_cursor;
        m_SelectionAnchorFollowsEnd = m_CursorFollowsEnd;
        ui_detail::FWidgetCallbackLifetimeGuard lifetime(*this);
        text.Set(Move(staged));
        if (!lifetime.IsAlive()) return true;
        NormalizeSelection();
        return true;
    }

    /**
     * フォーカス中の編集キーを処理する。
     *
     * @details 直接呼ばれた場合は修飾キーなしで処理する。3 引数版から転送中なら、その
     * callback の間だけ保持した修飾キースナップショットを使う。この関数をさらに派生型が
     * override していても、3 引数 Dispatch から仮想呼び出しで到達する。組み込みの
     * FTextInput 編集も残す派生 override は FTextInput::OnKey を呼ぶこと。
     * @param key FUiInput が編集キーへ割り当てる互換制御コード。
     * @param pressed_ 押下なら true、解放なら false。
     */
    void OnKey(i32 key, bool pressed_) noexcept override {
        const FUiKeyModifiers no_modifiers{};
        const FUiKeyModifiers& modifiers =
            m_ForwardedKeyModifiers ? *m_ForwardedKeyModifiers : no_modifiers;

        if (!focused || !pressed_) return;
        // AltGr (Ctrl+Alt) の文字入力を Ctrl+A と誤認しない。
        if (key == 0x41 && modifiers.bControl &&
            !modifiers.bAlt && !modifiers.bSuper) {
            SelectAll();
            return;
        }
        switch (key) {
        case 0x08: (void)TryEraseBeforeCursor(); break; // Backspace キー
        case 0x7F: (void)TryEraseAtCursor();     break; // Delete キー
        case 0x25:
            if (modifiers.bShift) ExtendSelectionLeft();
            else MoveCursorLeft();
            break; // 左
        case 0x27:
            if (modifiers.bShift) ExtendSelectionRight();
            else MoveCursorRight();
            break; // 右
        case 0x24:
            if (modifiers.bShift) ExtendSelectionToStart();
            else MoveCursorToStart();
            break; // 先頭
        case 0x23:
            if (modifiers.bShift) ExtendSelectionToEnd();
            else MoveCursorToEnd();
            break; // 末尾
        default: break;
        }
    }

    /**
     * 修飾キーを保持して従来の仮想 2 引数 OnKey へ転送する。
     *
     * @details FTextInput 自身では2引数版が Backspace/Delete/Left/Right/Home/End、
     * Shift 選択、Ctrl+A を処理する。ここから2引数版を仮想呼び出しするため、既存の
     * FTextInput 派生型が2引数版だけを override していても新しい3引数 Dispatch を受ける。
     * 入れ子の OnKey 呼び出しでは以前の snapshot を保存・復元する。
     * @param key FUiInput が編集キーへ割り当てる互換制御コード。
     * @param pressed_ 押下なら true、解放なら false。
     * @param modifiers 配信時点の修飾キー状態。
     */
    void OnKey(i32 key, bool pressed_,
               const FUiKeyModifiers& modifiers) noexcept override {
        ui_detail::FWidgetCallbackLifetimeGuard lifetime(*this);
        const FUiKeyModifiers* const previous = m_ForwardedKeyModifiers;
        m_ForwardedKeyModifiers = &modifiers;
        OnKey(key, pressed_);
        if (!lifetime.IsAlive()) return;
        m_ForwardedKeyModifiers = previous;
    }

    /**
     * Backspace 相当の削除を行う。
     *
     * @return 1 コードポイント削除した場合 true。先頭または OOM では false。
     */
    bool TryEraseBeforeCursor() noexcept {
        const usize cursor = NormalizedCursorOffset();
        const usize anchor = NormalizedSelectionAnchorOffset();
        if (cursor != anchor) {
            return TryEraseRange(cursor < anchor ? cursor : anchor,
                                 cursor < anchor ? anchor : cursor);
        }
        if (cursor == 0) return false;
        const usize erase_begin = PreviousBoundary(text.Get().Data(), text.Get().Size(),
                                                   cursor);
        return TryEraseRange(erase_begin, cursor);
    }

    /**
     * Delete 相当の削除を行う。
     *
     * @return 1 コードポイント削除した場合 true。末尾または OOM では false。
     */
    bool TryEraseAtCursor() noexcept {
        const FString& current = text.Get();
        const usize cursor = NormalizedCursorOffset();
        const usize anchor = NormalizedSelectionAnchorOffset();
        if (cursor != anchor) {
            return TryEraseRange(cursor < anchor ? cursor : anchor,
                                 cursor < anchor ? anchor : cursor);
        }
        if (cursor >= current.Size()) return false;
        const usize erase_end = NextBoundary(current.Data(), current.Size(), cursor);
        return TryEraseRange(cursor, erase_end);
    }

    /** cursor を 1 コードポイント左へ移動する。 */
    void MoveCursorLeft() noexcept {
        NormalizeSelection();
        if (m_CursorByteOffset != m_SelectionAnchorByteOffset) {
            m_CursorByteOffset =
                m_CursorByteOffset < m_SelectionAnchorByteOffset
                    ? m_CursorByteOffset
                    : m_SelectionAnchorByteOffset;
        } else {
            m_CursorByteOffset = PreviousBoundary(text.Get().Data(), text.Get().Size(),
                                                  m_CursorByteOffset);
        }
        m_CursorFollowsEnd = false;
        m_SelectionAnchorByteOffset = m_CursorByteOffset;
        m_SelectionAnchorFollowsEnd = false;
    }

    /** cursor を 1 コードポイント右へ移動する。 */
    void MoveCursorRight() noexcept {
        NormalizeSelection();
        const usize size = text.Get().Size();
        if (m_CursorByteOffset != m_SelectionAnchorByteOffset) {
            m_CursorByteOffset =
                m_CursorByteOffset > m_SelectionAnchorByteOffset
                    ? m_CursorByteOffset
                    : m_SelectionAnchorByteOffset;
        } else {
            m_CursorByteOffset = NextBoundary(text.Get().Data(), size,
                                              m_CursorByteOffset);
        }
        m_CursorFollowsEnd = (m_CursorByteOffset == size);
        m_SelectionAnchorByteOffset = m_CursorByteOffset;
        m_SelectionAnchorFollowsEnd = m_CursorFollowsEnd;
    }

    /** cursor を先頭へ移動する。 */
    void MoveCursorToStart() noexcept {
        m_CursorByteOffset = 0;
        m_CursorFollowsEnd = false;
        m_SelectionAnchorByteOffset = 0;
        m_SelectionAnchorFollowsEnd = false;
    }

    /** cursor を末尾へ移動する。 */
    void MoveCursorToEnd() noexcept {
        m_CursorByteOffset = text.Get().Size();
        m_CursorFollowsEnd = true;
        m_SelectionAnchorByteOffset = m_CursorByteOffset;
        m_SelectionAnchorFollowsEnd = true;
    }

    /**
     * cursor の現在位置を UTF-8 バイトオフセットで返す。
     *
     * @return [0, text.Size()] 内のコードポイント境界。
     */
    usize CursorByteOffset() noexcept {
        NormalizeSelection();
        return m_CursorByteOffset;
    }

    /**
     * cursor 位置を設定する。
     *
     * @details 範囲外またはコードポイント途中なら変更しない。
     * @param byte_offset UTF-8 バイトオフセット。
     * @return 正しい境界へ設定できた場合 true。
     */
    bool TrySetCursorByteOffset(usize byte_offset) noexcept {
        const FString& current = text.Get();
        if (byte_offset > current.Size()) return false;
        if (BoundaryAtOrBefore(current.Data(), current.Size(), byte_offset) != byte_offset) {
            return false;
        }
        m_CursorByteOffset = byte_offset;
        m_CursorFollowsEnd = (byte_offset == current.Size());
        m_SelectionAnchorByteOffset = byte_offset;
        m_SelectionAnchorFollowsEnd = m_CursorFollowsEnd;
        return true;
    }

    /**
     * 空でない選択範囲があるかを返す。
     *
     * @details 外部 binding で文字列が置き換わっていれば、両端点を現在の UTF-8 境界へ
     * 正規化してから判定する。
     */
    bool HasSelection() noexcept {
        NormalizeSelection();
        return m_CursorByteOffset != m_SelectionAnchorByteOffset;
    }

    /** 選択範囲の先頭を UTF-8 バイトオフセットで返す。 */
    usize SelectionStart() noexcept {
        NormalizeSelection();
        return m_CursorByteOffset < m_SelectionAnchorByteOffset
                   ? m_CursorByteOffset
                   : m_SelectionAnchorByteOffset;
    }

    /** 選択範囲の末尾を UTF-8 バイトオフセットで返す。 */
    usize SelectionEnd() noexcept {
        NormalizeSelection();
        return m_CursorByteOffset > m_SelectionAnchorByteOffset
                   ? m_CursorByteOffset
                   : m_SelectionAnchorByteOffset;
    }

    /**
     * UTF-8 バイトオフセットで選択範囲を設定する。
     *
     * @details `selection_start <= selection_end` で、両方が現在の文字列のコードポイント
     * 境界でなければならない。成功時は cursor を末尾へ置く。失敗時は全状態を維持する。
     */
    bool TrySetSelection(usize selection_start, usize selection_end) noexcept {
        const FString& current = text.Get();
        if (selection_start > selection_end || selection_end > current.Size()) {
            return false;
        }
        if (BoundaryAtOrBefore(current.Data(), current.Size(), selection_start) !=
                selection_start ||
            BoundaryAtOrBefore(current.Data(), current.Size(), selection_end) !=
                selection_end) {
            return false;
        }
        m_SelectionAnchorByteOffset = selection_start;
        m_SelectionAnchorFollowsEnd = (selection_start == current.Size());
        m_CursorByteOffset = selection_end;
        m_CursorFollowsEnd = (selection_end == current.Size());
        return true;
    }

    /** 選択を解除し、anchor を現在の cursor へ折り畳む。 */
    void ClearSelection() noexcept {
        NormalizeSelection();
        m_SelectionAnchorByteOffset = m_CursorByteOffset;
        m_SelectionAnchorFollowsEnd = m_CursorFollowsEnd;
    }

    /** 現在の文字列全体を選択し、cursor を末尾へ置く。 */
    void SelectAll() noexcept {
        const usize size = text.Get().Size();
        m_SelectionAnchorByteOffset = 0;
        m_SelectionAnchorFollowsEnd = (size == 0);
        m_CursorByteOffset = size;
        m_CursorFollowsEnd = true;
    }

    /**
     * 挿入可能な最大 UTF-8 バイト数を設定する。
     *
     * @details 既存文字列は切り詰めない。現在値より小さくした場合、削除は可能だが新規挿入は
     * 現在値が上限以下になるまで拒否される。
     * @param max_bytes 新しい上限。
     * @return hard limit 以下なら true。超過なら変更せず false。
     */
    bool TrySetMaxTextBytes(usize max_bytes) noexcept {
        if (max_bytes > kHardMaxTextBytes) return false;
        m_MaxTextBytes = max_bytes;
        return true;
    }

    /** 現在の入力上限を返す。 */
    usize MaxTextBytes() const noexcept { return m_MaxTextBytes; }

private:
    /** selection anchor を保ったまま cursor を 1 コードポイント左へ動かす。 */
    void ExtendSelectionLeft() noexcept {
        NormalizeSelection();
        const FString& current = text.Get();
        m_CursorByteOffset =
            PreviousBoundary(current.Data(), current.Size(), m_CursorByteOffset);
        m_CursorFollowsEnd = (m_CursorByteOffset == current.Size());
        NormalizeSelection();
    }

    /** selection anchor を保ったまま cursor を 1 コードポイント右へ動かす。 */
    void ExtendSelectionRight() noexcept {
        NormalizeSelection();
        const FString& current = text.Get();
        m_CursorByteOffset =
            NextBoundary(current.Data(), current.Size(), m_CursorByteOffset);
        m_CursorFollowsEnd = (m_CursorByteOffset == current.Size());
        NormalizeSelection();
    }

    /** selection anchor を保ったまま cursor を先頭へ動かす。 */
    void ExtendSelectionToStart() noexcept {
        NormalizeSelection();
        m_CursorByteOffset = 0;
        m_CursorFollowsEnd = false;
        NormalizeSelection();
    }

    /** selection anchor を保ったまま cursor を末尾へ動かす。 */
    void ExtendSelectionToEnd() noexcept {
        NormalizeSelection();
        m_CursorByteOffset = text.Get().Size();
        m_CursorFollowsEnd = true;
        NormalizeSelection();
    }

    /** continuation byte かを判定する。 */
    static bool IsContinuation(u8 byte) noexcept {
        return (byte & 0xC0u) == 0x80u;
    }

    /**
     * data[offset] から始まる正規 UTF-8 列の長さを返す。
     *
     * @details 不正列は安全に 1 byte として扱い、外部 binding が壊れた UTF-8 を渡しても
     * cursor traversal が停止・範囲外読みしないようにする。
     */
    static usize SequenceSize(const char* data, usize size, usize offset) noexcept {
        if (!data || offset >= size) return 0;
        const u8 b0 = static_cast<u8>(data[offset]);
        if (b0 < 0x80u) return 1;
        const usize remaining = size - offset;
        if (b0 >= 0xC2u && b0 <= 0xDFu && remaining >= 2u &&
            IsContinuation(static_cast<u8>(data[offset + 1u]))) {
            return 2;
        }
        if (b0 >= 0xE0u && b0 <= 0xEFu && remaining >= 3u) {
            const u8 b1 = static_cast<u8>(data[offset + 1u]);
            const u8 b2 = static_cast<u8>(data[offset + 2u]);
            const bool first_ok = b0 == 0xE0u ? (b1 >= 0xA0u && b1 <= 0xBFu)
                                : b0 == 0xEDu ? (b1 >= 0x80u && b1 <= 0x9Fu)
                                             : IsContinuation(b1);
            if (first_ok && IsContinuation(b2)) return 3;
        }
        if (b0 >= 0xF0u && b0 <= 0xF4u && remaining >= 4u) {
            const u8 b1 = static_cast<u8>(data[offset + 1u]);
            const u8 b2 = static_cast<u8>(data[offset + 2u]);
            const u8 b3 = static_cast<u8>(data[offset + 3u]);
            const bool first_ok = b0 == 0xF0u ? (b1 >= 0x90u && b1 <= 0xBFu)
                                : b0 == 0xF4u ? (b1 >= 0x80u && b1 <= 0x8Fu)
                                             : IsContinuation(b1);
            if (first_ok && IsContinuation(b2) && IsContinuation(b3)) return 4;
        }
        return 1;
    }

    static usize NextBoundary(const char* data, usize size, usize offset) noexcept {
        if (offset >= size) return size;
        const usize sequence_size = SequenceSize(data, size, offset);
        return sequence_size > size - offset ? size : offset + sequence_size;
    }

    static usize BoundaryAtOrBefore(const char* data, usize size, usize requested) noexcept {
        if (requested >= size) return size;
        usize offset = 0;
        while (offset < requested) {
            const usize next = NextBoundary(data, size, offset);
            if (next > requested || next <= offset) break;
            offset = next;
        }
        return offset;
    }

    static usize PreviousBoundary(const char* data, usize size, usize offset) noexcept {
        if (offset == 0) return 0;
        if (offset > size) offset = size;
        usize previous = 0;
        usize current = 0;
        while (current < offset) {
            previous = current;
            const usize next = NextBoundary(data, size, current);
            if (next >= offset || next <= current) break;
            current = next;
        }
        return previous;
    }

    static usize EncodeCodepoint(u32 codepoint, char out[4]) noexcept {
        if (!out || codepoint < 0x20u || (codepoint >= 0x7Fu && codepoint <= 0x9Fu) ||
            codepoint == 0x2028u || codepoint == 0x2029u ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu) || codepoint > 0x10FFFFu) {
            return 0;
        }
        if (codepoint <= 0x7Fu) {
            out[0] = static_cast<char>(codepoint);
            return 1;
        }
        if (codepoint <= 0x7FFu) {
            out[0] = static_cast<char>(0xC0u | (codepoint >> 6u));
            out[1] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
            return 2;
        }
        if (codepoint <= 0xFFFFu) {
            out[0] = static_cast<char>(0xE0u | (codepoint >> 12u));
            out[1] = static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu));
            out[2] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
            return 3;
        }
        out[0] = static_cast<char>(0xF0u | (codepoint >> 18u));
        out[1] = static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu));
        out[3] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
        return 4;
    }

    usize NormalizedCursorOffset() const noexcept {
        const FString& current = text.Get();
        if (m_CursorFollowsEnd) {
            return current.Size();
        }
        return BoundaryAtOrBefore(current.Data(), current.Size(),
                                  m_CursorByteOffset);
    }

    usize NormalizedSelectionAnchorOffset() const noexcept {
        const FString& current = text.Get();
        if (m_SelectionAnchorFollowsEnd) {
            return current.Size();
        }
        return BoundaryAtOrBefore(current.Data(), current.Size(),
                                  m_SelectionAnchorByteOffset);
    }

    void NormalizeSelection() noexcept {
        m_CursorByteOffset = NormalizedCursorOffset();
        m_SelectionAnchorByteOffset = NormalizedSelectionAnchorOffset();
        if (m_CursorByteOffset == m_SelectionAnchorByteOffset) {
            m_SelectionAnchorFollowsEnd = m_CursorFollowsEnd;
        }
    }

    bool TryEraseRange(usize erase_begin, usize erase_end) noexcept {
        const FString& current = text.Get();
        const usize current_size = current.Size();
        if (erase_begin >= erase_end || erase_end > current_size) return false;

        const usize new_size = current_size - (erase_end - erase_begin);
        FString staged(*current.GetAllocator());
        if (!staged.TryReserve(new_size)) return false;
        if (!staged.TryAppend(FStringView(current.Data(), erase_begin))) return false;
        if (!staged.TryAppend(FStringView(current.Data() + erase_end,
                                          current_size - erase_end))) {
            return false;
        }

        m_CursorByteOffset = erase_begin;
        m_CursorFollowsEnd = (erase_begin == new_size);
        m_SelectionAnchorByteOffset = erase_begin;
        m_SelectionAnchorFollowsEnd = m_CursorFollowsEnd;
        ui_detail::FWidgetCallbackLifetimeGuard lifetime(*this);
        text.Set(Move(staged));
        if (!lifetime.IsAlive()) return true;
        NormalizeSelection();
        return true;
    }

    usize m_CursorByteOffset = 0;
    usize m_SelectionAnchorByteOffset = 0;
    usize m_MaxTextBytes = kDefaultMaxTextBytes;
    bool m_CursorFollowsEnd = true;
    bool m_SelectionAnchorFollowsEnd = true;

    /**
     * 3 引数版から仮想 2 引数版へ転送中の modifier snapshot。
     *
     * @details callback の動的スコープ内だけ有効で、保存・所有しない。入れ子呼び出しでは
     * 呼び出し元の値へ復元する。UI OnKey は同一 widget へ並行呼び出ししない契約で使う。
     */
    const FUiKeyModifiers* m_ForwardedKeyModifiers = nullptr;
};

} // namespace acs
