// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — ANode 実装 (旧 FNode2D/FNode3D 統一、docs/NodeUnification.md)
#include "gameframework/ANode.h"
#include "gameframework/Material2D.h"      // FMaterial2D / MaterialClock
#include "gameframework/RenderContext.h"   // rc.Sprites() (マテリアル効果の適用先)
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

/** この node に使用マテリアル (効果 or PBR) の値を焼き込む。 */
void ANode::SetMaterial(const FMaterial2D& mat) noexcept {
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
FTransform3D ANode::World() const noexcept {
    if (m_Parent == nullptr) return m_Local;
    // 親をたどって合成 (キャッシュなし、深いツリーで O(depth) コスト)
    return m_Parent->World().Compose(m_Local);
}

/** 子の強参照を受け取り、未 spawn なら OnSpawn を即時に呼ぶ。 */
ANode& ANode::AddChild(TObjectPtr<ANode> child) noexcept {
    // null ならエスケープ (チェイン記述が壊れないよう自身を返す)。
    if (!child) return *this;
    child->m_Parent = this;
    m_Children.PushBack(Move(child));
    ANode& ref = *m_Children.Back();
    if (!ref.m_Spawned) {
        ref.m_Spawned = true;
        ref.OnSpawn();
    }
    return ref;
}

/** root まで遡って配線済み FSceneServices を返す (m_Parent と同じ非所有規約)。 */
FSceneServices* ANode::SceneServices() const noexcept {
    const ANode* n = this;
    while (n->m_Parent != nullptr) n = n->m_Parent;
    return n->m_Services;
}

/** root に services を設定し、subtree 全コンポーネントの OnAttachServices を一度発火する。 */
void ANode::_ActivateServices(FSceneServices& svc) noexcept {
    m_Services = &svc;                 // root にのみ設定 (子は walk-to-root で解決)
    _ActivateSubtreeServices(&svc);
}

/** subtree を DFS し各コンポーネントの OnAttachServices をガード付きで発火する。 */
void ANode::_ActivateSubtreeServices(FSceneServices* svc) noexcept {
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->_MaybeAttachServices(svc);
    }
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        if (m_Children[i]) m_Children[i]->_ActivateSubtreeServices(svc);
    }
}

/** root まで遡って配線済み World サブシステム束を返す (services と同じ非所有規約)。 */
FSubsystemCollection* ANode::Subsystems() const noexcept {
    const ANode* n = this;
    while (n->m_Parent != nullptr) n = n->m_Parent;
    return n->m_Subsystems;
}

/** subtree (this + 子孫) を DFS して SerialId 一致のノードを返す (無ければ nullptr)。 */
ANode* ANode::FindBySerialId(i32 id) noexcept {
    if (id < 0) return nullptr;
    if (m_SerialId == id) return this;
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        if (m_Children[i]) {
            if (ANode* hit = m_Children[i]->FindBySerialId(id)) return hit;
        }
    }
    return nullptr;
}

/** コンポーネント → owner ツリーの services 解決 (owner 未設定なら nullptr)。 */
FSceneServices* AComponent::SceneServices() const noexcept {
    return (m_Owner != nullptr) ? m_Owner->SceneServices() : nullptr;
}

/** services が配線済みか。 */
bool AComponent::HasSceneServices() const noexcept {
    return SceneServices() != nullptr;
}

/** コンポーネント → owner ツリーの World サブシステム束を解決 (owner 未設定なら nullptr)。 */
FSubsystemCollection* AComponent::Subsystems() const noexcept {
    return (m_Owner != nullptr) ? m_Owner->Subsystems() : nullptr;
}

/** 自身と components の OnUpdate を呼び、子へ可変刻み update を伝播する。 */
void ANode::UpdateTree(f32 dt) noexcept {
    if (!m_Enabled || m_PendingDestroy) return;
    OnUpdate(dt);
    // components の OnUpdate を node 自身の後に呼ぶ (合成された振る舞いを適用)
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnUpdate(dt);
    }
    // index 走査で AddChild 走査中追加に対応 (新しい子は同フレームで OnUpdate される)
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        ANode* c = m_Children[i].Get();
        if (c != nullptr) c->UpdateTree(dt);
    }
}

/** 自身と components の OnFixedUpdate を呼び、子へ固定刻み update を伝播する。 */
void ANode::FixedUpdateTree(f32 fixed_dt) noexcept {
    if (!m_Enabled || m_PendingDestroy) return;
    OnFixedUpdate(fixed_dt);
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnFixedUpdate(fixed_dt);
    }
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        ANode* c = m_Children[i].Get();
        if (c != nullptr) c->FixedUpdateTree(fixed_dt);
    }
}

/** 自身と components を描画し、子ツリーをツリー順で描く。 */
void ANode::DrawTree(RenderContext& rc) noexcept {
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
        // 影の上下関係 = 描画順。自分より下 (番号が小さい) のキャスターの影は受けない。
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
        // ライトのあるシーンではマテリアル未設定のノードも陰影付けする。
        FLitMaterialParams lm;
        lm.roughness = 0.85f;
        const i32 selfK = (m_SelfOccluder <= -2) ? (-m_SelfOccluder - 2) : m_SelfOccluder;
        if (m_SelfOccluder <= -2) lm.selfShadowOccluder = selfK;
        else                      lm.selfOccluder       = selfK;
        if (selfK > 0) lm.occluderSkipMask = (1u << selfK) - 1u;
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
    // 子はツリー順で描く。描画順の並べ替え (DrawLayer/DrawPriority/YSort) は
    // シーンの描画パスがフラット収集 + 安定ソートで行う (docs/NodeUnification.md)。
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        ANode* c = m_Children[i].Get();
        if (c != nullptr) c->DrawTree(rc);
    }
    // 子ツリー描画の後に後処理フックを呼ぶ (ステンシルマスクの解除等)。
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnDrawPostChildren(rc);
    }
}

bool ANode::IsAncestorOf(const ANode* candidate) const noexcept {
    if (candidate == nullptr) return false;
    // candidate から親を辿り、自分 (this) に行き着けば ancestor。
    const ANode* cur = candidate->m_Parent;
    while (cur != nullptr) {
        if (cur == this) return true;
        cur = cur->m_Parent;
    }
    return false;
}

void ANode::Reparent(ANode& new_parent) noexcept {
    // ルールチェック (フレーム境界での実適用前に静的検証)
    if (&new_parent == this) {
        ACS_LOG_WARN("ANode::Reparent: cannot reparent to self");
        return;
    }
    if (&new_parent == m_Parent) {
        return;   // 既に同 parent (チェイン記述で重複呼出されても安全)
    }
    if (m_Parent == nullptr) {
        ACS_LOG_WARN("ANode::Reparent: root node has no parent (scene root cannot be reparented)");
        return;
    }
    if (IsAncestorOf(&new_parent)) {
        ACS_LOG_WARN("ANode::Reparent: target is descendant — would create cycle");
        return;
    }
    if (m_PendingDestroy) {
        ACS_LOG_WARN("ANode::Reparent: node is pending destroy — ignored");
        return;
    }
    m_PendingReparentTarget = &new_parent;
}

void ANode::ResolveStructuralChanges() noexcept {
    // 1) 子の subtree を先に resolve (子の死を先に確定させる)
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        ANode* c = m_Children[i].Get();
        if (c != nullptr) c->ResolveStructuralChanges();
    }

    // 2) 親側で pending_destroy な子を OnDespawn して配列から除く (compact pattern)。
    //    Reparent 対象は reparent_pending へ Move して後で new_parent に付け直す。
    TArray<TObjectPtr<ANode>> reparent_pending;

    u32 w = 0;
    for (u32 r = 0; r < m_Children.Size(); ++r) {
        ANode* c = m_Children[r].Get();
        if (c == nullptr) continue;
        if (c->m_PendingDestroy) {
            // 親→子の順で OnDespawn。components の OnDetach を node より先に発火。
            for (u32 ci = 0; ci < c->m_Components.Size(); ++ci) {
                if (c->m_Components[ci]) c->m_Components[ci]->OnDetach();
            }
            c->m_Components.Clear();
            c->OnDespawn();
            // 強参照をリセット (これが最後の強参照なら node のデストラクタ →
            // children 配列のデストラクタが走り、子孫の破棄は子→親の順)。
            m_Children[r].Reset();
        } else if (c->m_PendingReparentTarget != nullptr) {
            // Reparent 対象 → m_Children から外して reparent_pending へ Move。
            // OnSpawn/OnDespawn は呼ばない (= 既に生きているノードの移動)。
            reparent_pending.PushBack(Move(m_Children[r]));
        } else {
            if (w != r) m_Children[w] = Move(m_Children[r]);
            ++w;
        }
    }
    // 余分の slot を末尾から削除
    while (m_Children.Size() > w) m_Children.PopBack();

    // 3) reparent 対象を target の m_Children に追加 (防御的 nullptr ガード付き)。
    for (u32 i = 0; i < reparent_pending.Size(); ++i) {
        if (!reparent_pending[i]) continue;
        ANode* moved = reparent_pending[i].Get();
        ANode* target = moved->m_PendingReparentTarget;
        moved->m_PendingReparentTarget = nullptr;
        if (target == nullptr) {
            // 何らかの race で target が消えた場合は orphan を作らず自分に戻す。
            moved->m_Parent = this;
            m_Children.PushBack(Move(reparent_pending[i]));
            continue;
        }
        moved->m_Parent = target;
        target->m_Children.PushBack(Move(reparent_pending[i]));
    }
}

} // namespace acs::game
