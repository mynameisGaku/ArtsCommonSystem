// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — ANode 実装 (旧 FNode2D/FNode3D 統一、docs/NodeUnification.md)
#include "gameframework/ANode.h"
#include "gameframework/Material2D.h"      // FMaterial2D / MaterialClock
#include "gameframework/RenderContext.h"   // rc.Sprites() (マテリアル効果の適用先)
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

ANode::FReparentTargetObserver::~FReparentTargetObserver() noexcept {
    Reset();
}

void ANode::FReparentTargetObserver::Observe(ANode& target) noexcept {
    Reset();
    m_Target = &target;
    m_Next = target.m_ReparentObserverHead;
    if (m_Next != nullptr) m_Next->m_Previous = this;
    target.m_ReparentObserverHead = this;
}

void ANode::FReparentTargetObserver::Reset() noexcept {
    if (m_Target != nullptr) {
        if (m_Previous != nullptr) {
            m_Previous->m_Next = m_Next;
        } else {
            m_Target->m_ReparentObserverHead = m_Next;
        }
        if (m_Next != nullptr) m_Next->m_Previous = m_Previous;
    }
    m_Target = nullptr;
    m_Previous = nullptr;
    m_Next = nullptr;
}

ANode::~ANode() noexcept {
    // 対象側から全 observer を切り離す。各 source のデストラクタが後で
    // Reset しても、破棄中の this を再参照しない状態にする。
    while (m_ReparentObserverHead != nullptr) {
        FReparentTargetObserver* observer = m_ReparentObserverHead;
        m_ReparentObserverHead = observer->m_Next;
        observer->m_Target = nullptr;
        observer->m_Previous = nullptr;
        observer->m_Next = nullptr;
    }
}

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
    // 再帰と動的確保を使わず root まで収集し、root → this の順に合成する。
    const ANode* chain[kNodeMaxTreeDepth + 1u]{};
    u32 count = 0u;
    const ANode* current = this;
    while (current != nullptr && count <= kNodeMaxTreeDepth) {
        chain[count++] = current;
        current = current->m_Parent;
    }

    // 公開構造変更 API が深度上限を保証するため count は必ず 1..上限+1。
    FTransform3D world = chain[count - 1u]->m_Local;
    for (u32 i = count - 1u; i > 0u; --i) {
        world = world.Compose(chain[i - 1u]->m_Local);
    }
    return world;
}

/** root から自身までの深度 (root=0) を返す。 */
u32 ANode::TreeDepth() const noexcept {
    u32 depth = 0u;
    const ANode* current = this;
    while (current->m_Parent != nullptr) {
        ++depth;
        current = current->m_Parent;
    }
    return depth;
}

/** subtree の最大深度を反復 DFS で返す。 */
u32 ANode::SubtreeHeight() const noexcept {
    struct FDepthEntry {
        const ANode* node = nullptr;
        u32 depth = 0u;
    };

    TArray<FDepthEntry> stack;
    stack.PushBack(FDepthEntry{this, 0u});
    u32 max_depth = 0u;
    while (!stack.IsEmpty()) {
        const FDepthEntry entry = stack.Back();
        stack.PopBack();
        if (entry.node == nullptr) continue;
        if (entry.depth > max_depth) max_depth = entry.depth;
        // 上限超過を判定する用途なので、それ以上の全走査は不要。
        if (max_depth > kNodeMaxTreeDepth) return max_depth;
        for (u32 i = 0; i < entry.node->m_Children.Size(); ++i) {
            const ANode* child = entry.node->m_Children[i].Get();
            if (child != nullptr) stack.PushBack(FDepthEntry{child, entry.depth + 1u});
        }
    }
    return max_depth;
}

/** 検証に成功した場合だけ子の強参照を受け取る。 */
EAddChildResult ANode::TryAddChild(TObjectPtr<ANode>& child) noexcept {
    if (!child) return EAddChildResult::NullChild;
    if (child.Get() == this) return EAddChildResult::SelfChild;
    if (child->IsAncestorOf(this)) return EAddChildResult::WouldCreateCycle;
    if (child->m_Parent != nullptr) return EAddChildResult::AlreadyParented;
    if (m_PendingDestroy) return EAddChildResult::ParentPendingDestroy;
    if (child->m_PendingDestroy) return EAddChildResult::ChildPendingDestroy;

    const u32 parent_depth = TreeDepth();
    const u32 child_height = child->SubtreeHeight();
    if (parent_depth >= kNodeMaxTreeDepth ||
        child_height > (kNodeMaxTreeDepth - parent_depth - 1u)) {
        return EAddChildResult::TreeDepthLimitExceeded;
    }

    child->m_Parent = this;
    m_Children.PushBack(Move(child));
    ANode& ref = *m_Children.Back();
    if (!ref.m_Spawned) {
        ref.m_Spawned = true;
        ref.OnSpawn();
    }
    return EAddChildResult::Added;
}

/** 子の強参照を受け取り、未 spawn なら OnSpawn を即時に呼ぶ。 */
ANode& ANode::AddChild(TObjectPtr<ANode> child) noexcept {
    ANode* requested = child.Get();
    const EAddChildResult result = TryAddChild(child);
    if (result == EAddChildResult::Added) return *requested;

    // 後方互換: 失敗時は従来どおり自身を返す。ただし不正な構造変更は適用しない。
    ACS_LOG_WARN("ANode::AddChild: rejected unsafe child (result=%u)",
                 static_cast<u32>(result));
    return *this;
}

/** root まで遡って配線済み CSceneServices を返す (m_Parent と同じ非所有規約)。 */
CSceneServices* ANode::SceneServices() const noexcept {
    const ANode* n = this;
    while (n->m_Parent != nullptr) n = n->m_Parent;
    return n->m_Services;
}

/** root に services を設定し、subtree 全コンポーネントの OnAttachServices を一度発火する。 */
void ANode::_ActivateServices(CSceneServices& svc) noexcept {
    m_Services = &svc;                 // root にのみ設定 (子は walk-to-root で解決)
    _ActivateSubtreeServices(&svc);
}

/** subtree を DFS し各コンポーネントの OnAttachServices をガード付きで発火する。 */
void ANode::_ActivateSubtreeServices(CSceneServices* svc) noexcept {
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->_MaybeAttachServices(svc);
    }
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        if (m_Children[i]) m_Children[i]->_ActivateSubtreeServices(svc);
    }
}

/** root まで遡って配線済み World サブシステム束を返す (services と同じ非所有規約)。 */
CSubsystemCollection* ANode::Subsystems() const noexcept {
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
CSceneServices* AComponent::SceneServices() const noexcept {
    return (m_Owner != nullptr) ? m_Owner->SceneServices() : nullptr;
}

/** services が配線済みか。 */
bool AComponent::HasSceneServices() const noexcept {
    return SceneServices() != nullptr;
}

/** コンポーネント → owner ツリーの World サブシステム束を解決 (owner 未設定なら nullptr)。 */
CSubsystemCollection* AComponent::Subsystems() const noexcept {
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

namespace {

/**
 * DrawTreeSorted の 1 描画単位 (フラット収集アイテム)。
 */
struct FDrawItem {
    /** 描画するノード (非所有、収集フレーム内でのみ有効)。 */
    ANode* node    = nullptr;

    /** 第 1 キー: 描画レイヤー (昇順)。 */
    i32   layer    = 0;

    /** 第 2 キー: 層内プライオリティ (昇順)。 */
    i32   priority = 0;

    /** 第 3 キー: YSort 有効ノードの world.y + bias (昇順、両者有効時のみ比較)。 */
    f32   y        = 0.0f;

    /** YSort に参加するか。 */
    bool  ysort    = false;

    /** 原子 subtree (内部は DrawTree で描く) か。 */
    bool  atomic   = false;

    /** 安定 tie-break 用のツリー出現順。 */
    u32   seq      = 0;
};

/**
 * a を b より先に描くべきかを返す (厳密弱順序、安定ソート用)。
 *
 * @details (layer, priority, [両者 YSort 時のみ y], seq) の辞書式。Y ソートは
 * 同 layer・同 priority の YSort ノード間でのみ効く (混在時も推移律を保つ)。
 */
bool DrawItemBefore(const FDrawItem& a, const FDrawItem& b) noexcept
{
    if (a.layer    != b.layer)    return a.layer    < b.layer;
    if (a.priority != b.priority) return a.priority < b.priority;
    if (a.ysort && b.ysort && a.y != b.y) return a.y < b.y;
    return a.seq < b.seq;
}

/**
 * 可視 subtree をフラット収集する (原子 subtree は展開しない)。
 *
 * @param n 収集ルート。
 * @param out 収集先。
 * @param any_keys レイヤー/プライオリティ/YSort/原子のいずれかが使われていたら true。
 */
void CollectDrawItems(ANode& n, TArray<FDrawItem>& out, bool& any_keys) noexcept
{
    if (!n.IsVisible() || n.IsPendingDestroy()) return;
    FDrawItem it;
    it.node     = &n;
    it.layer    = n.DrawLayer();
    it.priority = n.DrawPriority();
    it.ysort    = n.IsYSortEnabled();
    it.atomic   = n.HasAtomicSubtreeComponent();
    it.y        = it.ysort ? (n.World().position.y + n.YSortBias()) : 0.0f;
    it.seq      = static_cast<u32>(out.Size());
    if (it.layer != 0 || it.priority != 0 || it.ysort || it.atomic) any_keys = true;
    out.PushBack(it);
    if (it.atomic) return;   // 内部は DrawTree (ツリー順) で一塊描画
    for (u32 i = 0; i < n.ChildCount(); ++i) {
        if (ANode* c = n.Child(i)) CollectDrawItems(*c, out, any_keys);
    }
}

/**
 * FDrawItem 配列のインデックスを安定ボトムアップ merge sort で並べる。
 *
 * @details STL 不使用・O(n log n)・安定。挿入ソート (SpriteSortList) と違い
 * ノード数が数千でも劣化しない。
 */
void StableSortDrawOrder(const TArray<FDrawItem>& items, TArray<u32>& order, TArray<u32>& scratch) noexcept
{
    const u32 n = static_cast<u32>(items.Size());
    order.Resize(n);
    scratch.Resize(n);
    for (u32 i = 0; i < n; ++i) order[i] = i;

    u32* src = order.Data();
    u32* dst = scratch.Data();
    for (u32 width = 1; width < n; width *= 2) {
        for (u32 lo = 0; lo < n; lo += width * 2) {
            const u32 mid = (lo + width < n) ? (lo + width) : n;
            const u32 hi  = (lo + width * 2 < n) ? (lo + width * 2) : n;
            u32 a = lo, b = mid, w = lo;
            while (a < mid && b < hi) {
                // 安定性: 右が左より厳密に前のときだけ右を先に取る。
                if (DrawItemBefore(items[src[b]], items[src[a]])) dst[w++] = src[b++];
                else                                              dst[w++] = src[a++];
            }
            while (a < mid) dst[w++] = src[a++];
            while (b < hi)  dst[w++] = src[b++];
        }
        u32* t = src; src = dst; dst = t;   // ping-pong
    }
    // 最終結果が scratch 側にある場合は order へ書き戻す。
    if (src != order.Data()) {
        for (u32 i = 0; i < n; ++i) order[i] = src[i];
    }
}

} // namespace

/** このノード自身 (OnDraw + components、子は含まない) を描画する。 */
void ANode::DrawSelf(FRenderContext& rc) noexcept {
    if (!m_Visible || m_PendingDestroy) return;
    // 使用マテリアル (PBR or 効果プリセット) で「この node 自身の描画」を包む。子には及ばない
    // (各 node が自分のマテリアルを持つ)。アニメ付きは共有クロックを参照する。
    // スプライトバッチ未配線 (ヘッドレス実行) ではマテリアル包み込みをスキップする。
    const bool has_sb = rc.HasSprites();
    bool fx = false, lit = false;
    if (has_sb && m_Mat.active && m_Mat.kind == 0) {   // PBR (Lit): 集めたライト + 法線で BRDF
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
    } else if (has_sb && m_Mat.active && m_Mat.effect != 0) { // 効果プリセット
        FEffectParams p;
        p.strength = m_Mat.strength; p.p0 = m_Mat.p0; p.p1 = m_Mat.p1; p.p2 = m_Mat.p2;
        p.color = m_Mat.color;
        if (m_Mat.animated) p.time = MaterialClock();
        rc.Sprites().SetEffect(static_cast<ESpriteEffect>(m_Mat.effect), p);
        fx = true;
    } else if (has_sb && rc.Sprites().LightsActive()) {       // マテリアル無し + ライト有り: 既定 Lit
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
    else if (fx) rc.Sprites().ClearEffect();
    // フラット実行では子は独立アイテムとして後から描かれるため、非原子ノードの
    // OnDrawPostChildren (no-op が既定) はここで即時に呼ぶ。
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnDrawPostChildren(rc);
    }
}

/** 自身と components を描画し、子ツリーをツリー順で描く。 */
void ANode::DrawTree(FRenderContext& rc) noexcept {
    if (!m_Visible || m_PendingDestroy) return;
    // DrawSelf と同じマテリアル包み込みだが、OnDrawPostChildren は子ツリーの後。
    const bool has_sb = rc.HasSprites();
    bool fx = false, lit = false;
    if (has_sb && m_Mat.active && m_Mat.kind == 0) {
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
        const i32 selfK = (m_SelfOccluder <= -2) ? (-m_SelfOccluder - 2) : m_SelfOccluder;
        if (m_SelfOccluder <= -2) lm.selfShadowOccluder = selfK;
        else                      lm.selfOccluder       = selfK;
        if (selfK > 0) lm.occluderSkipMask = (1u << selfK) - 1u;
        rc.Sprites().SetLitMaterial(lm, static_cast<IRhiTexture*>(m_Mat.normalTex));
        lit = true;
    } else if (has_sb && m_Mat.active && m_Mat.effect != 0) {
        FEffectParams p;
        p.strength = m_Mat.strength; p.p0 = m_Mat.p0; p.p1 = m_Mat.p1; p.p2 = m_Mat.p2;
        p.color = m_Mat.color;
        if (m_Mat.animated) p.time = MaterialClock();
        rc.Sprites().SetEffect(static_cast<ESpriteEffect>(m_Mat.effect), p);
        fx = true;
    } else if (has_sb && rc.Sprites().LightsActive()) {
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
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnDraw(rc);
    }
    if (lit)     rc.Sprites().ClearLit();
    else if (fx) rc.Sprites().ClearEffect();   // 子ツリーの前に効果を解除
    // 子はツリー順。
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        ANode* c = m_Children[i].Get();
        if (c != nullptr) c->DrawTree(rc);
    }
    // 子ツリー描画の後に後処理フックを呼ぶ (ステンシルマスクの解除等)。
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnDrawPostChildren(rc);
    }
}

/** subtree をグローバル描画順 (layer, priority, [y], 出現順) で描く。 */
void ANode::DrawTreeSorted(FRenderContext& rc) noexcept {
    if (!m_Visible || m_PendingDestroy) return;

    // 1) フラット収集 (原子 subtree は 1 アイテムに畳む)。
    TArray<FDrawItem> items;
    bool any_keys = false;
    CollectDrawItems(*this, items, any_keys);

    // 2) 全ノードがキー未使用ならソートを省略してツリー順で描く
    //    (= 従来挙動と完全一致・ゼロオーバーヘッド)。
    if (!any_keys) {
        DrawTree(rc);
        return;
    }

    // 3) 安定ソート → 4) フラット実行 (原子は subtree ごと DrawTree)。
    TArray<u32> order;
    TArray<u32> scratch;
    StableSortDrawOrder(items, order, scratch);
    for (u32 i = 0; i < order.Size(); ++i) {
        const FDrawItem& it = items[order[i]];
        if (it.node == nullptr) continue;
        if (it.atomic) it.node->DrawTree(rc);
        else           it.node->DrawSelf(rc);
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
    if (new_parent.m_PendingDestroy) {
        ACS_LOG_WARN("ANode::Reparent: target is pending destroy — ignored");
        return;
    }
    const u32 parent_depth = new_parent.TreeDepth();
    const u32 subtree_height = SubtreeHeight();
    if (parent_depth >= kNodeMaxTreeDepth ||
        subtree_height > (kNodeMaxTreeDepth - parent_depth - 1u)) {
        ACS_LOG_WARN("ANode::Reparent: tree depth limit exceeded — ignored");
        return;
    }
    m_PendingReparentTarget.Observe(new_parent);
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
        } else if (c->m_PendingReparentTarget.Get() != nullptr) {
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
        ANode* target = moved->m_PendingReparentTarget.Get();
        moved->m_PendingReparentTarget.Reset();

        // 要求後に対象の状態やツリーが変わり得るため、適用直前の構造で再検証する。
        bool target_pending_destroy = false;
        for (ANode* ancestor = target; ancestor != nullptr; ancestor = ancestor->m_Parent) {
            if (ancestor->m_PendingDestroy) {
                target_pending_destroy = true;
                break;
            }
        }
        bool can_apply = target != nullptr &&
                         target != moved &&
                         !moved->m_PendingDestroy &&
                         !target_pending_destroy &&
                         !moved->IsAncestorOf(target);
        if (can_apply) {
            const u32 target_depth = target->TreeDepth();
            const u32 moved_height = moved->SubtreeHeight();
            can_apply = target_depth < kNodeMaxTreeDepth &&
                        moved_height <= (kNodeMaxTreeDepth - target_depth - 1u);
        }
        if (!can_apply) {
            // 対象消滅・破棄予定・循環・深度超過では orphan を作らず元の親へ戻す。
            ACS_LOG_WARN("ANode::ResolveStructuralChanges: stale or unsafe reparent target — cancelled");
            moved->m_Parent = this;
            m_Children.PushBack(Move(reparent_pending[i]));
            continue;
        }
        moved->m_Parent = target;
        target->m_Children.PushBack(Move(reparent_pending[i]));
    }
}

} // namespace acs::game
