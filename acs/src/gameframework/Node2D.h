// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FNode2D (Phase 5)
//
// シーンの中身を表す唯一のノードクラス (2D 専用、抽象 Node 基底は作らない)。
// 親子ツリーで階層的な transform を持ち、各ノードが OnSpawn/OnUpdate/OnDraw/
// OnDespawn を override してロジック・描画を書く。
//
// 設計選択 (Phase 5 = Pillar B Phase 1):
//   ・**non-copy / non-move**: `TUniquePtr<FNode2D>` で所有、`FNode2D*` で参照。
//     `AddChild(MakeUnique<MyNode>(args))` が標準パターン。
//   ・**lifecycle**: `AddChild` 即時 `OnSpawn` (Phase 1 簡略化)、`Destroy()` で
//     `m_PendingDestroy` をマーク、フレーム境界の `ResolveStructuralChanges()`
//     で OnDespawn を呼んで TArray から除去。子ツリーが先に reap される。
//   ・**transform**: `m_Local` を真値、`World()` は親をたどってオンザフライ計算
//     (Phase 1)。dirty キャッシュは Phase 2 (大きなツリーで最適化)。
//   ・**iteration safety**: UpdateTree/DrawTree は index ベースで走査。
//     AddChild が走査中に呼ばれた場合の新規子は同フレームで走らせる (Unity 互換)。
//     Destroy は遅延 reap なので走査中の即時除去はしない。
//
// 範囲外 (Phase 5+ で):
//   ・FComponent2D (Sprite/FAnimation/Collider など)
//   ・dirty propagation + cached world transform (パフォーマンス最適化)
//   ・Reparent (構造変更の 3 種目)
//   ・FNodeId (stale 参照検出)
//   ・ECS bridging
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

class FNode2D {
public:
    FNode2D() noexcept = default;
    virtual ~FNode2D() noexcept = default;

    FNode2D(const FNode2D&)            = delete;
    FNode2D& operator=(const FNode2D&) = delete;
    FNode2D(FNode2D&&)                 = delete;
    FNode2D& operator=(FNode2D&&)      = delete;

    // ----- Lifecycle hooks (override only what you need、全 noexcept 必須) -----
    virtual void OnSpawn()                noexcept {}
    virtual void OnUpdate(f32 /*dt*/)     noexcept {}
    // 固定刻み update (物理・決定論ロジック)。同フレームで 0..max_fixed_steps 回
    // 呼ばれる。FGame::SetFixedTimeStep が 0 のとき (= 固定 update 無効) は呼ばれない。
    virtual void OnFixedUpdate(f32 /*fixed_dt*/) noexcept {}
    virtual void OnDraw(RenderContext& /*rc*/) noexcept {}
    virtual void OnDespawn()              noexcept {}

    // ----- Transform -----
    FTransform2D&       Local()       noexcept { return m_Local; }
    const FTransform2D& Local() const noexcept { return m_Local; }

    // 親をたどって world を合成 (Phase 1 はオンザフライ計算、キャッシュなし)
    FTransform2D World() const noexcept;

    // ----- 有効/可視フラグ (subtree ごと skip 可能) -----
    void SetEnabled(bool b) noexcept { m_Enabled = b; }
    bool IsEnabled() const noexcept { return m_Enabled; }
    void SetVisible(bool b) noexcept { m_Visible = b; }
    bool IsVisible() const noexcept { return m_Visible; }

    // ----- 描画順 (Phase 8: レイヤ / Y-sort) -----
    // 子の描画順モード。既定 Tree = 配列追加順 (従来挙動・ゼロオーバーヘッド)。
    // 見下ろしゲームの Y 遮蔽や、背景/ワールド/前景/HUD のレイヤ分離に使う。
    enum class EChildDrawOrder : u8 {
        Tree       = 0,  // 配列追加順 (従来)。ソートしない。
        Layer      = 1,  // SortLayer 昇順 (低い層が奥=先に描画)。同層は配列順で安定。
        LayerThenY = 2,  // SortLayer 昇順 → 同層内は (world.y + YSortBias) の降順。
                         // +Y=up なので大きい y (画面上=奥) を先に描画 = 見下ろし遮蔽。
    };

    // この node が「自分の子」を描画する順序を設定する。Layer/LayerThenY のとき
    // DrawTree は子を安定ソートしてから描く (兄弟間のみ。木の階層は維持)。
    void            SetChildDrawOrder(EChildDrawOrder o) noexcept { m_ChildOrder = o; }
    EChildDrawOrder ChildDrawOrder() const noexcept { return m_ChildOrder; }

    // この node の描画レイヤ。親が Layer/LayerThenY のとき兄弟間ソートの第1キー。
    // 低いほど奥 (先に描画)。既定 0。
    void SetSortLayer(i32 layer) noexcept { m_SortLayer = layer; }
    i32  SortLayer() const noexcept { return m_SortLayer; }

    // Y-sort の pivot バイアス。world.y に加算してソートキーにする
    // (例: スプライト中心でなく「足元」で遮蔽したい場合に負値を入れる)。既定 0。
    void SetYSortBias(f32 bias) noexcept { m_YSortBias = bias; }
    f32  YSortBias() const noexcept { return m_YSortBias; }

    // ----- Tree -----
    FNode2D* Parent() const noexcept { return m_Parent; }
    u32     ChildCount() const noexcept { return static_cast<u32>(m_Children.Size()); }
    FNode2D* Child(u32 i) const noexcept {
        return i < m_Children.Size() ? m_Children[i].Get() : nullptr;
    }

    // 子を追加 (所有権を奪う)。Spawn を即時に呼ぶ (Phase 1 簡略化)。
    // 戻り値は追加した子への参照 (チェイン記述用)。
    FNode2D& AddChild(TUniquePtr<FNode2D> child) noexcept;

    // 自身を「破棄予定」にマーク。実際の破棄は次の ResolveStructuralChanges で
    // 起こる (OnDespawn 呼出 → TArray から除去 → デストラクタで memory release)。
    void Destroy() noexcept { m_PendingDestroy = true; }
    bool IsPendingDestroy() const noexcept { return m_PendingDestroy; }

    // 自分を `new_parent` の子に移動するよう要求する (フレーム境界で適用)。
    // new_parent == nullptr は不正 (警告ログ + 無視)、自分自身 or 子孫を指定した
    // 場合も不正 (cycle 検出、警告 + 無視)。ResolveStructuralChanges 内で
    // m_Children TArray 間を Move し、parent ポインタを書き換える。
    // OnSpawn/OnDespawn は呼ばれない (= 既に生きているノードの移動)。
    void Reparent(FNode2D& new_parent) noexcept;
    bool IsPendingReparent() const noexcept { return m_PendingReparentTarget != nullptr; }

    // ----- FNodeId (Phase 3 = Pillar B Phase 3) -----
    // ノード単位に振られる generational handle。Scene 内で唯一であることは
    // 保証されない (生成側が一意性を管理)。default は invalid (m_Packed == 0)。
    FNodeId Id() const noexcept { return m_Id; }
    void   _SetId(FNodeId id) noexcept { m_Id = id; }

    // ----- Components (Phase 7、Pillar B Phase 2) -----
    // T の FComponent2D を構築・attach し、参照を返す。OnAttach は即時呼出。
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

    // T があれば返し、無ければ追加して返す (RequireComponent の自動追加に使う)。
    template<typename T, typename... Args>
    T& GetOrAddComponent(Args&&... args) noexcept {
        if (T* existing = GetComponent<T>()) return *existing;
        return AddComponent<T>(Forward<Args>(args)...);
    }

    // 最初に見つかった T 型コンポーネントを返す。無ければ nullptr。
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

    template<typename T>
    bool HasComponent() const noexcept {
        const void* k = ComponentKindOf<T>();
        for (u32 i = 0; i < m_Components.Size(); ++i) {
            if (m_Components[i] && m_Components[i]->Kind() == k) return true;
        }
        return false;
    }

    // 最初に見つかった T 型コンポーネントを 1 つ除去 (OnDetach → 破棄)。true=除去した。
    template<typename T>
    bool RemoveComponent() noexcept {
        const void* k = ComponentKindOf<T>();
        for (u32 i = 0; i < m_Components.Size(); ++i) {
            if (m_Components[i] && m_Components[i]->Kind() == k) {
                m_Components[i]->OnDetach();
                m_Components[i].Reset();
                // compact: 末尾を i に詰める (順序は壊れる、Phase 1 はこれで十分)
                if (i + 1 < m_Components.Size()) {
                    m_Components[i] = Move(m_Components[m_Components.Size() - 1]);
                }
                m_Components.PopBack();
                return true;
            }
        }
        return false;
    }

    u32 ComponentCount() const noexcept { return static_cast<u32>(m_Components.Size()); }

    // ----- Subtree traversal (root から呼ぶ) -----
    void UpdateTree(f32 dt) noexcept;
    // 固定刻み update を subtree に propagate (Pillar A polish)。Scene の
    // OnFixedUpdate から root に対して 1 回呼ぶ。
    void FixedUpdateTree(f32 fixed_dt) noexcept;
    void DrawTree(RenderContext& rc) noexcept;

    // フレーム境界で 1 回呼ぶ。pending_destroy なノードを subtree ごと OnDespawn
    // 呼んで子配列から除去する (子から先に reap、その後に自分が抜ける)。
    // また pending_reparent_target がセットされた子があれば、その子を target
    // の m_Children へ Move する (cycle 検出済)。
    void ResolveStructuralChanges() noexcept;

private:
    // Reparent 操作で cycle が生じないか (= target が自分の子孫でないか) を確認。
    bool IsAncestorOf(const FNode2D* candidate) const noexcept;

    // m_ChildOrder != Tree のとき、子を (SortLayer, world.y) で安定ソートして描画する。
    void DrawChildrenSorted(RenderContext& rc) noexcept;

    FTransform2D m_Local{};
    FNode2D*     m_Parent          = nullptr;
    TArray<TUniquePtr<FNode2D>>      m_Children;
    TArray<TUniquePtr<FComponent2D>> m_Components;
    FNodeId      m_Id{};                        // Phase 3: generational handle (default = invalid)
    FNode2D*     m_PendingReparentTarget = nullptr;  // 非 null なら次の resolve で移動
    i32          m_SortLayer       = 0;          // Phase 8: 描画レイヤ (親ソート時の第1キー)
    f32          m_YSortBias       = 0.0f;       // Phase 8: Y-sort の pivot バイアス
    EChildDrawOrder m_ChildOrder   = EChildDrawOrder::Tree;  // Phase 8: 子の描画順モード
    bool        m_Enabled         = true;
    bool        m_Visible         = true;
    bool        m_bSpawned         = false;
    bool        m_PendingDestroy = false;
};

} // namespace acs::game
