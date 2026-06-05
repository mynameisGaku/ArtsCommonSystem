// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FNode2D
//
// シーンの中身を表す唯一のノードクラス (2D 専用、抽象 Node 基底は作らない)。
// 親子ツリーで階層的な transform を持ち、各ノードが OnSpawn/OnUpdate/OnDraw/
// OnDespawn を override してロジック・描画を書く。
//
// 設計選択:
//   ・**non-copy / non-move**: `TUniquePtr<FNode2D>` で所有、`FNode2D*` で参照。
//     `AddChild(MakeUnique<MyNode>(args))` が標準パターン。
//   ・**lifecycle**: `AddChild` 即時 `OnSpawn`、`Destroy()` で
//     `m_PendingDestroy` をマーク、フレーム境界の `ResolveStructuralChanges()`
//     で OnDespawn を呼んで TArray から除去。子ツリーが先に reap される。
//   ・**transform**: `m_Local` を真値、`World()` は親をたどってオンザフライ計算。
//   ・**iteration safety**: UpdateTree/DrawTree は index ベースで走査。
//     AddChild が走査中に呼ばれた場合の新規子は同フレームで走らせる (Unity 互換)。
//     Destroy は遅延 reap なので走査中の即時除去はしない。
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "memory/UniquePtr.h"
#include "container/Array.h"
#include "gameframework/Transform2D.h"
#include "gameframework/Component2D.h"
#include "gameframework/NodeId.h"

namespace acs::game {

class RenderContext;

/**
 * シーンの中身を表す唯一のノード (2D 専用)。
 *
 * @details
 * 親子ツリーで階層的な transform を持ち、各ノードが OnSpawn/OnUpdate/OnDraw/
 * OnDespawn を override してロジック・描画を書く。`TUniquePtr<FNode2D>` で所有し
 * `FNode2D*` で参照する non-copy / non-move 型で、`AddChild(MakeUnique<MyNode>())`
 * が標準パターン。transform は m_Local を真値とし、World() は親をたどって合成する。
 */
class FNode2D {
public:
    /** 空のノードを構築する (transform は単位、親なし)。 */
    FNode2D() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~FNode2D() noexcept = default;

    /** コピー禁止 (ノードは TUniquePtr で単独所有するため)。 */
    FNode2D(const FNode2D&)            = delete;

    /** コピー代入も禁止。 */
    FNode2D& operator=(const FNode2D&) = delete;

    /** ムーブ禁止 (親が保持するポインタの安定性を保つため)。 */
    FNode2D(FNode2D&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FNode2D& operator=(FNode2D&&)      = delete;

    /** AddChild でツリーに入った直後に 1 回呼ばれる初期化フック。 */
    virtual void OnSpawn()                noexcept {}

    /**
     * 毎フレーム呼ばれる可変刻み update フック。
     *
     * @param dt 前フレームからの経過秒。
     */
    virtual void OnUpdate(f32 /*dt*/)     noexcept {}

    /**
     * 固定刻み update フック (物理・決定論ロジック)。
     *
     * @details
     * 同フレームで 0..max_fixed_steps 回呼ばれる。FGame::SetFixedTimeStep が 0 のとき
     * (= 固定 update 無効) は呼ばれない。
     * @param fixed_dt 固定刻みの秒 (SetFixedTimeStep で指定した値)。
     */
    virtual void OnFixedUpdate(f32 /*fixed_dt*/) noexcept {}

    /**
     * 描画フック。スプライト等を積む。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    virtual void OnDraw(RenderContext& /*rc*/) noexcept {}

    /** ツリーから除去される直前に 1 回呼ばれる後始末フック。 */
    virtual void OnDespawn()              noexcept {}

    /**
     * ローカル transform への可変参照を返す (位置・回転・スケールを直接書き換える)。
     *
     * @return ローカル transform への参照。
     */
    FTransform2D&       Local()       noexcept { return m_Local; }

    /**
     * ローカル transform への const 参照を返す。
     *
     * @return ローカル transform への const 参照。
     */
    const FTransform2D& Local() const noexcept { return m_Local; }

    /**
     * 親をたどって world transform を合成して返す (オンザフライ計算、キャッシュなし)。
     *
     * @return root からこのノードまで合成した world transform。
     */
    FTransform2D World() const noexcept;

    /**
     * 有効フラグを設定する。
     *
     * @param b false なら subtree ごと update をスキップする。
     */
    void SetEnabled(bool b) noexcept { m_Enabled = b; }

    /**
     * 有効フラグを返す。
     *
     * @return 有効なら true。
     */
    bool IsEnabled() const noexcept { return m_Enabled; }

    /**
     * 可視フラグを設定する。
     *
     * @param b false なら subtree ごと描画をスキップする。
     */
    void SetVisible(bool b) noexcept { m_Visible = b; }

    /**
     * 可視フラグを返す。
     *
     * @return 可視なら true。
     */
    bool IsVisible() const noexcept { return m_Visible; }

    /**
     * 子の描画順モード。
     *
     * @details
     * 既定 Tree = 配列追加順 (従来挙動・ゼロオーバーヘッド)。見下ろしゲームの Y 遮蔽や、
     * 背景/ワールド/前景/HUD のレイヤ分離に使う。
     */
    enum class EChildDrawOrder : u8 {
        /** 配列追加順 (従来)。ソートしない。 */
        Tree       = 0,

        /** SortLayer 昇順 (低い層が奥=先に描画)。同層は配列順で安定。 */
        Layer      = 1,

        /** SortLayer 昇順 → 同層内は (world.y + YSortBias) 昇順。+Y=画面下なので小さい y を先に描画 = 見下ろし遮蔽。 */
        LayerThenY = 2,
    };

    /**
     * この node が「自分の子」を描画する順序を設定する (兄弟間のみ。木の階層は維持)。
     *
     * @param o 適用する子描画順モード。
     */
    void            SetChildDrawOrder(EChildDrawOrder o) noexcept { m_ChildOrder = o; }

    /**
     * 現在の子描画順モードを返す。
     *
     * @return 設定済みの EChildDrawOrder。
     */
    EChildDrawOrder ChildDrawOrder() const noexcept { return m_ChildOrder; }

    /**
     * この node の描画レイヤを設定する。
     *
     * @details 親が Layer/LayerThenY のとき兄弟ソートの第1キー。低いほど奥 (先に描画)。
     * @param layer 描画レイヤ (既定 0)。
     */
    void SetSortLayer(i32 layer) noexcept { m_SortLayer = layer; }

    /**
     * 描画レイヤを返す。
     *
     * @return 設定済みの描画レイヤ。
     */
    i32  SortLayer() const noexcept { return m_SortLayer; }

    /**
     * Y-sort の pivot バイアスを設定する。
     *
     * @details world.y に加算してソートキーにする。足元で遮蔽したいときは正値 (+Y=画面下=足元)。
     * @param bias world.y に加算するバイアス (既定 0)。
     */
    void SetYSortBias(f32 bias) noexcept { m_YSortBias = bias; }

    /**
     * Y-sort の pivot バイアスを返す。
     *
     * @return 設定済みのバイアス。
     */
    f32  YSortBias() const noexcept { return m_YSortBias; }

    /**
     * 親ノードを返す。
     *
     * @return 親ノード (root なら nullptr)。
     */
    FNode2D* Parent() const noexcept { return m_Parent; }

    /**
     * 直接の子の数を返す。
     *
     * @return 子の数。
     */
    u32     ChildCount() const noexcept { return static_cast<u32>(m_Children.Size()); }

    /**
     * i 番目の子を返す。
     *
     * @param i 子のインデックス。
     * @return i 番目の子 (範囲外なら nullptr)。
     */
    FNode2D* Child(u32 i) const noexcept {
        return i < m_Children.Size() ? m_Children[i].Get() : nullptr;
    }

    /**
     * 子を追加して所有権を奪い、OnSpawn を即時に呼ぶ。
     *
     * @param child 追加する子 (所有権が移る)。
     * @return 追加した子への参照 (チェイン記述用)。
     */
    FNode2D& AddChild(TUniquePtr<FNode2D> child) noexcept;

    /**
     * 自身を「破棄予定」にマークする。
     *
     * @details
     * 実際の破棄は次の ResolveStructuralChanges で起こる
     * (OnDespawn 呼出 → TArray から除去 → デストラクタで memory release)。
     */
    void Destroy() noexcept { m_PendingDestroy = true; }

    /**
     * 破棄予定フラグが立っているかを返す。
     *
     * @return 破棄予定なら true。
     */
    bool IsPendingDestroy() const noexcept { return m_PendingDestroy; }

    /**
     * 自分を `new_parent` の子に移動するよう要求する (フレーム境界で適用)。
     *
     * @details
     * new_parent == nullptr は不正 (警告ログ + 無視)、自分自身 or 子孫を指定した場合も
     * 不正 (cycle 検出、警告 + 無視)。ResolveStructuralChanges 内で m_Children TArray 間を
     * Move し、parent ポインタを書き換える。OnSpawn/OnDespawn は呼ばれない
     * (= 既に生きているノードの移動)。
     * @param new_parent 移動先の親ノード。
     */
    void Reparent(FNode2D& new_parent) noexcept;

    /**
     * 親付け替え予定が立っているかを返す。
     *
     * @return 付け替え予定なら true。
     */
    bool IsPendingReparent() const noexcept { return m_PendingReparentTarget != nullptr; }

    /**
     * ノード単位に振られる generational handle を返す。
     *
     * @details
     * Scene 内で唯一であることは保証されない (生成側が一意性を管理)。
     * default は invalid (m_Packed == 0)。
     * @return ノードの FNodeId。
     */
    FNodeId Id() const noexcept { return m_Id; }

    /**
     * ノード ID を設定する (内部用。生成側が割り当てる)。
     *
     * @param id 割り当てる FNodeId。
     */
    void   _SetId(FNodeId id) noexcept { m_Id = id; }

    /**
     * T の FComponent2D を構築・attach し、参照を返す。
     *
     * @details OnAttach は即時呼出。依存コンポーネントは OnRequire で先に確保される。
     * @tparam T 追加する FComponent2D 派生型。
     * @tparam Args T のコンストラクタ引数型。
     * @param args T のコンストラクタへ転送する引数。
     * @return attach した T への参照。
     */
    template<typename T, typename... Args>
    T& AddComponent(Args&&... args) noexcept {
        TUniquePtr<T> comp = MakeUnique<T>(Forward<Args>(args)...);
        T* ref = comp.Get();
        ref->_SetOwner(this);
        // 依存コンポーネントを先に確保 (Unity の RequireComponent 相当)。
        // 依存が m_Components に先に積まれるので、この後の OnAttach から
        // GetComponent<Dep>() で確実に取得できる。
        ref->OnRequire(*this);
        m_Components.PushBack(TUniquePtr<FComponent2D>(comp.Release(), comp.GetAllocator()));
        ref->OnAttach(*this);
        return *ref;
    }

    /**
     * T があれば返し、無ければ追加して返す (RequireComponent の自動追加に使う)。
     *
     * @tparam T 取得または追加する FComponent2D 派生型。
     * @tparam Args 新規追加時に T のコンストラクタへ渡す引数型。
     * @param args 新規追加時に T のコンストラクタへ転送する引数。
     * @return 既存または新規に追加した T への参照。
     */
    template<typename T, typename... Args>
    T& GetOrAddComponent(Args&&... args) noexcept {
        if (T* existing = GetComponent<T>()) return *existing;
        return AddComponent<T>(Forward<Args>(args)...);
    }

    /**
     * 最初に見つかった T 型コンポーネントを返す。
     *
     * @tparam T 探す FComponent2D 派生型。
     * @return 見つかった T へのポインタ (無ければ nullptr)。
     */
    template<typename T>
    T* GetComponent() noexcept {
        const void* k = ComponentKindOf<T>();
        for (u32 i = 0; i < m_Components.Size(); ++i) {
            if (m_Components[i] && m_Components[i]->Kind() == k) {
                return static_cast<T*>(m_Components[i].Get());
            }
        }
        return nullptr;
    }

    /**
     * T 型コンポーネントを持っているかを返す。
     *
     * @tparam T 探す FComponent2D 派生型。
     * @return 持っていれば true。
     */
    template<typename T>
    bool HasComponent() const noexcept {
        const void* k = ComponentKindOf<T>();
        for (u32 i = 0; i < m_Components.Size(); ++i) {
            if (m_Components[i] && m_Components[i]->Kind() == k) return true;
        }
        return false;
    }

    /**
     * 最初に見つかった T 型コンポーネントを 1 つ除去する (OnDetach → 破棄)。
     *
     * @tparam T 除去する FComponent2D 派生型。
     * @return 除去したら true、見つからなければ false。
     */
    template<typename T>
    bool RemoveComponent() noexcept {
        const void* k = ComponentKindOf<T>();
        for (u32 i = 0; i < m_Components.Size(); ++i) {
            if (m_Components[i] && m_Components[i]->Kind() == k) {
                m_Components[i]->OnDetach();
                m_Components[i].Reset();
                // compact: 末尾を i に詰める (順序は壊れる)
                if (i + 1 < m_Components.Size()) {
                    m_Components[i] = Move(m_Components[m_Components.Size() - 1]);
                }
                m_Components.PopBack();
                return true;
            }
        }
        return false;
    }

    /**
     * attach 済みコンポーネントの数を返す。
     *
     * @return コンポーネント数。
     */
    u32 ComponentCount() const noexcept { return static_cast<u32>(m_Components.Size()); }

    /**
     * subtree 全体に可変刻み update を伝播する (root から呼ぶ)。
     *
     * @param dt 前フレームからの経過秒。
     */
    void UpdateTree(f32 dt) noexcept;

    /**
     * subtree 全体に固定刻み update を伝播する。
     *
     * @details Scene の OnFixedUpdate から root に対して 1 回呼ぶ。
     * @param fixed_dt 固定刻みの秒。
     */
    void FixedUpdateTree(f32 fixed_dt) noexcept;

    /**
     * subtree 全体を描画する (root から呼ぶ)。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void DrawTree(RenderContext& rc) noexcept;

    /**
     * フレーム境界で 1 回呼び、保留中の構造変更を適用する。
     *
     * @details
     * pending_destroy なノードを subtree ごと OnDespawn 呼んで子配列から除去する
     * (子から先に reap、その後に自分が抜ける)。また pending_reparent_target が
     * セットされた子があれば、その子を target の m_Children へ Move する (cycle 検出済)。
     */
    void ResolveStructuralChanges() noexcept;

private:
    /**
     * Reparent 操作で cycle が生じないかを確認する。
     *
     * @param candidate 移動先候補のノード。
     * @return candidate が自分の子孫 (= cycle になる) なら true。
     */
    bool IsAncestorOf(const FNode2D* candidate) const noexcept;

    /**
     * m_ChildOrder != Tree のとき、子を (SortLayer, world.y) で安定ソートして描画する。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void DrawChildrenSorted(RenderContext& rc) noexcept;

    /** ローカル transform (真値。world は親から合成)。 */
    FTransform2D m_Local{};

    /** 親ノード (root なら nullptr)。 */
    FNode2D*     m_Parent          = nullptr;

    /** 直接の子 (所有権を持つ)。 */
    TArray<TUniquePtr<FNode2D>>      m_Children;

    /** attach されたコンポーネント (所有権を持つ)。 */
    TArray<TUniquePtr<FComponent2D>> m_Components;

    /** ノードの generational handle (default = invalid)。 */
    FNodeId      m_Id{};

    /** 非 null なら次の resolve でこの target 配下へ移動する。 */
    FNode2D*     m_PendingReparentTarget = nullptr;

    /** 描画レイヤ (親ソート時の第1キー)。 */
    i32          m_SortLayer       = 0;

    /** Y-sort の pivot バイアス。 */
    f32          m_YSortBias       = 0.0f;

    /** 子の描画順モード。 */
    EChildDrawOrder m_ChildOrder   = EChildDrawOrder::Tree;

    /** 有効フラグ (false で subtree の update をスキップ)。 */
    bool        m_Enabled         = true;

    /** 可視フラグ (false で subtree の描画をスキップ)。 */
    bool        m_Visible         = true;

    /** OnSpawn 済みフラグ (二重 spawn 防止)。 */
    bool        m_Spawned         = false;

    /** 破棄予定フラグ (次の resolve で reap)。 */
    bool        m_PendingDestroy = false;
};

} // namespace acs::game
