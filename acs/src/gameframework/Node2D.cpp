// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — Node2D 実装 (Phase 5 + Phase 3 拡張)
#include "gameframework/Node2D.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

FTransform2D Node2D::World() const noexcept {
    if (_parent == nullptr) return _local;
    // 親をたどって合成 (Phase 1 はキャッシュなし、深いツリーで O(depth) コスト)
    return _parent->World().Compose(_local);
}

Node2D& Node2D::AddChild(TUniquePtr<Node2D> child) noexcept {
    // 重複防止 (nullptr ならエスケープ)。null の場合は自身を返して
    // チェイン記述が壊れないようにする (将来 stale id 対策の sentinel として)。
    if (!child) return *this;
    child->_parent = this;
    _children.PushBack(Move(child));
    Node2D& ref = *_children.Back();
    if (!ref._spawned) {
        ref._spawned = true;
        ref.OnSpawn();
    }
    return ref;
}

void Node2D::UpdateTree(f32 dt) noexcept {
    if (!_enabled || _pending_destroy) return;
    OnUpdate(dt);
    // Phase 7: components の OnUpdate を node 自身の後に呼ぶ (合成された振る舞いを適用)
    for (u32 i = 0; i < _components.Size(); ++i) {
        if (_components[i]) _components[i]->OnUpdate(dt);
    }
    // index 走査で AddChild 走査中追加に対応 (新しい子は同フレームで OnUpdate される)
    for (u32 i = 0; i < _children.Size(); ++i) {
        Node2D* c = _children[i].Get();
        if (c != nullptr) c->UpdateTree(dt);
    }
}

void Node2D::FixedUpdateTree(f32 fixed_dt) noexcept {
    // 変数刻みの UpdateTree とほぼ同じ構造。違いは OnFixedUpdate を呼ぶことと、
    // _pending_destroy なノードは固定 step も止める (= UpdateTree と同じ規律)。
    if (!_enabled || _pending_destroy) return;
    OnFixedUpdate(fixed_dt);
    for (u32 i = 0; i < _components.Size(); ++i) {
        if (_components[i]) _components[i]->OnFixedUpdate(fixed_dt);
    }
    for (u32 i = 0; i < _children.Size(); ++i) {
        Node2D* c = _children[i].Get();
        if (c != nullptr) c->FixedUpdateTree(fixed_dt);
    }
}

void Node2D::DrawTree(RenderContext& rc) noexcept {
    if (!_visible || _pending_destroy) return;
    OnDraw(rc);
    // Phase 7: components の OnDraw (描画も合成)
    for (u32 i = 0; i < _components.Size(); ++i) {
        if (_components[i]) _components[i]->OnDraw(rc);
    }
    for (u32 i = 0; i < _children.Size(); ++i) {
        Node2D* c = _children[i].Get();
        if (c != nullptr) c->DrawTree(rc);
    }
}

bool Node2D::IsAncestorOf(const Node2D* candidate) const noexcept {
    if (candidate == nullptr) return false;
    // candidate から親を辿り、自分 (this) に行き着けば ancestor。
    // ループ深度は subtree 深度上限 (実用 < 32) で、cycle 防止は構造的に
    // _parent ポインタが単一所有 = 木構造である前提で担保される。
    const Node2D* cur = candidate->_parent;
    while (cur != nullptr) {
        if (cur == this) return true;
        cur = cur->_parent;
    }
    return false;
}

void Node2D::Reparent(Node2D& new_parent) noexcept {
    // ルールチェック (フレーム境界での実適用前に静的検証)
    if (&new_parent == this) {
        ACS_LOG_WARN("Node2D::Reparent: cannot reparent to self");
        return;
    }
    if (&new_parent == _parent) {
        // 既に同 parent なので no-op (チェイン記述で重複呼出されても安全)
        return;
    }
    if (_parent == nullptr) {
        // root を reparent するには SceneManager 側の owner 切替が必要。
        // Phase 3 では root の reparent はサポートしない (= scene root は固定)。
        ACS_LOG_WARN("Node2D::Reparent: root node has no parent (scene root cannot be reparented)");
        return;
    }
    if (IsAncestorOf(&new_parent)) {
        // 自分の子孫を new_parent に指定 = cycle になる
        ACS_LOG_WARN("Node2D::Reparent: target is descendant — would create cycle");
        return;
    }
    if (_pending_destroy) {
        ACS_LOG_WARN("Node2D::Reparent: node is pending destroy — ignored");
        return;
    }
    _pending_reparent_target = &new_parent;
}

void Node2D::ResolveStructuralChanges() noexcept {
    // 1) 子の subtree を先に resolve (子の死を先に確定させる)
    for (u32 i = 0; i < _children.Size(); ++i) {
        Node2D* c = _children[i].Get();
        if (c != nullptr) c->ResolveStructuralChanges();
    }

    // 2) 親側で pending_destroy な子を OnDespawn して TArray から除く。
    //    順序を保つために compact pattern (read=r, write=w) を使う。
    //    生存ノードは ranges を維持、destroy 対象は OnDespawn してから TUniquePtr が
    //    Move 上書きで破棄される (= ノード本体のデストラクタが走る)。
    //    また pending_reparent_target が立っている子は new_parent 側へ Move する
    //    (= w 側からも除く)。Reparent 対象の Move は本ループの後 _move_outs に集めて
    //    一括で new_parent->_children に PushBack する。
    TArray<TUniquePtr<Node2D>> reparent_pending;

    u32 w = 0;
    for (u32 r = 0; r < _children.Size(); ++r) {
        Node2D* c = _children[r].Get();
        if (c == nullptr) continue;
        if (c->_pending_destroy) {
            // 親→子の順で OnDespawn (v3 spec: 「OnDespawn 親→子」)。子は既に
            // 先の resolve で自分の pending_destroy 子を片付けているが、自分
            // 自身は今ここで初めて OnDespawn が呼ばれる。
            // Phase 7: components の OnDetach も同時に発火 (node より先に
            // detach し、node 本体の OnDespawn 時点では既に component 群が
            // 切り離された状態)。
            for (u32 ci = 0; ci < c->_components.Size(); ++ci) {
                if (c->_components[ci]) c->_components[ci]->OnDetach();
            }
            c->_components.Clear();
            c->OnDespawn();
            // 続いて TUniquePtr<Node2D> をリセット (= ノードのデストラクタ →
            // children TArray のデストラクタが走り、子ノードのデストラクタ
            // (memory release) は子→親の順)。
            _children[r].Reset();
        } else if (c->_pending_reparent_target != nullptr) {
            // Reparent 対象 → _children から外して reparent_pending へ Move。
            // 実際の新 parent への PushBack はこのループ後に行う (走査安全性)。
            // OnSpawn/OnDespawn は呼ばない (= 既に生きているノードの移動)。
            reparent_pending.PushBack(Move(_children[r]));
        } else {
            if (w != r) _children[w] = Move(_children[r]);
            ++w;
        }
    }
    // 余分の slot を末尾から削除
    while (_children.Size() > w) _children.PopBack();

    // 3) reparent 対象を target の _children に追加。target == nullptr は静的に
    //    防いだので発生しないが、防御的に nullptr ガード。
    for (u32 i = 0; i < reparent_pending.Size(); ++i) {
        if (!reparent_pending[i]) continue;
        Node2D* moved = reparent_pending[i].Get();
        Node2D* target = moved->_pending_reparent_target;
        moved->_pending_reparent_target = nullptr;
        if (target == nullptr) {
            // 何らかの race で target が消えた場合は orphan を作らないように
            // 元の自分の _children に戻す。
            moved->_parent = this;
            _children.PushBack(Move(reparent_pending[i]));
            continue;
        }
        moved->_parent = target;
        target->_children.PushBack(Move(reparent_pending[i]));
    }
}

} // namespace acs::game
