// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FNode2D 実装 (Phase 5 + Phase 3 拡張)
#include "gameframework/Node2D.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

FTransform2D FNode2D::World() const noexcept {
    if (m_Parent == nullptr) return m_Local;
    // 親をたどって合成 (Phase 1 はキャッシュなし、深いツリーで O(depth) コスト)
    return m_Parent->World().Compose(m_Local);
}

FNode2D& FNode2D::AddChild(TUniquePtr<FNode2D> child) noexcept {
    // 重複防止 (nullptr ならエスケープ)。null の場合は自身を返して
    // チェイン記述が壊れないようにする (将来 stale id 対策の sentinel として)。
    if (!child) return *this;
    child->m_Parent = this;
    m_Children.PushBack(Move(child));
    FNode2D& ref = *m_Children.Back();
    if (!ref.m_bSpawned) {
        ref.m_bSpawned = true;
        ref.OnSpawn();
    }
    return ref;
}

void FNode2D::UpdateTree(f32 dt) noexcept {
    if (!m_Enabled || m_PendingDestroy) return;
    OnUpdate(dt);
    // Phase 7: components の OnUpdate を node 自身の後に呼ぶ (合成された振る舞いを適用)
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnUpdate(dt);
    }
    // index 走査で AddChild 走査中追加に対応 (新しい子は同フレームで OnUpdate される)
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        FNode2D* c = m_Children[i].Get();
        if (c != nullptr) c->UpdateTree(dt);
    }
}

void FNode2D::FixedUpdateTree(f32 fixed_dt) noexcept {
    // 変数刻みの UpdateTree とほぼ同じ構造。違いは OnFixedUpdate を呼ぶことと、
    // m_PendingDestroy なノードは固定 step も止める (= UpdateTree と同じ規律)。
    if (!m_Enabled || m_PendingDestroy) return;
    OnFixedUpdate(fixed_dt);
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnFixedUpdate(fixed_dt);
    }
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        FNode2D* c = m_Children[i].Get();
        if (c != nullptr) c->FixedUpdateTree(fixed_dt);
    }
}

void FNode2D::DrawTree(RenderContext& rc) noexcept {
    if (!m_Visible || m_PendingDestroy) return;
    OnDraw(rc);
    // Phase 7: components の OnDraw (描画も合成)
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnDraw(rc);
    }
    // Phase 8: 描画順モード。既定 (Tree) は配列順でゼロオーバーヘッド。
    // Layer/LayerThenY のときだけ兄弟を安定ソートしてから描く。
    if (m_ChildOrder == EChildDrawOrder::Tree) {
        for (u32 i = 0; i < m_Children.Size(); ++i) {
            FNode2D* c = m_Children[i].Get();
            if (c != nullptr) c->DrawTree(rc);
        }
    } else {
        DrawChildrenSorted(rc);
    }
    // 子ツリー描画の後に後処理フックを呼ぶ (ステンシルマスクの解除等)。
    // これにより clip コンポーネントは「OnDraw でマスク設定 → 子ツリー描画 →
    // OnDrawPostChildren で解除」と、自分の子ツリーをマスクで囲える。
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnDrawPostChildren(rc);
    }
}

// Phase 8: 子を (SortLayer 昇順, world.y+YSortBias 昇順) で安定ソートして描画。
// - 第1キー = SortLayer 昇順 (低い層を先に=奥)。
// - 第2キー (LayerThenY のみ) = world.y 昇順 (+Y=画面下なので小さい y=画面上=奥 を先に)。
// - 安定: 同キーの兄弟は配列追加順を保つ。
// World() は O(深さ) なので 1 子につき 1 回だけ評価してキャッシュ (比較中は読まない)。
// 兄弟数は通常小さいので挿入ソート (安定・追加メモリ最小)。スタック上限超過時のみ
// ヒープ確保にフォールバックする。
void FNode2D::DrawChildrenSorted(RenderContext& rc) noexcept {
    const u32 n = static_cast<u32>(m_Children.Size());
    if (n == 0) return;
    const bool by_y = (m_ChildOrder == EChildDrawOrder::LayerThenY);

    constexpr u32 kStackCap = 256;
    u32 idx_s[kStackCap];
    i32 lay_s[kStackCap];
    f32 y_s[kStackCap];
    TArray<u32> idx_h;
    TArray<i32> lay_h;
    TArray<f32> y_h;
    u32* idx = idx_s;
    i32* lay = lay_s;
    f32* yy  = y_s;
    if (n > kStackCap) {
        idx_h.Resize(n); lay_h.Resize(n); y_h.Resize(n);
        idx = idx_h.Data(); lay = lay_h.Data(); yy = y_h.Data();
    }

    // キー収集 (World() は 1 回だけ)。
    for (u32 i = 0; i < n; ++i) {
        idx[i] = i;
        FNode2D* c = m_Children[i].Get();
        lay[i] = (c != nullptr) ? c->m_SortLayer : 0;
        yy[i]  = (by_y && c != nullptr) ? (c->World().position.y + c->m_YSortBias) : 0.0f;
    }

    // 安定挿入ソート。prev(j-1) が ti より「後ろに描く」べきなら右へずらす。
    for (u32 i = 1; i < n; ++i) {
        const u32 ti = idx[i];
        const i32 tl = lay[i];
        const f32 ty = yy[i];
        u32 j = i;
        while (j > 0) {
            const i32 pl = lay[j - 1];
            const f32 py = yy[j - 1];
            bool prev_after;                       // prev を ti の後ろに描くべきか
            if (pl != tl)      prev_after = (pl > tl);   // 層が大 = 手前 = 後
            else if (by_y)     prev_after = (py > ty);   // y が大 (画面下) = 手前 = 後
            else               prev_after = false;        // 同キー = 安定 (動かさない)
            if (!prev_after) break;
            idx[j] = idx[j - 1]; lay[j] = lay[j - 1]; yy[j] = yy[j - 1];
            --j;
        }
        idx[j] = ti; lay[j] = tl; yy[j] = ty;
    }

    for (u32 i = 0; i < n; ++i) {
        FNode2D* c = m_Children[idx[i]].Get();
        if (c != nullptr) c->DrawTree(rc);
    }
}

bool FNode2D::IsAncestorOf(const FNode2D* candidate) const noexcept {
    if (candidate == nullptr) return false;
    // candidate から親を辿り、自分 (this) に行き着けば ancestor。
    // ループ深度は subtree 深度上限 (実用 < 32) で、cycle 防止は構造的に
    // m_Parent ポインタが単一所有 = 木構造である前提で担保される。
    const FNode2D* cur = candidate->m_Parent;
    while (cur != nullptr) {
        if (cur == this) return true;
        cur = cur->m_Parent;
    }
    return false;
}

void FNode2D::Reparent(FNode2D& new_parent) noexcept {
    // ルールチェック (フレーム境界での実適用前に静的検証)
    if (&new_parent == this) {
        ACS_LOG_WARN("FNode2D::Reparent: cannot reparent to self");
        return;
    }
    if (&new_parent == m_Parent) {
        // 既に同 parent なので no-op (チェイン記述で重複呼出されても安全)
        return;
    }
    if (m_Parent == nullptr) {
        // root を reparent するには FSceneManager 側の owner 切替が必要。
        // Phase 3 では root の reparent はサポートしない (= scene root は固定)。
        ACS_LOG_WARN("FNode2D::Reparent: root node has no parent (scene root cannot be reparented)");
        return;
    }
    if (IsAncestorOf(&new_parent)) {
        // 自分の子孫を new_parent に指定 = cycle になる
        ACS_LOG_WARN("FNode2D::Reparent: target is descendant — would create cycle");
        return;
    }
    if (m_PendingDestroy) {
        ACS_LOG_WARN("FNode2D::Reparent: node is pending destroy — ignored");
        return;
    }
    m_PendingReparentTarget = &new_parent;
}

void FNode2D::ResolveStructuralChanges() noexcept {
    // 1) 子の subtree を先に resolve (子の死を先に確定させる)
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        FNode2D* c = m_Children[i].Get();
        if (c != nullptr) c->ResolveStructuralChanges();
    }

    // 2) 親側で pending_destroy な子を OnDespawn して TArray から除く。
    //    順序を保つために compact pattern (read=r, write=w) を使う。
    //    生存ノードは ranges を維持、destroy 対象は OnDespawn してから TUniquePtr が
    //    Move 上書きで破棄される (= ノード本体のデストラクタが走る)。
    //    また pending_reparent_target が立っている子は new_parent 側へ Move する
    //    (= w 側からも除く)。Reparent 対象の Move は本ループの後 m_MoveOuts に集めて
    //    一括で new_parent->m_Children に PushBack する。
    TArray<TUniquePtr<FNode2D>> reparent_pending;

    u32 w = 0;
    for (u32 r = 0; r < m_Children.Size(); ++r) {
        FNode2D* c = m_Children[r].Get();
        if (c == nullptr) continue;
        if (c->m_PendingDestroy) {
            // 親→子の順で OnDespawn (v3 spec: 「OnDespawn 親→子」)。子は既に
            // 先の resolve で自分の pending_destroy 子を片付けているが、自分
            // 自身は今ここで初めて OnDespawn が呼ばれる。
            // Phase 7: components の OnDetach も同時に発火 (node より先に
            // detach し、node 本体の OnDespawn 時点では既に component 群が
            // 切り離された状態)。
            for (u32 ci = 0; ci < c->m_Components.Size(); ++ci) {
                if (c->m_Components[ci]) c->m_Components[ci]->OnDetach();
            }
            c->m_Components.Clear();
            c->OnDespawn();
            // 続いて TUniquePtr<FNode2D> をリセット (= ノードのデストラクタ →
            // children TArray のデストラクタが走り、子ノードのデストラクタ
            // (memory release) は子→親の順)。
            m_Children[r].Reset();
        } else if (c->m_PendingReparentTarget != nullptr) {
            // Reparent 対象 → m_Children から外して reparent_pending へ Move。
            // 実際の新 parent への PushBack はこのループ後に行う (走査安全性)。
            // OnSpawn/OnDespawn は呼ばない (= 既に生きているノードの移動)。
            reparent_pending.PushBack(Move(m_Children[r]));
        } else {
            if (w != r) m_Children[w] = Move(m_Children[r]);
            ++w;
        }
    }
    // 余分の slot を末尾から削除
    while (m_Children.Size() > w) m_Children.PopBack();

    // 3) reparent 対象を target の m_Children に追加。target == nullptr は静的に
    //    防いだので発生しないが、防御的に nullptr ガード。
    for (u32 i = 0; i < reparent_pending.Size(); ++i) {
        if (!reparent_pending[i]) continue;
        FNode2D* moved = reparent_pending[i].Get();
        FNode2D* target = moved->m_PendingReparentTarget;
        moved->m_PendingReparentTarget = nullptr;
        if (target == nullptr) {
            // 何らかの race で target が消えた場合は orphan を作らないように
            // 元の自分の m_Children に戻す。
            moved->m_Parent = this;
            m_Children.PushBack(Move(reparent_pending[i]));
            continue;
        }
        moved->m_Parent = target;
        target->m_Children.PushBack(Move(reparent_pending[i]));
    }
}

} // namespace acs::game
