// SPDX-License-Identifier: Apache-2.0
// FWidget — ACS UI フレームワークの基底クラス
//
// 設計:
//   ・retained-mode UI (ImGui のような毎フレーム再構築でなく、ツリーを保持)
//   ・MVVM 駆動: 各 FWidget は FObservable<T> プロパティを公開し、FViewModel と Bind 可能
//   ・FSpriteBatch + FFont で描画 (Diligent / Dx12 を意識しない)
//   ・親 → 子の所有を TUniquePtr<FWidget> で表現、Add で子を取り込む
//   ・Layout は親の Layout モードに応じて子に再帰的に配置
//
// 使い方:
//   FStackPanel root;
//   root.SetPadding(8);
//   auto* btn = root.Add<FButton>("OK");
//   btn->on_clicked.Subscribe(...);
//
//   // 毎フレーム:
//   ui_renderer.Render(root, *cmd, screen_w, screen_h);
//   ui_input.Dispatch(root);
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"

namespace acs {

// 矩形 (UI 座標、左上原点ピクセル単位)
struct FUiRect {
    f32 x = 0, y = 0, w = 0, h = 0;
    bool Contains(f32 px, f32 py) const noexcept {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// 整列方向
enum class EStackDir : u8 { Vertical, Horizontal };

// アンカー / 余白
struct FUiPadding { f32 l = 0, t = 0, r = 0, b = 0; };

// FWidget の基底
class FWidget {
public:
    FWidget() noexcept = default;
    virtual ~FWidget() noexcept = default;

    FWidget(const FWidget&) = delete;
    FWidget& operator=(const FWidget&) = delete;

    // ---- 子ウィジェットの管理 -------------------------------------------
    template<typename W, typename... Args>
    W* Add(Args&&... args) noexcept {
        auto u = MakeUnique<W>(Forward<Args>(args)...);
        W* raw = u.Get();
        raw->_parent = this;
        _children.PushBack(Move(u));
        return raw;
    }

    FWidget* Parent() const noexcept { return _parent; }
    usize   ChildCount() const noexcept { return _children.Size(); }
    FWidget* Child(usize i) const noexcept { return i < _children.Size() ? _children[i].Get() : nullptr; }

    // ---- 表示 ----
    bool   visible = true;
    FUiRect rect;                    // レイアウト後に確定する絶対座標
    FUiRect requested;               // 要望サイズ (0 = レイアウトに任せる)

    // ---- フォーカス / hover ----
    bool hovered = false;
    bool focused = false;
    bool pressed = false;           // 直近のフレームで押下されているか (FButton 等で使う)

    // ---- 仮想メソッド: Layout は親が呼ぶ。Render は FUiRenderer が呼ぶ ----
    virtual void Layout(f32 x, f32 y, f32 w, f32 h) noexcept {
        rect = { x, y, w, h };
    }

    virtual void Render(class FUiRenderer& r) noexcept {
        // 既定は子だけ描画する (visible なものに絞り)
        for (usize i = 0; i < _children.Size(); ++i) {
            if (_children[i] && _children[i]->visible) _children[i]->Render(r);
        }
    }

    // 入力が来たときの hit-test (true なら自身が拾う、false なら通過)
    virtual bool HitTest(f32 px, f32 py) const noexcept {
        return rect.Contains(px, py);
    }

    // クリック / drag / 文字入力等のイベント処理。デフォは何もしない。
    virtual void OnPointerDown(f32 /*px*/, f32 /*py*/) noexcept {}
    virtual void OnPointerUp  (f32 /*px*/, f32 /*py*/) noexcept {}
    virtual void OnPointerMove(f32 /*px*/, f32 /*py*/) noexcept {}
    virtual void OnTextInput  (u32 /*codepoint*/) noexcept {}
    virtual void OnKey        (i32 /*key*/, bool /*pressed*/) noexcept {}

    // 子を含めた hit test (subclass で再帰的にヒットテストする補助)
    FWidget* HitTestRecursive(f32 px, f32 py) noexcept {
        if (!visible) return nullptr;
        // 後ろの子 (上に描画されてる) から優先的にヒット
        for (usize i = _children.Size(); i > 0; --i) {
            FWidget* c = _children[i - 1].Get();
            if (c) {
                if (FWidget* h = c->HitTestRecursive(px, py)) return h;
            }
        }
        return HitTest(px, py) ? this : nullptr;
    }

protected:
    FWidget*                       _parent   = nullptr;
    TArray<TUniquePtr<FWidget>>      _children;
};

// ---------- レイアウト系 ----------

// FStackPanel: 縦 or 横にきれいに並べる
class FStackPanel : public FWidget {
public:
    FStackPanel() noexcept = default;

    EStackDir   dir      = EStackDir::Vertical;
    f32        spacing  = 4.0f;
    FUiPadding  padding{ 8, 8, 8, 8 };

    void Layout(f32 x, f32 y, f32 w, f32 h) noexcept override {
        rect = { x, y, w, h };
        f32 cx = x + padding.l;
        f32 cy = y + padding.t;
        f32 cw = w - padding.l - padding.r;
        f32 ch = h - padding.t - padding.b;

        for (usize i = 0; i < _children.Size(); ++i) {
            FWidget* c = _children[i].Get();
            if (!c || !c->visible) continue;

            if (dir == EStackDir::Vertical) {
                f32 child_h = c->requested.h > 0 ? c->requested.h : 24.0f;
                c->Layout(cx, cy, cw, child_h);
                cy += child_h + spacing;
            } else {
                f32 child_w = c->requested.w > 0 ? c->requested.w : 80.0f;
                c->Layout(cx, cy, child_w, ch);
                cx += child_w + spacing;
            }
        }
    }
};

// FContainer: 自身は描画せず、レイアウトだけ親と同じ範囲を子に渡す (透過パネル)
class FContainer : public FWidget {
public:
    void Layout(f32 x, f32 y, f32 w, f32 h) noexcept override {
        rect = { x, y, w, h };
        for (usize i = 0; i < _children.Size(); ++i) {
            FWidget* c = _children[i].Get();
            if (c && c->visible) c->Layout(x, y, w, h);
        }
    }
};

} // namespace acs
