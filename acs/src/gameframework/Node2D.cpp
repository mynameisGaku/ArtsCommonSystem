// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FNode2D 実装
#include "gameframework/Node2D.h"
#include "gameframework/Material2D.h"      // FMaterial2D / ApplyMaterial / MaterialClock
#include "gameframework/RenderContext.h"   // rc.Sprites() (マテリアル効果の適用先)
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

/** この node に使用マテリアル (効果 or PBR) の値を焼き込む。 */
void FNode2D::SetMaterial(const FMaterial2D& mat) noexcept {
    m_Mat.kind     = static_cast<i32>(mat.kind);
    // Effect
    m_Mat.effect   = static_cast<i32>(mat.effect);
    m_Mat.strength = mat.params.strength;
    m_Mat.p0       = mat.params.p0;
    m_Mat.p1       = mat.params.p1;
    m_Mat.p2       = mat.params.p2;
    m_Mat.color    = mat.params.color;
    m_Mat.animated = mat.animated;
    // PBR
    m_Mat.baseColor        = mat.pbr.baseColor;
    m_Mat.metallic         = mat.pbr.metallic;
    m_Mat.roughness        = mat.pbr.roughness;
    m_Mat.normalStrength   = mat.pbr.normalStrength;
    m_Mat.ao               = mat.pbr.ao;
    m_Mat.emissive         = mat.pbr.emissive;
    m_Mat.emissiveStrength = mat.pbr.emissiveStrength;
    m_Mat.shadingMode      = mat.pbr.shadingMode;
    m_Mat.shadow1Color     = mat.pbr.shadow1Color; m_Mat.shadow1Threshold = mat.pbr.shadow1Threshold;
    m_Mat.shadow2Color     = mat.pbr.shadow2Color; m_Mat.shadow2Threshold = mat.pbr.shadow2Threshold;
    m_Mat.rimColor         = mat.pbr.rimColor;     m_Mat.rimPower = mat.pbr.rimPower;
    m_Mat.specColor        = mat.pbr.specColor;    m_Mat.specThreshold = mat.pbr.specThreshold;
    m_Mat.toonSoftness     = mat.pbr.toonSoftness;
    // Substrate 拡張
    m_Mat.clearcoat          = mat.pbr.clearcoat;
    m_Mat.clearcoatRoughness = mat.pbr.clearcoatRoughness;
    m_Mat.anisotropy         = mat.pbr.anisotropy;
    m_Mat.specularLevel      = mat.pbr.specularLevel;
    m_Mat.specularTint       = mat.pbr.specularTint;
    m_Mat.sheen              = mat.pbr.sheen;
    m_Mat.sheenRoughness     = mat.pbr.sheenRoughness;
    m_Mat.sheenColor         = mat.pbr.sheenColor;
    m_Mat.subsurface         = mat.pbr.subsurface;
    m_Mat.subsurfaceColor    = mat.pbr.subsurfaceColor;
    m_Mat.active   = true;
}

/** 親をたどって world transform を合成して返す (キャッシュなし)。 */
FTransform2D FNode2D::World() const noexcept {
    if (m_Parent == nullptr) return m_Local;
    // 親をたどって合成 (キャッシュなし、深いツリーで O(depth) コスト)
    return m_Parent->World().Compose(m_Local);
}

/** 子の所有権を奪い、未 spawn なら OnSpawn を即時に呼ぶ。 */
FNode2D& FNode2D::AddChild(TUniquePtr<FNode2D> child) noexcept {
    // 重複防止 (nullptr ならエスケープ)。null の場合は自身を返して
    // チェイン記述が壊れないようにする。
    if (!child) return *this;
    child->m_Parent = this;
    m_Children.PushBack(Move(child));
    FNode2D& ref = *m_Children.Back();
    if (!ref.m_Spawned) {
        ref.m_Spawned = true;
        ref.OnSpawn();
    }
    return ref;
}

/** root まで遡って配線済み FSceneServices を返す (m_Owner/m_Parent と同じ非所有規約)。 */
FSceneServices* FNode2D::SceneServices() const noexcept {
    const FNode2D* n = this;
    while (n->m_Parent != nullptr) n = n->m_Parent;
    return n->m_Services;
}

/** root に services を設定し、subtree 全コンポーネントの OnAttachServices を一度発火する。 */
void FNode2D::_ActivateServices(FSceneServices& svc) noexcept {
    m_Services = &svc;                 // root にのみ設定 (子は walk-to-root で解決)
    _ActivateSubtreeServices(&svc);
}

/** subtree を DFS し各コンポーネントの OnAttachServices をガード付きで発火する (ポインタは設定しない)。 */
void FNode2D::_ActivateSubtreeServices(FSceneServices* svc) noexcept {
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->_MaybeAttachServices(svc);
    }
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        if (m_Children[i]) m_Children[i]->_ActivateSubtreeServices(svc);
    }
}

/** root まで遡って配線済み World サブシステム束を返す (services と同じ非所有規約)。 */
FSubsystemCollection* FNode2D::Subsystems() const noexcept {
    const FNode2D* n = this;
    while (n->m_Parent != nullptr) n = n->m_Parent;
    return n->m_Subsystems;
}

/** コンポーネント → owner ツリーの services 解決 (owner 未設定なら nullptr)。 */
FSceneServices* FComponent2D::SceneServices() const noexcept {
    return (m_Owner != nullptr) ? m_Owner->SceneServices() : nullptr;
}

/** services が配線済みか。 */
bool FComponent2D::HasSceneServices() const noexcept {
    return SceneServices() != nullptr;
}

/** コンポーネント → owner ツリーの World サブシステム束を解決 (owner 未設定なら nullptr)。 */
FSubsystemCollection* FComponent2D::Subsystems() const noexcept {
    return (m_Owner != nullptr) ? m_Owner->Subsystems() : nullptr;
}

/** 自身と components の OnUpdate を呼び、子へ可変刻み update を伝播する。 */
void FNode2D::UpdateTree(f32 dt) noexcept {
    if (!m_Enabled || m_PendingDestroy) return;
    OnUpdate(dt);
    // components の OnUpdate を node 自身の後に呼ぶ (合成された振る舞いを適用)
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnUpdate(dt);
    }
    // index 走査で AddChild 走査中追加に対応 (新しい子は同フレームで OnUpdate される)
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        FNode2D* c = m_Children[i].Get();
        if (c != nullptr) c->UpdateTree(dt);
    }
}

/** 自身と components の OnFixedUpdate を呼び、子へ固定刻み update を伝播する。 */
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

/** 自身と components を描画し、子ツリーを描画順モードに従って描く。 */
void FNode2D::DrawTree(RenderContext& rc) noexcept {
    if (!m_Visible || m_PendingDestroy) return;
    // 使用マテリアル (PBR or 効果プリセット) で「この node 自身の描画」を包む。子には及ばない
    // (各 node が自分のマテリアルを持つ)。アニメ付きは共有クロックを参照する。
    bool fx = false, lit = false;
    if (m_Mat.active && m_Mat.kind == 0) {          // PBR (Lit): 集めたライト + 法線で BRDF
        FLitMaterialParams lm;
        lm.baseColor = m_Mat.baseColor; lm.metallic = m_Mat.metallic; lm.roughness = m_Mat.roughness;
        lm.normalStrength = m_Mat.normalStrength; lm.ao = m_Mat.ao;
        lm.emissive = m_Mat.emissive; lm.emissiveStrength = m_Mat.emissiveStrength;
        lm.shadingMode = m_Mat.shadingMode;
        lm.shadow1Color = m_Mat.shadow1Color; lm.shadow1Threshold = m_Mat.shadow1Threshold;
        lm.shadow2Color = m_Mat.shadow2Color; lm.shadow2Threshold = m_Mat.shadow2Threshold;
        lm.rimColor = m_Mat.rimColor; lm.rimPower = m_Mat.rimPower;
        lm.specColor = m_Mat.specColor; lm.specThreshold = m_Mat.specThreshold;
        lm.toonSoftness = m_Mat.toonSoftness;
        lm.clearcoat = m_Mat.clearcoat; lm.clearcoatRoughness = m_Mat.clearcoatRoughness;
        lm.anisotropy = m_Mat.anisotropy; lm.specularLevel = m_Mat.specularLevel; lm.specularTint = m_Mat.specularTint;
        lm.sheen = m_Mat.sheen; lm.sheenRoughness = m_Mat.sheenRoughness; lm.sheenColor = m_Mat.sheenColor;
        lm.subsurface = m_Mat.subsurface; lm.subsurfaceColor = m_Mat.subsurfaceColor;
        // m_SelfOccluder: ≥0=自己影スキップ番号 / ≤-2=自己影有効 (-(oc+2) エンコード、Scene2D 参照)
        const i32 selfK = (m_SelfOccluder <= -2) ? (-m_SelfOccluder - 2) : m_SelfOccluder;
        if (m_SelfOccluder <= -2) lm.selfShadowOccluder = selfK;
        else                      lm.selfOccluder       = selfK;
        // 影の上下関係 = 描画順 (オクルーダーは描画順に収集される)。自分より下 (番号が小さい)
        // のキャスターの影は受けない。上の面に下からの影が乗る物理は無く、重なり時のまだら影も防ぐ。
        if (selfK > 0) lm.occluderSkipMask = (1u << selfK) - 1u;
        rc.Sprites().SetLitMaterial(lm, static_cast<IRhiTexture*>(m_Mat.normalTex));   // 法線マップ (null=平面)
        lit = true;
    } else if (m_Mat.active && m_Mat.effect != 0) { // 効果プリセット
        FEffectParams p;
        p.strength = m_Mat.strength; p.p0 = m_Mat.p0; p.p1 = m_Mat.p1; p.p2 = m_Mat.p2;
        p.color = m_Mat.color;
        if (m_Mat.animated) p.time = MaterialClock();
        rc.Sprites().SetEffect(static_cast<ESpriteEffect>(m_Mat.effect), p);
        fx = true;
    } else if (rc.Sprites().LightsActive()) {       // マテリアル無し + ライト有り: 既定 Lit
        // ライトのあるシーンではマテリアル未設定のノードも陰影付けする。さもないと
        // 影の中のノードが無灯火のまま明るく浮く。マット寄りの既定 PBR (白 tint =
        // ノード色がそのまま出る) で、自分が影オクルーダーなら自己影をスキップする。
        FLitMaterialParams lm;
        lm.roughness = 0.85f;
        const i32 selfK = (m_SelfOccluder <= -2) ? (-m_SelfOccluder - 2) : m_SelfOccluder;
        if (m_SelfOccluder <= -2) lm.selfShadowOccluder = selfK;                 // 自己影有効 (エンコード値)
        else                      lm.selfOccluder       = selfK;
        if (selfK > 0) lm.occluderSkipMask = (1u << selfK) - 1u;                 // 自分より下のキャスターを除外
        rc.Sprites().SetLitMaterial(lm, nullptr);
        lit = true;
    }
    OnDraw(rc);
    // components の OnDraw (描画も合成)
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnDraw(rc);
    }
    if (lit)     rc.Sprites().ClearLit();
    else if (fx) rc.Sprites().ClearEffect();   // 子ツリーの前に効果を解除
    // 描画順モード。既定 (Tree) は配列順でゼロオーバーヘッド。
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

/**
 * 子を (SortLayer 昇順, world.y+YSortBias 昇順) で安定ソートして描画する。
 *
 * @details
 * 第1キーは SortLayer 昇順 (低い層を先に=奥)、第2キー (LayerThenY のみ) は world.y
 * 昇順 (+Y=画面下なので小さい y=画面上=奥 を先に)。同キーの兄弟は配列追加順を保つ
 * (安定)。World() は O(深さ) なので 1 子につき 1 回だけ評価してキャッシュする。兄弟数
 * は通常小さいので挿入ソートを使い、スタック上限超過時のみヒープ確保にフォール
 * バックする。
 * @param rc 描画コマンドを積む先のレンダーコンテキスト。
 */
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
        // root の reparent はサポートしない (= scene root は固定)。
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
            // components の OnDetach も同時に発火 (node より先に
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
