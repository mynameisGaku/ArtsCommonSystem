// SPDX-License-Identifier: Apache-2.0
// 標準ウィジェット — Label / Button / Slider / Checkbox / TextInput
//
// すべての widget は Observable<T> プロパティで状態を公開しているので、
// MVVM の Bind* で FViewModel と直結できる:
//
//   class PlayerVM : public FViewModel { Observable<f32> hp{100}; };
//   PlayerVM vm;
//   StackPanel root;
//   auto* sl = root.Add<Slider>(0.0f, 100.0f);
//   auto bind = MakeTwoWayBind(vm.hp, sl->value);   // 双方向同期
#pragma once

#include "ui/Widget.h"
#include "container/String.h"
#include "mvvm/Observable.h"
#include "mvvm/Binder.h"   // Bind / MakeBind / MakeTwoWayBind を ui/ ユーザに直送

namespace acs {

class Font;

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
class Label : public Widget {
public:
    /**
     * 初期テキストを指定して構築する。
     *
     * @param initial 初期表示文字列 (既定は空文字列)。
     */
    explicit Label(const char* initial = "") noexcept : text(FString{initial}) {
        requested.h = 22.0f;
    }

    /** 表示する文字列 (双方向 Bind 可能)。 */
    Observable<FString> text;

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
class Button : public Widget {
public:
    /**
     * ラベル文字列を指定して構築する。
     *
     * @param label ボタンに表示する文字列 (既定 "Button")。
     */
    explicit Button(const char* label = "Button") noexcept : text(FString{label}) {
        requested.h = 32.0f;
    }

    /** ボタンのラベル文字列 (双方向 Bind 可能)。 */
    Observable<FString> text;

    /**
     * クリック完了を通知する pulse Observable。
     *
     * @details
     * Down → Up が widget 内で完結したとき true を Set した直後に false へ戻す。
     * Subscribe 側は値が true のときだけ反応する。
     */
    Observable<bool> clicked{ false };

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
            clicked.Set(true);
            clicked.Set(false);  // pulse 仕様 (Subscribe 側は true のときのみ反応)
        }
        pressed = false;
    }
};

/**
 * 範囲付きの値をドラッグで編集するスライダー widget。
 */
class Slider : public Widget {
public:
    /**
     * 値域を指定して構築する。
     *
     * @param min_v 取りうる最小値 (既定 0)。
     * @param max_v 取りうる最大値 (既定 1)。
     */
    Slider(f32 min_v = 0, f32 max_v = 1) noexcept : min_value(min_v), max_value(max_v) {
        requested.h = 24.0f;
    }

    /** 現在値 (双方向 Bind 可能)。 */
    Observable<f32> value{ 0.0f };

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
class Checkbox : public Widget {
public:
    /**
     * ラベル文字列を指定して構築する。
     *
     * @param label チェックボックス横に表示する文字列 (既定は空文字列)。
     */
    explicit Checkbox(const char* label = "") noexcept : text(FString{label}) {
        requested.h = 24.0f;
    }

    /** チェック状態 (双方向 Bind 可能)。 */
    Observable<bool>   checked{ false };

    /** 横に表示するラベル文字列 (双方向 Bind 可能)。 */
    Observable<FString> text;

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
 * 1 行のテキストを編集する入力 widget (UTF-8、ASCII 範囲のみ対応)。
 */
class TextInput : public Widget {
public:
    /** 空文字列で構築する。 */
    TextInput() noexcept {
        requested.h = 28.0f;
    }

    /** 編集中の文字列 (双方向 Bind 可能)。 */
    Observable<FString> text{ FString{} };

    /**
     * 入力欄の背景・枠・文字列を描画する。
     *
     * @param r 描画ヘルパとテーマ色を提供する UI レンダラ。
     */
    void Render(FUiRenderer& r) noexcept override;

    /**
     * クリックでフォーカスを得る。
     *
     * @param px 押下点の X 座標 (未使用)。
     * @param py 押下点の Y 座標 (未使用)。
     */
    void OnPointerDown(f32 /*px*/, f32 /*py*/) noexcept override { focused = true; }

    /**
     * フォーカス中なら入力文字を末尾に追加する (ASCII 範囲のみ)。
     *
     * @details 制御文字 (codepoint < 32) と非 ASCII (> 0x7F) は無視する。cursor 管理はせず末尾追加のみ。
     * @param codepoint 入力された Unicode コードポイント。
     */
    void OnTextInput(u32 codepoint) noexcept override {
        if (!focused) return;
        if (codepoint < 32 || codepoint > 0x7F) return;  // ASCII 印字可能文字のみ (制御文字・非 ASCII は無視)
        // 末尾に 1 文字追加する (cursor 管理はしない)。FString::Append で長さ制限なく伸ばせる。
        FString s = text.Get();
        s.Append(static_cast<char>(codepoint));
        text.Set(s);
    }
    /**
     * フォーカス中の編集キーを処理する (現状 Backspace のみ)。
     *
     * @details key==0x08 (Backspace) で末尾 1 文字を削除する。解放やフォーカス外は無視。
     * @param key 制御コードに対応付けられたキーコード。
     * @param pressed_ 押下なら true、解放なら false。
     */
    void OnKey(i32 key, bool pressed_) noexcept override {
        if (!focused || !pressed_) return;
        if (key == 0x08) {  // Backspace: 末尾 1 文字 (ASCII = 1 バイト) を削る
            const FString cur = text.Get();
            const usize n = cur.Size();
            if (n == 0) return;
            FString s;
            const char* d = cur.Data();
            for (usize i = 0; i + 1 < n; ++i) s.Append(d[i]);
            text.Set(s);
        }
    }
};

} // namespace acs
