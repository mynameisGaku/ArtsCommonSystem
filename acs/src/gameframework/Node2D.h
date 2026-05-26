// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — Node2D (Phase 5)
//
// シーンの中身を表す唯一のノードクラス (2D 専用、抽象 Node 基底は作らない)。
// 親子ツリーで階層的な transform を持ち、各ノードが OnSpawn/OnUpdate/OnDraw/
// OnDespawn を override してロジック・描画を書く。
//
// 設計選択 (Phase 5 = Pillar B Phase 1):
//   ・**non-copy / non-move**: `TUniquePtr<Node2D>` で所有、`Node2D*` で参照。
//     `AddChild(MakeUnique<MyNode>(args))` が標準パターン。
//   ・**lifecycle**: `AddChild` 即時 `OnSpawn` (Phase 1 簡略化)、`Destroy()` で
//     `_pending_destroy` をマーク、フレーム境界の `ResolveStructuralChanges()`
//     で OnDespawn を呼んで TArray から除去。子ツリーが先に reap される。
//   ・**transform**: `_local` を真値、`World()` は親をたどってオンザフライ計算
//     (Phase 1)。dirty キャッシュは Phase 2 (大きなツリーで最適化)。
//   ・**iteration safety**: UpdateTree/DrawTree は index ベースで走査。
//     AddChild が走査中に呼ばれた場合の新規子は同フレームで走らせる (Unity 互換)。
//     Destroy は遅延 reap なので走査中の即時除去はしない。
//
// 範囲外 (Phase 5+ で):
//   ・Component2D (Sprite/Animation/Collider など)
//   ・dirty propagation + cached world transform (パフォーマンス最適化)
//   ・Reparent (構造変更の 3 種目)
//   ・NodeId (stale 参照検出)
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

class Node2D {
public:
    Node2D() noexcept = default;
    virtual ~Node2D() noexcept = default;

    Node2D(const Node2D&)            = delete;
    Node2D& operator=(const Node2D&) = delete;
    Node2D(Node2D&&)                 = delete;
    Node2D& operator=(Node2D&&)      = delete;

    // ----- Lifecycle hooks (override only what you need、全 noexcept 必須) -----
    virtual void OnSpawn()                noexcept {}
    virtual void OnUpdate(f32 /*dt*/)     noexcept {}
    // 固定刻み update (物理・決定論ロジック)。同フレームで 0..max_fixed_steps 回
    // 呼ばれる。Game::SetFixedTimeStep が 0 のとき (= 固定 update 無効) は呼ばれない。
    virtual void OnFixedUpdate(f32 /*fixed_dt*/) noexcept {}
    virtual void OnDraw(RenderContext& /*rc*/) noexcept {}
    virtual void OnDespawn()              noexcept {}

    // ----- Transform -----
    Transform2D&       Local()       noexcept { return _local; }
    const Transform2D& Local() const noexcept { return _local; }

    // 親をたどって world を合成 (Phase 1 はオンザフライ計算、キャッシュなし)
    Transform2D World() const noexcept;

    // ----- 有効/可視フラグ (subtree ごと skip 可能) -----
    void SetEnabled(bool b) noexcept { _enabled = b; }
    bool IsEnabled() const noexcept { return _enabled; }
    void SetVisible(bool b) noexcept { _visible = b; }
    bool IsVisible() const noexcept { return _visible; }

    // ----- Tree -----
    Node2D* Parent() const noexcept { return _parent; }
    u32     ChildCount() const noexcept { return static_cast<u32>(_children.Size()); }
    Node2D* Child(u32 i) const noexcept {
        return i < _children.Size() ? _children[i].Get() : nullptr;
    }

    // 子を追加 (所有権を奪う)。Spawn を即時に呼ぶ (Phase 1 簡略化)。
    // 戻り値は追加した子への参照 (チェイン記述用)。
    Node2D& AddChild(TUniquePtr<Node2D> child) noexcept;

    // 自身を「破棄予定」にマーク。実際の破棄は次の ResolveStructuralChanges で
    // 起こる (OnDespawn 呼出 → TArray から除去 → デストラクタで memory release)。
    void Destroy() noexcept { _pending_destroy = true; }
    bool IsPendingDestroy() const noexcept { return _pending_destroy; }

    // 自分を `new_parent` の子に移動するよう要求する (フレーム境界で適用)。
    // new_parent == nullptr は不正 (警告ログ + 無視)、自分自身 or 子孫を指定した
    // 場合も不正 (cycle 検出、警告 + 無視)。ResolveStructuralChanges 内で
    // _children TArray 間を Move し、parent ポインタを書き換える。
    // OnSpawn/OnDespawn は呼ばれない (= 既に生きているノードの移動)。
    void Reparent(Node2D& new_parent) noexcept;
    bool IsPendingReparent() const noexcept { return _pending_reparent_target != nullptr; }

    // ----- NodeId (Phase 3 = Pillar B Phase 3) -----
    // ノード単位に振られる generational handle。Scene 内で唯一であることは
    // 保証されない (生成側が一意性を管理)。default は invalid (_packed == 0)。
    NodeId Id() const noexcept { return _id; }
    void   _SetId(NodeId id) noexcept { _id = id; }

    // ----- Components (Phase 7、Pillar B Phase 2) -----
    // T の Component2D を構築・attach し、参照を返す。OnAttach は即時呼出。
    template<typename T, typename... Args>
    T& AddComponent(Args&&... args) noexcept {
        TUniquePtr<T> comp = MakeUnique<T>(Forward<Args>(args)...);
        T* ref = comp.Get();
        ref->_SetOwner(this);
        _components.PushBack(TUniquePtr<Component2D>(comp.Release(), comp.GetAllocator()));
        ref->OnAttach(*this);
        return *ref;
    }

    // 最初に見つかった T 型コンポーネントを返す。無ければ nullptr。
    template<typename T>
    T* GetComponent() noexcept {
        const void* k = ComponentKindOf<T>();
        for (u32 i = 0; i < _components.Size(); ++i) {
            if (_components[i] && _components[i]->Kind() == k) {
                return static_cast<T*>(_components[i].Get());
            }
        }
        return nullptr;
    }

    template<typename T>
    bool HasComponent() const noexcept {
        const void* k = ComponentKindOf<T>();
        for (u32 i = 0; i < _components.Size(); ++i) {
            if (_components[i] && _components[i]->Kind() == k) return true;
        }
        return false;
    }

    // 最初に見つかった T 型コンポーネントを 1 つ除去 (OnDetach → 破棄)。true=除去した。
    template<typename T>
    bool RemoveComponent() noexcept {
        const void* k = ComponentKindOf<T>();
        for (u32 i = 0; i < _components.Size(); ++i) {
            if (_components[i] && _components[i]->Kind() == k) {
                _components[i]->OnDetach();
                _components[i].Reset();
                // compact: 末尾を i に詰める (順序は壊れる、Phase 1 はこれで十分)
                if (i + 1 < _components.Size()) {
                    _components[i] = Move(_components[_components.Size() - 1]);
                }
                _components.PopBack();
                return true;
            }
        }
        return false;
    }

    u32 ComponentCount() const noexcept { return static_cast<u32>(_components.Size()); }

    // ----- Subtree traversal (root から呼ぶ) -----
    void UpdateTree(f32 dt) noexcept;
    // 固定刻み update を subtree に propagate (Pillar A polish)。Scene の
    // OnFixedUpdate から root に対して 1 回呼ぶ。
    void FixedUpdateTree(f32 fixed_dt) noexcept;
    void DrawTree(RenderContext& rc) noexcept;

    // フレーム境界で 1 回呼ぶ。pending_destroy なノードを subtree ごと OnDespawn
    // 呼んで子配列から除去する (子から先に reap、その後に自分が抜ける)。
    // また pending_reparent_target がセットされた子があれば、その子を target
    // の _children へ Move する (cycle 検出済)。
    void ResolveStructuralChanges() noexcept;

private:
    // Reparent 操作で cycle が生じないか (= target が自分の子孫でないか) を確認。
    bool IsAncestorOf(const Node2D* candidate) const noexcept;

    Transform2D _local{};
    Node2D*     _parent          = nullptr;
    TArray<TUniquePtr<Node2D>>      _children;
    TArray<TUniquePtr<Component2D>> _components;
    NodeId      _id{};                        // Phase 3: generational handle (default = invalid)
    Node2D*     _pending_reparent_target = nullptr;  // 非 null なら次の resolve で移動
    bool        _enabled         = true;
    bool        _visible         = true;
    bool        _spawned         = false;
    bool        _pending_destroy = false;
};

} // namespace acs::game
