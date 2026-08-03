// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — SceneSerialize 実装。詳細はヘッダ参照。
// =============================================================================
#include "gameframework/SceneSerialize.h"
#include "gameframework/ANode.h"
#include "gameframework/AComponent.h"
#include "gameframework/Transform3D.h"
#include "gameframework/Reflect.h"            // CTypeRegistry / FTypeDesc
#include "gameframework/ReflectSerialize.h"   // SerializeByName / DeserializeReflected
#include "gameframework/ComponentFactory.h"   // CreateComponentByName
#include "container/Array.h"
#include "container/HashMap.h"
#include "foundation/Move.h"

#include <cstring>   // std::memcpy

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
    u32 Remaining() const noexcept { return ok ? size - cur : 0u; }
};

/** v4 のコンポーネントなしノードが占める最小バイト数。 */
constexpr u32 kV4MinimumNodeBytes = 63u;

struct FFlattenEntry {
    const ANode* Node = nullptr;
    i32 ParentIndex = -1;
    bool Exit = false;
};

/**
 * DFS pre-order でノードを平坦化する。
 *
 * 明示スタックを使うため、保存対象が深いツリーでも C++ 呼び出しスタックを消費しない。
 * Visiting/Complete の 2 状態を持ち、祖先への辺 (循環) と完了済みノードへの辺
 * (共有子/重複参照) を区別する。
 */
ESceneSerializeError Flatten(const ANode* root,
                             TArray<const ANode*>& nodes,
                             TArray<i32>& parents) noexcept {
    if (root == nullptr) return ESceneSerializeError::NullRoot;

    TArray<FFlattenEntry> stack;
    THashMap<const ANode*, u8> states;
    if (!stack.TryAdd(FFlattenEntry{root, -1, false}))
        return ESceneSerializeError::AllocationFailure;
    while (!stack.IsEmpty()) {
        const FFlattenEntry entry = stack.Last();
        stack.Pop();
        if (entry.Node == nullptr) return ESceneSerializeError::InvalidStructure;

        if (entry.Exit) {
            u8* state = states.Find(entry.Node);
            if (state == nullptr || *state != 1u)
                return ESceneSerializeError::InvalidStructure;
            *state = 2u;
            continue;
        }

        if (const u8* state = states.Find(entry.Node)) {
            return *state == 1u ? ESceneSerializeError::CyclicNodeGraph
                                : ESceneSerializeError::DuplicateNodeReference;
        }
        if (nodes.Num() >= kSceneSerializeMaxNodeCount)
            return ESceneSerializeError::NodeLimitExceeded;
        if (!states.TryAdd(entry.Node, 1u))
            return ESceneSerializeError::AllocationFailure;
        const i32 my_index = static_cast<i32>(nodes.Num());
        if (!nodes.TryAdd(entry.Node) || !parents.TryAdd(entry.ParentIndex))
            return ESceneSerializeError::AllocationFailure;
        if (!stack.TryAdd(FFlattenEntry{entry.Node, entry.ParentIndex, true}))
            return ESceneSerializeError::AllocationFailure;

        // 逆順に積むことで従来の再帰 DFS と同じ子順を保つ。
        const u32 child_count = entry.Node->ChildCount();
        if (child_count > kSceneSerializeMaxNodeCount - static_cast<u32>(nodes.Num()))
            return ESceneSerializeError::NodeLimitExceeded;
        for (u32 i = entry.Node->ChildCount(); i > 0u; --i) {
            const ANode* child = entry.Node->Child(i - 1u);
            if (child == nullptr) return ESceneSerializeError::InvalidStructure;
            if (!stack.TryAdd(FFlattenEntry{child, my_index, false}))
                return ESceneSerializeError::AllocationFailure;
        }
    }
    return nodes.IsEmpty() ? ESceneSerializeError::EmptyTree : ESceneSerializeError::None;
}

/** 最大 255 bytes の NUL 終端名を検証する。境界外を読まず、不正時は 0。 */
u32 ValidComponentNameLength(const char* name) noexcept {
    if (name == nullptr || name[0] == '\0') return 0u;
    for (u32 i = 1u; i <= 255u; ++i) {
        if (name[i] == '\0') return i;
    }
    return 0u;
}

bool CheckedAdd(u32& value, u32 increment) noexcept {
    constexpr u32 kU32Max = ~u32{0};
    if (increment > kU32Max - value) return false;
    value += increment;
    return true;
}

ESceneSerializeError MeasureNode(const ANode* node, u32& required_bytes,
                                 u32& total_components) noexcept {
    u32 serializable_components = 0u;
    for (u32 c = 0; c < node->ComponentCount(); ++c) {
        const AComponent* component = node->ComponentAt(c);
        if (component == nullptr || component->ReflectName() == nullptr) continue;
        if (serializable_components >= kSceneSerializeMaxComponentCountPerNode)
            return ESceneSerializeError::ComponentLimitExceeded;
        const u32 name_length = ValidComponentNameLength(component->ReflectName());
        if (name_length == 0u) return ESceneSerializeError::InvalidComponentName;
        ++serializable_components;
    }

    if (!CheckedAdd(required_bytes, kV4MinimumNodeBytes))
        return ESceneSerializeError::SerializedSizeOverflow;
    if (!CheckedAdd(total_components, serializable_components))
        return ESceneSerializeError::SerializedSizeOverflow;

    for (u32 c = 0; c < node->ComponentCount(); ++c) {
        const AComponent* component = node->ComponentAt(c);
        if (component == nullptr || component->ReflectName() == nullptr) continue;
        const char* name = component->ReflectName();
        const u32 name_length = ValidComponentNameLength(name);

        u8 payload[kSceneSerializeMaxComponentPayloadBytes];
        const u32 payload_length =
            SerializeByName(name, component, payload, sizeof(payload));
        if (payload_length == 0u) {
            return CTypeRegistry::Get().FindByName(name) == nullptr
                 ? ESceneSerializeError::InvalidComponentName
                 : ESceneSerializeError::ComponentPayloadLimitExceeded;
        }
        if (payload_length > kSceneSerializeMaxComponentPayloadBytes)
            return ESceneSerializeError::ComponentPayloadLimitExceeded;
        if (!CheckedAdd(required_bytes, 1u + name_length + 4u) ||
            !CheckedAdd(required_bytes, payload_length)) {
            return ESceneSerializeError::SerializedSizeOverflow;
        }
    }
    return ESceneSerializeError::None;
}

ESceneSerializeError WriteNode(FWriter& w, const ANode* n, i32 parent_index) noexcept {
    const FTransform3D& t = n->Local();
    w.I32(parent_index);
    w.F32(t.position.x); w.F32(t.position.y); w.F32(t.position.z);
    w.F32(t.rotation.x); w.F32(t.rotation.y);
    w.F32(t.rotation.z); w.F32(t.rotation.w);
    w.F32(t.scale.x);    w.F32(t.scale.y);    w.F32(t.scale.z);
    w.U8(n->IsEnabled() ? 1u : 0u);
    w.U8(n->IsVisible() ? 1u : 0u);
    w.I32(n->DrawLayer());
    w.I32(n->DrawPriority());
    w.U8(n->IsYSortEnabled() ? 1u : 0u);
    w.F32(n->YSortBias());

    // コンポーネント: ReflectName を持つ (= 反射型に解決できる) ものだけ書く。
    u32 comp_count = 0;
    for (u32 c = 0; c < n->ComponentCount(); ++c) {
        const AComponent* comp = n->ComponentAt(c);
        if (comp == nullptr || comp->ReflectName() == nullptr) continue;
        if (ValidComponentNameLength(comp->ReflectName()) == 0u)
            return ESceneSerializeError::InvalidComponentName;
        if (comp_count >= kSceneSerializeMaxComponentCountPerNode)
            return ESceneSerializeError::ComponentLimitExceeded;
        ++comp_count;
    }
    w.U32(comp_count);
    for (u32 c = 0; c < n->ComponentCount(); ++c) {
        const AComponent* comp = n->ComponentAt(c);
        if (comp == nullptr || comp->ReflectName() == nullptr) continue;
        const char* nm   = comp->ReflectName();
        const u32   nlen = ValidComponentNameLength(nm);
        const u8    nl   = static_cast<u8>(nlen);
        w.U8(nl);
        w.Bytes(nm, nl);
        // 値ペイロード (ReflectSerialize)。field-reflected なら値、RPROP のみなら空ヘッダ。
        u8 cbuf[kSceneSerializeMaxComponentPayloadBytes];
        const u32 clen = SerializeByName(nm, comp, cbuf, sizeof(cbuf));
        if (clen == 0u) {
            return CTypeRegistry::Get().FindByName(nm) == nullptr
                 ? ESceneSerializeError::InvalidComponentName
                 : ESceneSerializeError::ComponentPayloadLimitExceeded;
        }
        if (clen > kSceneSerializeMaxComponentPayloadBytes)
            return ESceneSerializeError::ComponentPayloadLimitExceeded;
        w.U32(clen);
        w.Bytes(cbuf, clen);
    }
    return w.ok ? ESceneSerializeError::None : ESceneSerializeError::BufferTooSmall;
}

u32 MinimumNodeBytes(u32 version) noexcept {
    if (version >= 4u) return kV4MinimumNodeBytes;
    return version == 3u ? 43u : 39u;
}

FSceneLoadResult LoadFailure(ESceneSerializeError error, u32 bytes_read,
                             u32 format_version = 0u) noexcept {
    return FSceneLoadResult{
        TObjectPtr<ANode>{}, error, bytes_read, format_version, 0u
    };
}

} // namespace

const char* SceneSerializeErrorName(ESceneSerializeError error) noexcept {
    switch (error) {
    case ESceneSerializeError::None:                         return "none";
    case ESceneSerializeError::NullInput:                    return "null_input";
    case ESceneSerializeError::TruncatedData:                return "truncated_data";
    case ESceneSerializeError::InvalidMagic:                 return "invalid_magic";
    case ESceneSerializeError::UnsupportedVersion:           return "unsupported_version";
    case ESceneSerializeError::EmptyTree:                    return "empty_tree";
    case ESceneSerializeError::NodeLimitExceeded:            return "node_limit_exceeded";
    case ESceneSerializeError::ComponentLimitExceeded:       return "component_limit_exceeded";
    case ESceneSerializeError::InvalidComponentName:         return "invalid_component_name";
    case ESceneSerializeError::ComponentPayloadLimitExceeded: return "component_payload_limit_exceeded";
    case ESceneSerializeError::InvalidComponentPayload:      return "invalid_component_payload";
    case ESceneSerializeError::InvalidStructure:             return "invalid_structure";
    case ESceneSerializeError::AllocationFailure:            return "allocation_failure";
    case ESceneSerializeError::NullRoot:                      return "null_root";
    case ESceneSerializeError::NullOutput:                    return "null_output";
    case ESceneSerializeError::BufferTooSmall:                return "buffer_too_small";
    case ESceneSerializeError::DuplicateNodeReference:        return "duplicate_node_reference";
    case ESceneSerializeError::CyclicNodeGraph:               return "cyclic_node_graph";
    case ESceneSerializeError::SerializedSizeOverflow:        return "serialized_size_overflow";
    case ESceneSerializeError::SceneChangedDuringSave:        return "scene_changed_during_save";
    }
    return "unknown";
}

FSceneSaveResult TrySaveNodeTree(const ANode* root, u8* buf, u32 cap) noexcept {
    FSceneSaveResult result{};
    if (root == nullptr) {
        result.Error = ESceneSerializeError::NullRoot;
        return result;
    }
    TArray<const ANode*> nodes;
    TArray<i32> parents;
    result.Error = Flatten(root, nodes, parents);
    if (result.Error != ESceneSerializeError::None) return result;

    result.RequiredBytes = 12u;
    result.NodeCount = static_cast<u32>(nodes.Num());
    for (u32 i = 0; i < nodes.Num(); ++i) {
        result.Error = MeasureNode(nodes[i], result.RequiredBytes, result.ComponentCount);
        if (result.Error != ESceneSerializeError::None) return result;
    }

    if (buf == nullptr) {
        result.Error = cap == 0u ? ESceneSerializeError::BufferTooSmall
                                 : ESceneSerializeError::NullOutput;
        return result;
    }
    if (cap < result.RequiredBytes) {
        result.Error = ESceneSerializeError::BufferTooSmall;
        return result;
    }

    FWriter w(buf, cap);
    w.U32(kSceneSerializeMagic);
    w.U32(kSceneSerializeVersion);
    w.U32(static_cast<u32>(nodes.Num()));
    for (u32 i = 0; i < nodes.Num(); ++i) {
        result.Error = WriteNode(w, nodes[i], parents[i]);
        if (result.Error != ESceneSerializeError::None) {
            result.BytesWritten = w.cur;
            return result;
        }
    }
    result.BytesWritten = w.cur;
    if (!w.ok) {
        result.Error = ESceneSerializeError::BufferTooSmall;
        return result;
    }
    if (result.BytesWritten != result.RequiredBytes) {
        result.Error = ESceneSerializeError::SceneChangedDuringSave;
        return result;
    }
    result.Error = ESceneSerializeError::None;
    return result;
}

u32 SaveNodeTree(const ANode* root, u8* buf, u32 cap) noexcept {
    const FSceneSaveResult result = TrySaveNodeTree(root, buf, cap);
    return result.Succeeded() ? result.BytesWritten : 0u;
}

FSceneLoadResult TryLoadNodeTree(const u8* data, u32 size) noexcept {
    TObjectPtr<ANode> root;
    if (data == nullptr) return LoadFailure(ESceneSerializeError::NullInput, 0u);
    if (size < 12u) return LoadFailure(ESceneSerializeError::TruncatedData, 0u);

    FReader r(data, size);
    if (r.U32() != kSceneSerializeMagic)
        return LoadFailure(ESceneSerializeError::InvalidMagic, r.cur);
    const u32 version = r.U32();
    if (version < 2u || version > kSceneSerializeVersion)
        return LoadFailure(ESceneSerializeError::UnsupportedVersion, r.cur, version);
    const u32 count = r.U32();
    if (!r.ok) return LoadFailure(ESceneSerializeError::TruncatedData, r.cur, version);
    if (count == 0u) return LoadFailure(ESceneSerializeError::EmptyTree, r.cur, version);
    if (count > kSceneSerializeMaxNodeCount)
        return LoadFailure(ESceneSerializeError::NodeLimitExceeded, r.cur, version);
    if (count > r.Remaining() / MinimumNodeBytes(version))
        return LoadFailure(ESceneSerializeError::TruncatedData, r.cur, version);

    // 各ノードを生成し、親 index で木を組む (preorder なので親は子より先に作られる)。
    TArray<ANode*> ptrs;   // index → 生きた raw ポインタ
    TArray<u32> depths;    // index → root からの深度 (深度上限の判定用)
    if (!ptrs.TryReserve(count) || !depths.TryReserve(count))
        return LoadFailure(ESceneSerializeError::AllocationFailure, r.cur, version);

    u32 depth_capped_count = 0u;
    for (u32 i = 0; i < count; ++i) {
        const i32 parent_index = r.I32();
        if (!r.ok) return LoadFailure(ESceneSerializeError::TruncatedData, r.cur, version);
        if ((i == 0u && parent_index != -1) ||
            (i > 0u && (parent_index < 0 || static_cast<u32>(parent_index) >= i))) {
            return LoadFailure(ESceneSerializeError::InvalidStructure, r.cur, version);
        }

        FTransform3D t = FTransform3D::Identity();
        if (version >= 4u) {
            t.position.x = r.F32(); t.position.y = r.F32(); t.position.z = r.F32();
            t.rotation.x = r.F32(); t.rotation.y = r.F32();
            t.rotation.z = r.F32(); t.rotation.w = r.F32();
            t.scale.x    = r.F32(); t.scale.y    = r.F32(); t.scale.z = r.F32();
        } else {
            FTransform2D legacy{};
            legacy.position.x = r.F32(); legacy.position.y = r.F32();
            legacy.rotation   = r.F32();
            legacy.scale.x    = r.F32(); legacy.scale.y = r.F32();
            t.position = FVec3{legacy.position.x, legacy.position.y, 0.0f};
            t.rotation = FQuat::AxisAngle(FVec3{0.0f, 0.0f, 1.0f}, legacy.rotation);
            t.scale    = FVec3{legacy.scale.x, legacy.scale.y, 1.0f};
        }
        const u8  enabled = r.U8();
        const u8  visible = r.U8();
        const i32 layer   = r.I32();
        i32       priority = 0;
        u8        ysort    = 0u;
        f32       ybias    = 0.0f;
        if (version >= 3u) {
            priority = r.I32();
            ysort    = r.U8();
            ybias    = r.F32();
        } else {
            ybias = r.F32();
            const u8 legacy_child_order = r.U8();
            ysort = (legacy_child_order == 2u) ? 1u : 0u;
        }
        if (!r.ok) return LoadFailure(ESceneSerializeError::TruncatedData, r.cur, version);

        auto node = NewObject<ANode>();
        if (!node) return LoadFailure(ESceneSerializeError::AllocationFailure, r.cur, version);
        node->Local() = t;
        node->SetEnabled(enabled != 0u);
        node->SetVisible(visible != 0u);
        node->SetDrawLayer(layer);
        node->SetDrawPriority(priority);
        node->SetYSortEnabled(ysort != 0u);
        node->SetYSortBias(ybias);

        // コンポーネント: 名前で実体化 → attach → 値を復元 (まだツリーに繋ぐ前に行う)。
        const u32 ccount = r.U32();
        if (!r.ok) return LoadFailure(ESceneSerializeError::TruncatedData, r.cur, version);
        if (ccount > kSceneSerializeMaxComponentCountPerNode)
            return LoadFailure(ESceneSerializeError::ComponentLimitExceeded, r.cur, version);
        for (u32 c = 0; c < ccount; ++c) {
            const u8 cnl = r.U8();
            if (!r.ok) return LoadFailure(ESceneSerializeError::TruncatedData, r.cur, version);
            if (cnl == 0u)
                return LoadFailure(ESceneSerializeError::InvalidComponentName, r.cur, version);
            char cname[256];
            r.Bytes(cname, cnl);
            if (!r.ok) return LoadFailure(ESceneSerializeError::TruncatedData, r.cur, version);
            cname[cnl] = '\0';
            const u32 clen = r.U32();
            if (!r.ok) return LoadFailure(ESceneSerializeError::TruncatedData, r.cur, version);
            if (clen > kSceneSerializeMaxComponentPayloadBytes)
                return LoadFailure(ESceneSerializeError::ComponentPayloadLimitExceeded, r.cur, version);
            if (clen > r.Remaining())
                return LoadFailure(ESceneSerializeError::TruncatedData, r.cur, version);
            const u8* cpayload = r.data + r.cur;
            r.cur += clen;

            TUniquePtr<AComponent> comp = CreateComponentByName(cname);
            if (comp.Get() != nullptr) {
                AComponent* cp = comp.Get();
                const FTypeDesc* cd = CTypeRegistry::Get().FindByName(cname);
                if (cd == nullptr || clen == 0u) {
                    return LoadFailure(ESceneSerializeError::InvalidComponentPayload,
                                       r.cur, version);
                }
                const FReflectDeserializeResult reflected =
                    TryDeserializeReflected(cd, cp, cpayload, clen);
                if (!reflected.Succeeded() || reflected.BytesRead != clen) {
                    return LoadFailure(ESceneSerializeError::InvalidComponentPayload,
                                       r.cur, version);
                }
                // 完全検証後だけ attach する。RPROP のみならヘッダだけを消費し既定値を保つ。
                node->AttachComponent(Move(comp));
            }
            // 実体化できない型 (Abstract 等) は attach をスキップ (payload は消費済み)。
        }

        if (i == 0u) {
            root = Move(node);
            if (!ptrs.TryAdd(root.Get()) || !depths.TryAdd(0u))
                return LoadFailure(ESceneSerializeError::AllocationFailure, r.cur, version);
        } else {
            ANode* parent = ptrs[static_cast<u32>(parent_index)];
            u32 parent_depth = depths[static_cast<u32>(parent_index)];
            // 深度上限: 敵対的な深い親チェーンは root 直下へ付け替える (デストラクタの
            // 再帰スタックオーバーフロー防止。ノードは失わない)。
            if (parent_depth + 1u > kSceneSerializeMaxTreeDepth) {
                parent       = root.Get();
                parent_depth = 0u;
                ++depth_capped_count;
            }
            ANode& added = parent->AddChild(Move(node));
            if (!ptrs.TryAdd(&added) || !depths.TryAdd(parent_depth + 1u))
                return LoadFailure(ESceneSerializeError::AllocationFailure, r.cur, version);
        }
    }
    return FSceneLoadResult{
        Move(root), ESceneSerializeError::None, r.cur, version, depth_capped_count
    };
}

TObjectPtr<ANode> LoadNodeTree(const u8* data, u32 size) noexcept {
    FSceneLoadResult result = TryLoadNodeTree(data, size);
    return result.Succeeded() ? Move(result.Root) : TObjectPtr<ANode>{};
}

} // namespace acs::game
