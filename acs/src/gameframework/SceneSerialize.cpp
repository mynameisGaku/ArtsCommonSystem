// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — SceneSerialize 実装。詳細はヘッダ参照。
// =============================================================================
#include "gameframework/SceneSerialize.h"
#include "gameframework/Node2D.h"
#include "gameframework/Component2D.h"
#include "gameframework/Transform2D.h"
#include "gameframework/Reflect.h"            // FTypeRegistry / FTypeDesc
#include "gameframework/ReflectSerialize.h"   // SerializeByName / DeserializeReflected
#include "gameframework/ComponentFactory.h"   // CreateComponentByName
#include "container/Array.h"

#include <cstring>   // std::memcpy / std::strlen

namespace acs::game {

namespace {

/** 固定バッファへの追記ライタ (cap 超過で ok=false)。減算形で u32 オーバーフロー安全。 */
struct FWriter {
    u8* buf; u32 cap; u32 cur; bool ok;
    FWriter(u8* b, u32 c) noexcept : buf(b), cap(c), cur(0u), ok(true) {}
    void Bytes(const void* p, u32 n) noexcept {
        if (!ok || n > cap - cur) { ok = false; return; }
        std::memcpy(buf + cur, p, n);
        cur += n;
    }
    void U8 (u8  v) noexcept { Bytes(&v, 1u); }
    void I32(i32 v) noexcept { Bytes(&v, 4u); }
    void U32(u32 v) noexcept { Bytes(&v, 4u); }
    void F32(f32 v) noexcept { Bytes(&v, 4u); }
};

/** 固定バッファからの読み出しリーダ (範囲外で ok=false)。 */
struct FReader {
    const u8* data; u32 size; u32 cur; bool ok;
    FReader(const u8* d, u32 s) noexcept : data(d), size(s), cur(0u), ok(true) {}
    void Bytes(void* out, u32 n) noexcept {
        if (!ok || n > size - cur) { ok = false; return; }
        std::memcpy(out, data + cur, n);
        cur += n;
    }
    u8  U8 () noexcept { u8  v = 0u;  Bytes(&v, 1u); return v; }
    i32 I32() noexcept { i32 v = 0;   Bytes(&v, 4u); return v; }
    u32 U32() noexcept { u32 v = 0u;  Bytes(&v, 4u); return v; }
    f32 F32() noexcept { f32 v = 0.0f; Bytes(&v, 4u); return v; }
};

/**
 * Load が受け付けるツリー深度の上限。
 *
 * @details
 * FNode2D の破棄は子 TArray 経由の再帰なので、敵対的なバイト列で数万深の親チェーンを
 * 作られるとデストラクタでスタックオーバーフローする (DoS)。上限を超える親付けは
 * 「壊れた parent index は root 直下へ」と同じ保険で root 直下へ付け替え、ノード自体は
 * 失わずに深度だけを抑える。正当なシーンで 512 階層を超えることは想定しない。
 */
constexpr u32 kMaxTreeDepth = 512;

/** DFS pre-order でノードを平坦化し (親が子より先)、各ノードと親 index を集める。 */
void Flatten(const FNode2D* n, i32 parent_index,
             TArray<const FNode2D*>& nodes, TArray<i32>& parents) noexcept {
    const i32 my_index = static_cast<i32>(nodes.Size());
    nodes.PushBack(n);
    parents.PushBack(parent_index);
    for (u32 i = 0; i < n->ChildCount(); ++i)
        Flatten(n->Child(i), my_index, nodes, parents);
}

void WriteNode(FWriter& w, const FNode2D* n, i32 parent_index) noexcept {
    const FTransform2D& t = n->Local();
    w.I32(parent_index);
    w.F32(t.position.x); w.F32(t.position.y);
    w.F32(t.rotation);
    w.F32(t.scale.x);    w.F32(t.scale.y);
    w.U8(n->IsEnabled() ? 1u : 0u);
    w.U8(n->IsVisible() ? 1u : 0u);
    w.I32(n->SortLayer());
    w.F32(n->YSortBias());
    w.U8(static_cast<u8>(n->ChildDrawOrder()));

    // コンポーネント: ReflectName を持つ (= 反射型に解決できる) ものだけ書く。
    u32 comp_count = 0;
    for (u32 c = 0; c < n->ComponentCount(); ++c) {
        const FComponent2D* comp = n->ComponentAt(c);
        if (comp != nullptr && comp->ReflectName() != nullptr) ++comp_count;
    }
    w.U32(comp_count);
    for (u32 c = 0; c < n->ComponentCount(); ++c) {
        const FComponent2D* comp = n->ComponentAt(c);
        if (comp == nullptr || comp->ReflectName() == nullptr) continue;
        const char* nm   = comp->ReflectName();
        const u32   nlen = static_cast<u32>(std::strlen(nm));
        const u8    nl   = (nlen > 255u) ? 255u : static_cast<u8>(nlen);
        w.U8(nl);
        w.Bytes(nm, nl);
        // 値ペイロード (ReflectSerialize)。field-reflected なら値、RPROP のみなら空ヘッダ。
        u8 cbuf[4096];
        const u32 clen = SerializeByName(nm, comp, cbuf, sizeof(cbuf));
        w.U32(clen);
        w.Bytes(cbuf, clen);
    }
}

} // namespace

u32 SaveNodeTree(const FNode2D* root, u8* buf, u32 cap) noexcept {
    if (root == nullptr || buf == nullptr) return 0u;

    TArray<const FNode2D*> nodes;
    TArray<i32>            parents;
    Flatten(root, -1, nodes, parents);

    FWriter w(buf, cap);
    w.U32(kSceneSerializeMagic);
    w.U32(kSceneSerializeVersion);
    w.U32(static_cast<u32>(nodes.Size()));
    for (u32 i = 0; i < nodes.Size(); ++i)
        WriteNode(w, nodes[i], parents[i]);

    return w.ok ? w.cur : 0u;
}

TUniquePtr<FNode2D> LoadNodeTree(const u8* data, u32 size) noexcept {
    TUniquePtr<FNode2D> root;
    if (data == nullptr) return root;

    FReader r(data, size);
    if (r.U32() != kSceneSerializeMagic)   return root;
    if (r.U32() != kSceneSerializeVersion) return root;
    const u32 count = r.U32();
    if (!r.ok || count == 0u) return root;

    // 各ノードを生成し、親 index で木を組む (preorder なので親は子より先に作られる)。
    TArray<FNode2D*> ptrs;   // index → 生きた raw ポインタ
    TArray<u32>      depths; // index → root からの深度 (深度上限の判定用)
    for (u32 i = 0; i < count && r.ok; ++i) {
        const i32 parent_index = r.I32();
        FTransform2D t{};
        t.position.x = r.F32(); t.position.y = r.F32();
        t.rotation   = r.F32();
        t.scale.x    = r.F32(); t.scale.y    = r.F32();
        const u8  enabled = r.U8();
        const u8  visible = r.U8();
        const i32 layer   = r.I32();
        const f32 ybias   = r.F32();
        const u8  order   = r.U8();
        if (!r.ok) break;

        auto node = MakeUnique<FNode2D>();
        node->Local() = t;
        node->SetEnabled(enabled != 0u);
        node->SetVisible(visible != 0u);
        node->SetSortLayer(layer);
        node->SetYSortBias(ybias);
        node->SetChildDrawOrder(static_cast<FNode2D::EChildDrawOrder>(order));

        // コンポーネント: 名前で実体化 → attach → 値を復元 (まだツリーに繋ぐ前に行う)。
        const u32 ccount = r.U32();
        for (u32 c = 0; c < ccount && r.ok; ++c) {
            const u8 cnl = r.U8();
            char cname[256];
            r.Bytes(cname, cnl);
            cname[r.ok ? cnl : 0u] = '\0';
            const u32 clen = r.U32();
            if (!r.ok || clen > r.size - r.cur) { r.ok = false; break; }
            const u8* cpayload = r.data + r.cur;
            r.cur += clen;

            TUniquePtr<FComponent2D> comp = CreateComponentByName(cname);
            if (comp.Get() != nullptr) {
                FComponent2D* cp = comp.Get();
                node->AttachComponent(Move(comp));
                // 値を復元 (field-reflected のみ適用、RPROP のみなら no-op で factory 既定値が残る)。
                const FTypeDesc* cd = FTypeRegistry::Get().FindByName(cname);
                if (cd != nullptr && clen > 0u) DeserializeReflected(cd, cp, cpayload, clen);
            }
            // 実体化できない型 (Abstract 等) は attach をスキップ (payload は消費済み)。
        }
        if (!r.ok) break;

        if (parent_index < 0 && !root) {
            root = Move(node);                 // 最初の root
            ptrs.PushBack(root.Get());
            depths.PushBack(0u);
        } else {
            // 親が範囲内ならそこへ、でなければ root 直下へ (壊れた index への保険)。
            FNode2D* parent = (parent_index >= 0 && static_cast<u32>(parent_index) < ptrs.Size())
                              ? ptrs[static_cast<u32>(parent_index)]
                              : root.Get();
            u32 parent_depth = (parent_index >= 0 && static_cast<u32>(parent_index) < depths.Size())
                              ? depths[static_cast<u32>(parent_index)]
                              : 0u;
            if (parent == nullptr) { ptrs.PushBack(nullptr); depths.PushBack(0u); continue; }   // root 未確定の異常入力
            // 深度上限: 敵対的な深い親チェーンは root 直下へ付け替える (デストラクタの
            // 再帰スタックオーバーフロー防止。ノードは失わない)。
            if (parent_depth + 1u > kMaxTreeDepth) {
                parent       = root.Get();
                parent_depth = 0u;
            }
            FNode2D& added = parent->AddChild(Move(node));
            ptrs.PushBack(&added);
            depths.PushBack(parent_depth + 1u);
        }
    }
    return root;
}

} // namespace acs::game
