// SPDX-License-Identifier: Apache-2.0
// Widget — ACS UI フレームワークの基底クラス
//
// 設計:
//   ・retained-mode UI (ImGui のような毎フレーム再構築でなく、ツリーを保持)
//   ・MVVM 駆動: 各 Widget は Observable<T> プロパティを公開し、FViewModel と Bind 可能
//   ・FSpriteBatch + Font で描画 (Diligent / Dx12 を意識しない)
//   ・親 → 子の所有を TUniquePtr<Widget> で表現、Add で子を取り込む
//   ・Layout は親の Layout モードに応じて子に再帰的に配置
//
// 使い方:
//   StackPanel root;
//   root.SetPadding(8);
//   auto* btn = root.Add<Button>("OK");
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

/**
 * UI 座標系の矩形 (左上原点・ピクセル単位)。
 *
 * @details widget のレイアウト結果や要望サイズの保持に使う。
 */
struct FUiRect {
    /** 左上 X 座標 (px)。 */
    f32 x = 0;

    /** 左上 Y 座標 (px)。 */
    f32 y = 0;

    /** 幅 (px)。 */
    f32 w = 0;

    /** 高さ (px)。 */
    f32 h = 0;

    /**
     * 点が矩形内 (右辺・下辺は排他) かを返す。
     *
     * @param px 判定する点の X 座標。
     * @param py 判定する点の Y 座標。
     * @return [x, x+w) × [y, y+h) に含まれれば true。
     */
    bool Contains(f32 px, f32 py) const noexcept {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

/**
 * StackPanel の子整列方向。
 */
enum class EStackDir : u8 {
    /** 縦方向 (上から下) に並べる。 */
    Vertical,

    /** 横方向 (左から右) に並べる。 */
    Horizontal
};

/**
 * 矩形の内側余白 (左・上・右・下)。
 */
struct FUiPadding {
    /** 左余白 (px)。 */
    f32 l = 0;

    /** 上余白 (px)。 */
    f32 t = 0;

    /** 右余白 (px)。 */
    f32 r = 0;

    /** 下余白 (px)。 */
    f32 b = 0;
};

/**
 * アンカー (Unity RectTransform 風): 親矩形に対する正規化アンカー + ピクセルオフセット。
 *
 * @details
 * 子の各辺を「親矩形の正規化点 (anchor) からのピクセルオフセット」で決める。
 * これにより画面 / 親のリサイズに対して **レスポンシブ** に追従する。
 *   child.left   = parent.x + min.x*parent.w + offset_l
 *   child.top    = parent.y + min.y*parent.h + offset_t
 *   child.right  = parent.x + max.x*parent.w + offset_r
 *   child.bottom = parent.y + max.y*parent.h + offset_b
 * min==max (点アンカー) なら固定サイズ配置、min!=max ならその軸が親に追従して伸縮する。
 * 既定は「親全体を埋める」(min=0, max=1, offset=0)。プリセットは下の static ヘルパ参照。
 */
struct FUiAnchor {
    /** アンカー最小点 (正規化 [0,1]、親矩形の左上=0 / 右下=1)。 */
    FVec2 min{ 0.0f, 0.0f };

    /** アンカー最大点 (正規化 [0,1])。min と等しいと点アンカー (固定サイズ)。 */
    FVec2 max{ 1.0f, 1.0f };

    /** 左辺オフセット (px、min.x のアンカー点から右が正)。 */
    f32 offset_l = 0.0f;

    /** 上辺オフセット (px、min.y のアンカー点から下が正)。 */
    f32 offset_t = 0.0f;

    /** 右辺オフセット (px、max.x のアンカー点から右が正、内側マージンは負)。 */
    f32 offset_r = 0.0f;

    /** 下辺オフセット (px、max.y のアンカー点から下が正、内側マージンは負)。 */
    f32 offset_b = 0.0f;

    /**
     * 点アンカー: 親の正規化点 ap に対し、サイズ (w,h) の矩形を (ox,oy) ずらして置く。
     *
     * @details 矩形の左上 = アンカー点 + (ox,oy)。角・端固定 UI (HUD 等) に使う。
     * @param ap 親矩形上の正規化アンカー点 ([0,0]=左上, [1,1]=右下, [0.5,0.5]=中央)。
     * @param w 矩形の幅 (px)。
     * @param h 矩形の高さ (px)。
     * @param ox X オフセット (px、既定 0)。
     * @param oy Y オフセット (px、既定 0)。
     * @return 構築した FUiAnchor。
     */
    static FUiAnchor Point(FVec2 ap, f32 w, f32 h, f32 ox = 0.0f, f32 oy = 0.0f) noexcept {
        FUiAnchor a;
        a.min = ap; a.max = ap;
        a.offset_l = ox;       a.offset_t = oy;
        a.offset_r = ox + w;   a.offset_b = oy + h;
        return a;
    }

    /**
     * 親中央に固定サイズで配置する点アンカー。
     *
     * @param w 幅 (px)。
     * @param h 高さ (px)。
     * @return アンカー点 (0.5,0.5)・矩形中心が親中心に来る FUiAnchor。
     */
    static FUiAnchor Centered(f32 w, f32 h) noexcept {
        FUiAnchor a;
        a.min = { 0.5f, 0.5f }; a.max = { 0.5f, 0.5f };
        a.offset_l = -w * 0.5f; a.offset_t = -h * 0.5f;
        a.offset_r =  w * 0.5f; a.offset_b =  h * 0.5f;
        return a;
    }

    /**
     * 親全体を四辺マージン付きで埋めるストレッチアンカー。
     *
     * @param l 左マージン (px)。
     * @param t 上マージン (px)。
     * @param r 右マージン (px)。
     * @param b 下マージン (px)。
     * @return 親に追従して伸縮する FUiAnchor。
     */
    static FUiAnchor Stretch(f32 l = 0.0f, f32 t = 0.0f, f32 r = 0.0f, f32 b = 0.0f) noexcept {
        FUiAnchor a;
        a.min = { 0.0f, 0.0f }; a.max = { 1.0f, 1.0f };
        a.offset_l =  l; a.offset_t =  t;
        a.offset_r = -r; a.offset_b = -b;
        return a;
    }
};

/**
 * 親矩形 + アンカーから子の絶対矩形を算出する。
 *
 * @details FUiAnchor の式をそのまま適用。負幅 / 負高さは 0 にクランプしない (呼び出し側責務)。
 * @param parent 親の絶対矩形。
 * @param a 子のアンカー設定。
 * @return 子の絶対矩形 (left/top/width/height)。
 */
inline FUiRect ComputeAnchoredRect(const FUiRect& parent, const FUiAnchor& a) noexcept {
    const f32 ax0 = parent.x + a.min.x * parent.w;
    const f32 ay0 = parent.y + a.min.y * parent.h;
    const f32 ax1 = parent.x + a.max.x * parent.w;
    const f32 ay1 = parent.y + a.max.y * parent.h;
    const f32 left   = ax0 + a.offset_l;
    const f32 top    = ay0 + a.offset_t;
    const f32 right  = ax1 + a.offset_r;
    const f32 bottom = ay1 + a.offset_b;
    return { left, top, right - left, bottom - top };
}

/**
 * retained-mode UI ツリーの基底ウィジェット。
 *
 * @details
 * 親が TUniquePtr<Widget> で子を所有し、Layout (親が呼ぶ) / Render (FUiRenderer が呼ぶ) /
 * 入力イベント (On*) を仮想メソッドで提供する。各派生は Observable<T> プロパティを公開して
 * MVVM の FViewModel と Bind できる。非コピー。
 */
class Widget {
public:
    /** 空のウィジェットを構築する (親なし・子なし)。 */
    Widget() noexcept = default;

    /** 派生を正しく破棄するための仮想デストラクタ。 */
    virtual ~Widget() noexcept = default;

    /** コピー禁止 (子を TUniquePtr で単独所有するため)。 */
    Widget(const Widget&) = delete;

    /** コピー代入も禁止。 */
    Widget& operator=(const Widget&) = delete;

    /**
     * W 型の子ウィジェットを構築・追加して所有し、生ポインタを返す。
     *
     * @tparam W 追加する Widget 派生型。
     * @tparam Args W のコンストラクタ引数型。
     * @param args W のコンストラクタへ転送する引数。
     * @return 追加した子 W への生ポインタ (所有はツリー側)。
     */
    template<typename W, typename... Args>
    W* Add(Args&&... args) noexcept {
        auto u = MakeUnique<W>(Forward<Args>(args)...);
        W* const raw = u.Get();
        raw->m_Parent = this;
        m_Children.PushBack(Move(u));
        return raw;
    }

    /**
     * 親ウィジェットを返す。
     *
     * @return 親 (root なら nullptr)。
     */
    Widget* Parent() const noexcept { return m_Parent; }

    /**
     * 直接の子の数を返す。
     *
     * @return 子の数。
     */
    usize   ChildCount() const noexcept { return m_Children.Size(); }

    /**
     * i 番目の子を返す。
     *
     * @param i 子のインデックス。
     * @return i 番目の子 (範囲外なら nullptr)。
     */
    Widget* Child(usize i) const noexcept { return i < m_Children.Size() ? m_Children[i].Get() : nullptr; }

    /** 表示フラグ (false で自身と子の描画・hit-test をスキップ)。 */
    bool   visible = true;

    /** レイアウト後に確定する絶対座標の矩形。 */
    FUiRect rect;

    /** 要望サイズ (0 の成分はレイアウトに任せる)。 */
    FUiRect requested;

    /** アンカー設定 (AnchorPanel の子のときのみ使われる。既定は親全体を埋める)。 */
    FUiAnchor anchor;

    /** ポインタが上にあるか (FUiInput が更新)。 */
    bool hovered = false;

    /** 入力フォーカス中か (TextInput 等で使う)。 */
    bool focused = false;

    /** 直近フレームで押下中か (Button 等で使う)。 */
    bool pressed = false;

    /**
     * 自身の矩形を確定する (親がレイアウト時に呼ぶ)。
     *
     * @details 既定は与えられた矩形をそのまま rect に設定する。子配置は派生で override する。
     * @param x 左上 X 座標 (px)。
     * @param y 左上 Y 座標 (px)。
     * @param w 幅 (px)。
     * @param h 高さ (px)。
     */
    virtual void Layout(f32 x, f32 y, f32 w, f32 h) noexcept {
        rect = { x, y, w, h };
    }

    /**
     * 自身を描画する (FUiRenderer が呼ぶ)。
     *
     * @details 既定は visible な子を再帰的に描画するだけ。見た目を持つ派生で override する。
     * @param r 描画ヘルパとテーマ色を提供する UI レンダラ。
     */
    virtual void Render(class FUiRenderer& r) noexcept {
        // 既定は子だけ描画する (visible なものに絞り)
        for (usize i = 0; i < m_Children.Size(); ++i) {
            if (m_Children[i] && m_Children[i]->visible) m_Children[i]->Render(r);
        }
    }

    /**
     * 点が自身にヒットするかを判定する (入力 hit-test)。
     *
     * @details 既定は自身の rect 内判定。true なら自身が入力を拾い、false なら通過する。
     * @param px 判定する点の X 座標。
     * @param py 判定する点の Y 座標。
     * @return ヒットすれば true。
     */
    virtual bool HitTest(f32 px, f32 py) const noexcept {
        return rect.Contains(px, py);
    }

    /**
     * ポインタ押下イベント。
     *
     * @details 既定は何もしない。Button/Slider 等が override する。
     * @param px 押下点の X 座標。
     * @param py 押下点の Y 座標。
     */
    virtual void OnPointerDown(f32 /*px*/, f32 /*py*/) noexcept {}

    /**
     * ポインタ解放イベント。
     *
     * @details 既定は何もしない。クリック確定判定等に override する。
     * @param px 解放点の X 座標。
     * @param py 解放点の Y 座標。
     */
    virtual void OnPointerUp  (f32 /*px*/, f32 /*py*/) noexcept {}

    /**
     * ポインタ移動イベント。
     *
     * @details 既定は何もしない。drag 追従等に override する。
     * @param px 現在の X 座標。
     * @param py 現在の Y 座標。
     */
    virtual void OnPointerMove(f32 /*px*/, f32 /*py*/) noexcept {}

    /**
     * 文字入力イベント (確定後の codepoint)。
     *
     * @details 既定は何もしない。TextInput が override する。
     * @param codepoint 入力された Unicode コードポイント。
     */
    virtual void OnTextInput  (u32 /*codepoint*/) noexcept {}

    /**
     * キーイベント。
     *
     * @details 既定は何もしない。Backspace 等の編集キー処理に override する。
     * @param key 制御コードに対応付けられたキーコード。
     * @param pressed 押下なら true、解放なら false。
     */
    virtual void OnKey        (i32 /*key*/, bool /*pressed*/) noexcept {}

    /**
     * 子を含めて再帰的に hit-test し、最前面のヒット widget を返す。
     *
     * @details 描画順で後ろ (= 手前に描かれる) の子から優先的に走査し、最初にヒットした
     * 子孫を返す。子がヒットしなければ自身を HitTest して判定する。
     * @param px 判定する点の X 座標。
     * @param py 判定する点の Y 座標。
     * @return ヒットした最前面の widget (なければ nullptr)。
     */
    Widget* HitTestRecursive(f32 px, f32 py) noexcept {
        if (!visible) return nullptr;
        // 後ろの子 (上に描画されてる) から優先的にヒット
        for (usize i = m_Children.Size(); i > 0; --i) {
            Widget* const c = m_Children[i - 1].Get();
            if (c) {
                if (Widget* h = c->HitTestRecursive(px, py)) return h;
            }
        }
        return HitTest(px, py) ? this : nullptr;
    }

protected:
    /** 親ウィジェット (root なら nullptr、所有はしない)。 */
    Widget*                       m_Parent   = nullptr;

    /** 直接の子 (所有権を持つ、描画/走査順は配列順)。 */
    TArray<TUniquePtr<Widget>>      m_Children;
};

/**
 * 子を縦または横に等間隔で並べるレイアウトパネル。
 *
 * @details dir に応じて子を順に配置し、各子の要望サイズ (なければ既定値) を使う。
 */
class StackPanel : public Widget {
public:
    /** 既定設定 (縦並び・spacing=4・padding=8) で構築する。 */
    StackPanel() noexcept = default;

    /** 子を並べる方向。 */
    EStackDir   dir      = EStackDir::Vertical;

    /** 隣り合う子の間隔 (px)。 */
    f32        spacing  = 4.0f;

    /** パネル内側の余白。 */
    FUiPadding  padding{ 8, 8, 8, 8 };

    /**
     * 子を dir 方向に順に配置する。
     *
     * @details
     * padding で内側を縮めた領域に、縦並びなら各子の要望高さ (なければ 24px)、横並びなら
     * 要望幅 (なければ 80px) を使って spacing 間隔で並べる。非 visible な子は飛ばす。
     * @param x 左上 X 座標 (px)。
     * @param y 左上 Y 座標 (px)。
     * @param w 幅 (px)。
     * @param h 高さ (px)。
     */
    void Layout(f32 x, f32 y, f32 w, f32 h) noexcept override {
        rect = { x, y, w, h };
        f32 cx = x + padding.l;
        f32 cy = y + padding.t;
        const f32 cw = w - padding.l - padding.r;
        const f32 ch = h - padding.t - padding.b;

        for (usize i = 0; i < m_Children.Size(); ++i) {
            Widget* const c = m_Children[i].Get();
            if (!c || !c->visible) continue;

            if (dir == EStackDir::Vertical) {
                const f32 child_h = c->requested.h > 0 ? c->requested.h : 24.0f;
                c->Layout(cx, cy, cw, child_h);
                cy += child_h + spacing;
            } else {
                const f32 child_w = c->requested.w > 0 ? c->requested.w : 80.0f;
                c->Layout(cx, cy, child_w, ch);
                cx += child_w + spacing;
            }
        }
    }
};

/**
 * 自身は描画せず、子に自分と同じ範囲を渡すだけの透過パネル。
 *
 * @details 同じ領域を複数の子に重ねて配置したいとき (オーバーレイ等) に使う。
 */
class Container : public Widget {
public:
    /**
     * 自身の rect を確定し、全 visible な子に同じ範囲を渡す。
     *
     * @param x 左上 X 座標 (px)。
     * @param y 左上 Y 座標 (px)。
     * @param w 幅 (px)。
     * @param h 高さ (px)。
     */
    void Layout(f32 x, f32 y, f32 w, f32 h) noexcept override {
        rect = { x, y, w, h };
        for (usize i = 0; i < m_Children.Size(); ++i) {
            Widget* const c = m_Children[i].Get();
            if (c && c->visible) c->Layout(x, y, w, h);
        }
    }
};

/**
 * 各子を自身の anchor (RectTransform 風) に従って配置するレスポンシブパネル。
 *
 * @details
 * Container が全子に同じ矩形を渡すのに対し、AnchorPanel は子ごとの `anchor` を
 * ComputeAnchoredRect で解決して個別配置する。パネルの rect が変わる (画面リサイズ等)
 * と全子が追従するため、HUD やオーバーレイの解像度非依存レイアウトに使う。
 *
 * 使い方:
 *   AnchorPanel hud;
 *   auto* score = hud.Add<Label>("0");
 *   score->anchor = FUiAnchor::Point({1,0}, 120, 32, -128, 8);  // 右上に固定サイズ
 *   auto* bar = hud.Add<Container>();
 *   bar->anchor = FUiAnchor::Stretch(16, 0, 16, 8);             // 下端を左右いっぱい
 *   hud.Layout(0, 0, screen_w, screen_h);
 */
class AnchorPanel : public Widget {
public:
    /**
     * 自身の rect を確定し、各 visible な子を anchor に従って配置する。
     *
     * @param x 左上 X 座標 (px)。
     * @param y 左上 Y 座標 (px)。
     * @param w 幅 (px)。
     * @param h 高さ (px)。
     */
    void Layout(f32 x, f32 y, f32 w, f32 h) noexcept override {
        rect = { x, y, w, h };
        for (usize i = 0; i < m_Children.Size(); ++i) {
            Widget* const c = m_Children[i].Get();
            if (!c || !c->visible) continue;
            const FUiRect cr = ComputeAnchoredRect(rect, c->anchor);
            c->Layout(cr.x, cr.y, cr.w, cr.h);
        }
    }
};

} // namespace acs
