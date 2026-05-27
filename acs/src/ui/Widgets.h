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

// ---- 共通 colorset (簡易テーマ) -------------------------------------------
struct FUiColors {
    FVec4 panel_bg     = { 0.16f, 0.18f, 0.24f, 0.95f };
    FVec4 panel_border = { 0.40f, 0.45f, 0.55f, 0.80f };
    FVec4 button_bg    = { 0.25f, 0.40f, 0.65f, 1.0f };
    FVec4 button_hover = { 0.35f, 0.55f, 0.85f, 1.0f };
    FVec4 button_press = { 0.18f, 0.30f, 0.50f, 1.0f };
    FVec4 button_text  = { 1.0f, 1.0f, 1.0f, 1.0f };
    FVec4 slider_track = { 0.20f, 0.20f, 0.25f, 1.0f };
    FVec4 slider_fill  = { 0.45f, 0.65f, 0.95f, 1.0f };
    FVec4 slider_knob  = { 0.85f, 0.90f, 1.0f, 1.0f };
    FVec4 check_box    = { 0.20f, 0.22f, 0.28f, 1.0f };
    FVec4 check_mark   = { 0.45f, 0.85f, 0.45f, 1.0f };
    FVec4 text         = { 1.0f, 1.0f, 1.0f, 1.0f };
    FVec4 text_dim     = { 0.7f, 0.75f, 0.85f, 1.0f };
    FVec4 input_bg     = { 0.10f, 0.12f, 0.16f, 1.0f };
    FVec4 input_focus  = { 0.20f, 0.30f, 0.50f, 1.0f };
};

inline FUiColors& DefaultUiColors() noexcept {
    static FUiColors s;
    return s;
}

// ============================================================================
// Label — 静的テキスト
// ============================================================================
class Label : public Widget {
public:
    explicit Label(const char* initial = "") noexcept : text(FString{initial}) {
        requested.h = 22.0f;
    }

    Observable<FString> text;

    void Render(FUiRenderer& r) noexcept override;
};

// ============================================================================
// Button — 押せる
// ============================================================================
class Button : public Widget {
public:
    explicit Button(const char* label = "Button") noexcept : text(FString{label}) {
        requested.h = 32.0f;
    }

    Observable<FString> text;

    // クリック完了時 (Down + Up が widget 内で完結) にこの Observable が発火
    // Set(true) → Subscribe している箇所が呼ばれる → 自動で false に戻す
    Observable<bool> clicked{ false };

    void Render(FUiRenderer& r) noexcept override;
    void OnPointerDown(f32 /*px*/, f32 /*py*/) noexcept override { pressed = true; }
    void OnPointerUp  (f32 px, f32 py) noexcept override {
        if (pressed && rect.Contains(px, py)) {
            clicked.Set(true);
            clicked.Set(false);  // pulse 仕様 (Subscribe 側は true のときのみ反応)
        }
        pressed = false;
    }
};

// ============================================================================
// Slider — 範囲付きの値編集
// ============================================================================
class Slider : public Widget {
public:
    Slider(f32 min_v = 0, f32 max_v = 1) noexcept : min_value(min_v), max_value(max_v) {
        requested.h = 24.0f;
    }

    Observable<f32> value{ 0.0f };
    f32 min_value = 0.0f;
    f32 max_value = 1.0f;

    void Render(FUiRenderer& r) noexcept override;
    void OnPointerDown(f32 px, f32 /*py*/) noexcept override { pressed = true; UpdateFromMouse(px); }
    void OnPointerMove(f32 px, f32 /*py*/) noexcept override { if (pressed) UpdateFromMouse(px); }
    void OnPointerUp  (f32 /*px*/, f32 /*py*/) noexcept override { pressed = false; }

private:
    void UpdateFromMouse(f32 px) noexcept {
        if (rect.w <= 0) return;
        f32 t = (px - rect.x) / rect.w;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        value.Set(min_value + t * (max_value - min_value));
    }
};

// ============================================================================
// Checkbox — bool トグル
// ============================================================================
class Checkbox : public Widget {
public:
    explicit Checkbox(const char* label = "") noexcept : text(FString{label}) {
        requested.h = 24.0f;
    }

    Observable<bool>   checked{ false };
    Observable<FString> text;

    void Render(FUiRenderer& r) noexcept override;
    void OnPointerUp(f32 px, f32 py) noexcept override {
        if (rect.Contains(px, py)) checked.Set(!checked.Get());
    }
};

// ============================================================================
// TextInput — 1 行文字入力 (UTF-8、ASCII 範囲のみ対応)
// ============================================================================
class TextInput : public Widget {
public:
    TextInput() noexcept {
        requested.h = 28.0f;
    }

    Observable<FString> text{ FString{} };

    void Render(FUiRenderer& r) noexcept override;
    void OnPointerDown(f32 /*px*/, f32 /*py*/) noexcept override { focused = true; }
    void OnTextInput(u32 codepoint) noexcept override {
        if (!focused) return;
        if (codepoint < 32) return;     // 制御文字無視 (ASCII)
        // 簡易: ASCII 範囲だけサポート
        if (codepoint > 0x7F) return;
        char buf[2] = { static_cast<char>(codepoint), 0 };
        FString cur = text.Get();
        // 末尾追加 (簡易、cursor 管理はしない)
        const usize len = 0;  // FString の length getter が無いので空文字列扱い → 前の値の末尾追加は別途
        (void)len;
        // 仮実装: 単純連結
        // ACS FString は AppendFormat 等あるが、ここでは新規 FString を作って差し替える
        // Container/String.h の API に応じて要調整
        char tmp[256];
        const char* src = cur.Data() ? cur.Data() : "";
        usize i = 0;
        while (i + 2 < sizeof(tmp) && src[i]) { tmp[i] = src[i]; ++i; }
        tmp[i++] = static_cast<char>(codepoint);
        tmp[i]   = 0;
        text.Set(FString{tmp});
    }
    void OnKey(i32 key, bool pressed_) noexcept override {
        if (!focused || !pressed_) return;
        // Backspace = 0x08 (実装は ASCII 想定 backspace)
        if (key == 0x08) {
            const char* src = text.Get().Data();
            if (!src || !*src) return;
            char tmp[256] = {};
            usize n = 0;
            while (src[n]) { tmp[n] = src[n]; ++n; }
            if (n > 0) tmp[n - 1] = 0;
            text.Set(FString{tmp});
        }
    }
};

} // namespace acs
