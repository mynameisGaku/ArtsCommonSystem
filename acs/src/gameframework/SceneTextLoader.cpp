// SPDX-License-Identifier: Apache-2.0
#include "gameframework/SceneTextLoader.h"

#include "gameframework/ANode.h"
#include "gameframework/AComponent.h"
#include "gameframework/AssetPack.h"
#include "gameframework/ComponentFactory.h"   // CreateComponentByName
#include "gameframework/ReflectApply.h"        // ApplyFieldValue
#include "gameframework/Reflect.h"             // CTypeRegistry / FTypeDesc
#include "gameframework/PrimitiveRenderer2D.h" // APrimitiveRenderer2D (builtin の typed setter)
#include "gameframework/PolygonRenderer2D.h"   // APolygonRenderer2D (POLY 行の描画)
#include "gameframework/Sprite2DComponent.h"   // ASprite2DComponent (SPRT 行の描画)
#include "gameframework/Material2D.h"           // FMaterial2D (MAT 行 = 使用マテリアル)
#include "asset/ImageAsset.h"                  // ImageAssetLoader (画像デコード)
#include "render/RenderAssets.h"               // UploadTexture (GPU テクスチャ化)
#include "render/IRhiTexture.h"
#include "container/Array.h"
#include "foundation/Move.h"

#include <cerrno>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace acs::game {

namespace {

/** ノード行 1 件分の解析結果 (親解決と visual 設定のため保持)。 */
struct FLoadedNode {
    int      id     = 0;
    int      parent = -1;
    ANode* node   = nullptr;
    FVec4    color{ 0.6f, 0.7f, 0.9f, 1.0f };
    f32      base   = 48.0f;
    // 剛体 (ARigidBody2D)。COMP の論理 slot 順を comp_count で数え、rigid_slot で CPROP を拾う。
    int      comp_count = 0;        // この node の COMP 行数 (= 論理 slot)
    int      rigid_slot = -1;       // ARigidBody2D の論理 slot (-1=無し)
    f32      rb[6]      = { 1.0f, 0.1f, 0.5f, 1.0f, 0.05f, 0.1f };  // bodyType,rest,fric,mass,linD,angD
    FVec2    poly[kMaxPolyVerts]{}; // POLY 行のローカル頂点 (物理の polygon 用)
    u32      polyCount  = 0u;
    // NFLG (ノードフラグ): visible/enabled/sortLayer。既定値は ANode の既定と一致。
    bool     visible    = true;
    bool     enabled    = true;
    i32      sortLayer  = 0;
    bool     hasNflg    = false;     // NFLG 行が存在したか
    // RPLY (描画用滑らか頂点)。POLY がコライダー用、RPLY が描画用。
    FVec2    renderVerts[APolygonRenderer2D::kMaxVerts]{};
    u32      renderCount = 0u;
};

struct FPreflightNode {
    int id = 0;
    int parent = -1;
    int parent_index = -1;
    u32 logical_components = 0u;
    u32 attached_components = 0u;
    u32 hierarchy_depth = 0u;
    u8 hierarchy_state = 0u;
};

struct FSceneTextPreflight {
    FSceneTextLoadResult result{};
    u32 max_relative_depth = 0u;
};

/** id からノードのインデックスを引く (線形)。 */
int FindLoadedIndex(const TArray<FLoadedNode>& nodes, int id) noexcept {
    for (u32 i = 0; i < nodes.Num(); ++i)
        if (nodes[i].id == id) return static_cast<int>(i);
    return -1;
}

int FindPreflightIndexLinear(const TArray<FPreflightNode>& nodes, int id) noexcept {
    for (u32 i = 0; i < nodes.Num(); ++i) {
        if (nodes[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

int FindPreflightIndex(const TArray<FPreflightNode>& nodes, int id) noexcept {
    u32 first = 0u;
    u32 count = nodes.Num();
    while (count > 0u) {
        const u32 step = count / 2u;
        const u32 index = first + step;
        if (nodes[index].id < id) {
            first = index + 1u;
            count -= step + 1u;
        } else {
            count = step;
        }
    }
    return first < nodes.Num() && nodes[first].id == id ? static_cast<int>(first) : -1;
}

bool IsFinite(f32 value) noexcept {
    return std::isfinite(static_cast<double>(value));
}

bool OnlyWhitespace(const char* text) noexcept {
    if (text == nullptr) return false;
    while (*text != '\0') {
        if (std::isspace(static_cast<unsigned char>(*text)) == 0) return false;
        ++text;
    }
    return true;
}

void SkipWhitespace(const char*& text) noexcept {
    while (*text != '\0' && std::isspace(static_cast<unsigned char>(*text)) != 0) ++text;
}

bool IsTokenBoundary(char c) noexcept {
    return c == '\0' || std::isspace(static_cast<unsigned char>(c)) != 0;
}

bool ParseIntToken(const char*& text, int& value) noexcept {
    SkipWhitespace(text);
    if (*text == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(text, &end, 10);
    if (end == text || errno == ERANGE || !IsTokenBoundary(*end) ||
        parsed < static_cast<long long>(INT_MIN) ||
        parsed > static_cast<long long>(INT_MAX)) {
        return false;
    }
    value = static_cast<int>(parsed);
    text = end;
    return true;
}

bool ParseU32Token(const char*& text, u32& value) noexcept {
    SkipWhitespace(text);
    if (*text == '\0' || *text == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || errno == ERANGE || !IsTokenBoundary(*end) ||
        parsed > static_cast<unsigned long long>(~u32{0})) {
        return false;
    }
    value = static_cast<u32>(parsed);
    text = end;
    return true;
}

bool ParseFloatToken(const char*& text, f32& value) noexcept {
    SkipWhitespace(text);
    if (*text == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const f32 parsed = std::strtof(text, &end);
    if (end == text || errno == ERANGE || !IsTokenBoundary(*end) || !IsFinite(parsed))
        return false;
    value = parsed;
    text = end;
    return true;
}

bool ParseWordToken(const char*& text, char* out, usize capacity) noexcept {
    SkipWhitespace(text);
    const char* begin = text;
    while (*text != '\0' && std::isspace(static_cast<unsigned char>(*text)) == 0) ++text;
    const usize length = static_cast<usize>(text - begin);
    if (length == 0u || length >= capacity) return false;
    std::memcpy(out, begin, length);
    out[length] = '\0';
    return true;
}

bool IsDirective(const char* line, const char* name) noexcept {
    const usize n = std::strlen(name);
    return std::strncmp(line, name, n) == 0 &&
           (line[n] == '\0' || std::isspace(static_cast<unsigned char>(line[n])) != 0);
}

template<typename T>
bool TryPrepareAppend(const TArray<T>& destination, const TArray<T>& source,
                      TArray<T>& prepared) noexcept {
    const usize destination_size = destination.Num();
    const usize source_size = source.Num();
    if (source_size > (~usize{0} - destination_size)) return false;
    if (!prepared.TryReserve(destination_size + source_size)) return false;
    for (usize i = 0u; i < destination_size; ++i) {
        if (!prepared.TryAdd(destination[i])) return false;
    }
    for (usize i = 0u; i < source_size; ++i) {
        if (!prepared.TryAdd(source[i])) return false;
    }
    return true;
}

template<typename T>
IAllocator& ArrayAllocatorOrDefault(TArray<T>* array) noexcept {
    return array != nullptr ? *array->GetAllocator() : DefaultAllocator();
}

ESceneTextLoadError ReadCheckedLine(const char*& cursor, char* out, u32 out_size,
                                    u32& line_number, bool& has_line) noexcept {
    has_line = false;
    if (*cursor == '\0') return ESceneTextLoadError::None;

    ++line_number;
    u32 length = 0u;
    const char* q = cursor;
    while (*q != '\0' && *q != '\n') {
        if (length >= kSceneTextMaxLineBytes || length + 1u >= out_size)
            return ESceneTextLoadError::LineTooLong;
        out[length++] = *q++;
    }
    if (length > 0u && out[length - 1u] == '\r') --length;
    out[length] = '\0';
    cursor = (*q == '\n') ? q + 1 : q;
    has_line = true;
    return ESceneTextLoadError::None;
}

FSceneTextPreflight PreflightAcsceneText(const char* text, const ANode& root) noexcept {
    FSceneTextPreflight checked;
    if (text == nullptr) {
        checked.result.Error = ESceneTextLoadError::NullText;
        return checked;
    }
    if (root.IsPendingDestroy()) {
        checked.result.Error = ESceneTextLoadError::TargetPendingDestroy;
        return checked;
    }

    usize text_size = 0u;
    while (text_size <= kSceneTextMaxBytes && text[text_size] != '\0') ++text_size;
    if (text_size > kSceneTextMaxBytes) {
        checked.result.Error = ESceneTextLoadError::TextTooLarge;
        return checked;
    }

    const char* cursor = text;
    char line[kSceneTextMaxLineBytes + 1u]{};
    u32 line_number = 0u;
    bool has_line = false;
    auto read = [&]() noexcept {
        return ReadCheckedLine(cursor, line, static_cast<u32>(sizeof(line)), line_number, has_line);
    };
    auto fail = [&](ESceneTextLoadError error) noexcept {
        checked.result.Error = error;
        checked.result.Line = line_number;
        return checked;
    };

    ESceneTextLoadError read_error = read();
    if (read_error != ESceneTextLoadError::None) return fail(read_error);
    if (!has_line || std::strncmp(line, "ACSCENE", 7) != 0 ||
        (line[7] != '\0' && std::isspace(static_cast<unsigned char>(line[7])) == 0)) {
        return fail(ESceneTextLoadError::InvalidHeader);
    }

    read_error = read();
    if (read_error != ESceneTextLoadError::None) return fail(read_error);
    int node_count = 0;
    const char* values = line;
    if (!has_line || !ParseIntToken(values, node_count) || !OnlyWhitespace(values)) {
        return fail(ESceneTextLoadError::MissingNodeCount);
    }
    if (node_count < 0 || static_cast<u32>(node_count) > kSceneTextMaxNodes) {
        return fail(ESceneTextLoadError::NodeLimitExceeded);
    }

    TArray<FPreflightNode> nodes;
    for (int i = 0; i < node_count; ++i) {
        read_error = read();
        if (read_error != ESceneTextLoadError::None) return fail(read_error);
        if (!has_line) return fail(ESceneTextLoadError::TruncatedNodeList);

        FPreflightNode node;
        f32 x = 0.0f, y = 0.0f, rotation = 0.0f, sx = 0.0f, sy = 0.0f;
        f32 base = 0.0f, r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
        values = line;
        if (!ParseIntToken(values, node.id) || !ParseIntToken(values, node.parent) ||
            !ParseFloatToken(values, x) || !ParseFloatToken(values, y) ||
            !ParseFloatToken(values, rotation) || !ParseFloatToken(values, sx) ||
            !ParseFloatToken(values, sy) || !ParseFloatToken(values, base) ||
            !ParseFloatToken(values, r) || !ParseFloatToken(values, g) ||
            !ParseFloatToken(values, b) || !ParseFloatToken(values, a) ||
            node.id < 0) {
            return fail(ESceneTextLoadError::InvalidNodeRecord);
        }
        if (FindPreflightIndexLinear(nodes, node.id) >= 0)
            return fail(ESceneTextLoadError::DuplicateNodeId);
        nodes.Add(node);
    }

    // 小さな検証 record を一度 sort し、record 上限近くの文書でも全 directive 参照を
    // O(log n) で解決する。
    for (u32 i = 1u; i < nodes.Num(); ++i) {
        const FPreflightNode value = nodes[i];
        u32 insertion = i;
        while (insertion > 0u && nodes[insertion - 1u].id > value.id) {
            nodes[insertion] = nodes[insertion - 1u];
            --insertion;
        }
        nodes[insertion] = value;
    }

    for (u32 i = 0; i < nodes.Num(); ++i) {
        if (nodes[i].parent < 0) continue;
        const int parent_index = FindPreflightIndex(nodes, nodes[i].parent);
        if (parent_index < 0 || static_cast<u32>(parent_index) == i)
            return fail(parent_index < 0 ? ESceneTextLoadError::InvalidNodeReference
                                         : ESceneTextLoadError::HierarchyCycle);
        nodes[i].parent_index = parent_index;
    }

    TArray<u32> hierarchy_chain;
    for (u32 i = 0; i < nodes.Num(); ++i) {
        if (nodes[i].hierarchy_state == 2u) continue;
        hierarchy_chain.Reset();
        int current = static_cast<int>(i);
        while (current >= 0 && nodes[static_cast<u32>(current)].hierarchy_state != 2u) {
            FPreflightNode& node = nodes[static_cast<u32>(current)];
            if (node.hierarchy_state == 1u)
                return fail(ESceneTextLoadError::HierarchyCycle);
            node.hierarchy_state = 1u;
            hierarchy_chain.Add(static_cast<u32>(current));
            current = node.parent_index;
        }

        u32 depth = current >= 0
                        ? nodes[static_cast<u32>(current)].hierarchy_depth + 1u
                        : 0u;
        while (!hierarchy_chain.IsEmpty()) {
            const u32 index = hierarchy_chain[hierarchy_chain.Num() - 1u];
            hierarchy_chain.Pop();
            nodes[index].hierarchy_depth = depth++;
            nodes[index].hierarchy_state = 2u;
            if (nodes[index].hierarchy_depth > checked.max_relative_depth)
                checked.max_relative_depth = nodes[index].hierarchy_depth;
        }
    }
    if (nodes.Num() > 0u &&
        (root.TreeDepth() >= kNodeMaxTreeDepth ||
         checked.max_relative_depth > kNodeMaxTreeDepth - root.TreeDepth() - 1u)) {
        return fail(ESceneTextLoadError::TreeDepthLimitExceeded);
    }

    while (true) {
        read_error = read();
        if (read_error != ESceneTextLoadError::None) return fail(read_error);
        if (!has_line) break;
        if (line[0] == '\0') continue;
        ++checked.result.DirectivesRead;
        if (checked.result.DirectivesRead > kSceneTextMaxDirectiveRecords)
            return fail(ESceneTextLoadError::DirectiveLimitExceeded);

        if (IsDirective(line, "COMP")) {
            int id = 0;
            char type[128]{};
            values = line + 4;
            if (!ParseIntToken(values, id) ||
                !ParseWordToken(values, type, sizeof(type)) ||
                !OnlyWhitespace(values)) {
                return fail(ESceneTextLoadError::InvalidDirective);
            }
            const int index = FindPreflightIndex(nodes, id);
            if (index < 0) return fail(ESceneTextLoadError::InvalidNodeReference);
            FPreflightNode& node = nodes[static_cast<u32>(index)];
            ++node.logical_components;
            ++node.attached_components;
            if (node.logical_components > kSceneTextMaxComponentsPerNode ||
                node.attached_components > kSceneTextMaxComponentsPerNode) {
                return fail(ESceneTextLoadError::ComponentLimitExceeded);
            }
            continue;
        }
        if (IsDirective(line, "CPROP")) {
            int id = 0;
            u32 slot = 0u, property = 0u;
            f32 field_values[4]{};
            values = line + 5;
            if (!ParseIntToken(values, id) || !ParseU32Token(values, slot) ||
                !ParseU32Token(values, property) ||
                !ParseFloatToken(values, field_values[0])) {
                return fail(ESceneTextLoadError::InvalidDirective);
            }
            for (u32 i = 1u; i < 4u && !OnlyWhitespace(values); ++i) {
                if (!ParseFloatToken(values, field_values[i]))
                    return fail(ESceneTextLoadError::InvalidDirective);
            }
            if (!OnlyWhitespace(values)) return fail(ESceneTextLoadError::InvalidDirective);
            const int index = FindPreflightIndex(nodes, id);
            if (index < 0) return fail(ESceneTextLoadError::InvalidNodeReference);
            if (slot >= nodes[static_cast<u32>(index)].logical_components)
                return fail(ESceneTextLoadError::InvalidDirective);
            continue;
        }
        if (IsDirective(line, "POLY") || IsDirective(line, "RPLY")) {
            const bool is_poly = IsDirective(line, "POLY");
            int id = 0, count = 0;
            values = line + 4;
            if (!ParseIntToken(values, id) || !ParseIntToken(values, count) ||
                count < 3 || count > static_cast<int>(APolygonRenderer2D::kMaxVerts)) {
                return fail(ESceneTextLoadError::InvalidDirective);
            }
            const int index = FindPreflightIndex(nodes, id);
            if (index < 0) return fail(ESceneTextLoadError::InvalidNodeReference);
            for (int i = 0; i < count; ++i) {
                f32 x = 0.0f, y = 0.0f;
                if (!ParseFloatToken(values, x) || !ParseFloatToken(values, y)) {
                    return fail(ESceneTextLoadError::InvalidDirective);
                }
            }
            if (!OnlyWhitespace(values)) return fail(ESceneTextLoadError::InvalidDirective);
            if (is_poly) {
                FPreflightNode& node = nodes[static_cast<u32>(index)];
                ++node.attached_components;
                if (node.attached_components > kSceneTextMaxComponentsPerNode)
                    return fail(ESceneTextLoadError::ComponentLimitExceeded);
            }
            continue;
        }
        if (IsDirective(line, "SPRT") || IsDirective(line, "MAT")) {
            const bool is_sprite = IsDirective(line, "SPRT");
            int id = 0;
            values = line + (is_sprite ? 4 : 3);
            if (!ParseIntToken(values, id))
                return fail(ESceneTextLoadError::InvalidDirective);
            if (FindPreflightIndex(nodes, id) < 0)
                return fail(ESceneTextLoadError::InvalidNodeReference);
            SkipWhitespace(values);
            const char* path = values;
            const usize path_size = std::strlen(path);
            if (path_size == 0u) return fail(ESceneTextLoadError::InvalidDirective);
            if (path_size > kSceneTextMaxPathBytes)
                return fail(ESceneTextLoadError::PathTooLong);
            continue;
        }
        if (IsDirective(line, "NFLG")) {
            int id = 0, visible = 1, enabled = 1, layer = 0;
            values = line + 4;
            if (!ParseIntToken(values, id) || !ParseIntToken(values, visible))
                return fail(ESceneTextLoadError::InvalidDirective);
            if (!OnlyWhitespace(values) && !ParseIntToken(values, enabled))
                return fail(ESceneTextLoadError::InvalidDirective);
            if (!OnlyWhitespace(values) && !ParseIntToken(values, layer))
                return fail(ESceneTextLoadError::InvalidDirective);
            if (!OnlyWhitespace(values)) return fail(ESceneTextLoadError::InvalidDirective);
            if (FindPreflightIndex(nodes, id) < 0)
                return fail(ESceneTextLoadError::InvalidNodeReference);
            continue;
        }
        if (IsDirective(line, "SEL")) continue;
        // forward compatibility のため、未知 record は意図的に無視する。
    }

    checked.result.NodesLoaded = static_cast<u32>(nodes.Num());
    return checked;
}

/** comp が APrimitiveRenderer2D ならそのポインタを返す (反射名で判定)。 */
APrimitiveRenderer2D* AsPrimitive(AComponent* c) noexcept {
    if (c == nullptr) return nullptr;
    const char* n = c->ReflectName();
    return (n != nullptr && std::strcmp(n, "APrimitiveRenderer2D") == 0)
               ? static_cast<APrimitiveRenderer2D*>(c) : nullptr;
}

bool ReadAssetPackEntry(IAssetPackReader& pack, const char* virtual_path,
                        TArray<byte>& out, bool nul_terminate) noexcept {
    if (virtual_path == nullptr || virtual_path[0] == '\0') return false;
    const auto size_result = pack.FileSize(virtual_path);
    if (size_result.IsErr()) return false;
    const u64 size64 = size_result.Value();
    const usize size = static_cast<usize>(size64);
    if (static_cast<u64>(size) != size64 ||
        (nul_terminate && size == static_cast<usize>(-1))) {
        return false;
    }
    const usize storage = size + (nul_terminate ? 1u : 0u);
    if (!out.TrySetNum(storage)) return false;
    if (size > 0u) {
        const auto read_result =
            pack.ReadFile(virtual_path, out.GetData(), size64);
        if (read_result.IsErr()) {
            out.Reset();
            return false;
        }
    }
    if (nul_terminate) out[size] = byte{0};
    return true;
}

} // namespace

static FSceneBounds LoadAcsceneTextValidated(const char* text, ANode& root,
                                             TArray<FSpriteRequest>* out_sprites,
                                             TArray<FRigidBodyRequest>* out_bodies,
                                             TArray<FMaterialTexRequest>* out_mat_tex,
                                             ANode** out_root,
                                             IAssetPackReader* pack,
                                             bool* pack_asset_failure) noexcept {
    FSceneBounds bounds;
    if (out_root != nullptr) *out_root = nullptr;
    if (pack_asset_failure != nullptr) *pack_asset_failure = false;
    if (text == nullptr) return bounds;

    const char* p = text;
    char line[2048];
    auto read_line = [&](char* out, int outsz) -> bool {
        if (*p == '\0') return false;
        int k = 0;
        while (*p != '\0' && *p != '\n') { if (k < outsz - 1) out[k++] = *p; ++p; }
        if (k > 0 && out[k - 1] == '\r') --k;
        out[k] = '\0';
        if (*p == '\n') ++p;
        return true;
    };

    if (!read_line(line, sizeof(line)) || std::strncmp(line, "ACSCENE", 7) != 0) return bounds;
    if (!read_line(line, sizeof(line))) return bounds;
    int count = 0;
    std::sscanf(line, "%d", &count);

    TArray<FLoadedNode> nodes;

    // --- ノード行: root 直下に平坦生成 (親付けは後で) ---
    for (int i = 0; i < count; ++i) {
        if (!read_line(line, sizeof(line))) break;
        FLoadedNode ln;
        float x = 0, y = 0, rot = 0, sx = 1, sy = 1, base = 48, r = 0.6f, g = 0.7f, b = 0.9f, a = 1;
        int consumed = 0;
        const int got = std::sscanf(line, "%d %d %f %f %f %f %f %f %f %f %f %f %n",
            &ln.id, &ln.parent, &x, &y, &rot, &sx, &sy, &base, &r, &g, &b, &a, &consumed);
        if (got < 12) continue;
        ANode& child = root.AddChild(NewObject<ANode>());
        child.SetLocal2D(FTransform2D{ FVec2{ x, y }, rot, FVec2{ sx, sy } });
        child.ManagementAccess().SetSerialId(ln.id);   // オブジェクト参照の解決キー (.acscene の id)
        ln.node = &child; ln.base = base; ln.color = FVec4{ r, g, b, a };
        nodes.Add(ln);
    }

    // --- COMP / CPROP を取り込む (COMP→CPROP の順に並ぶ) ---
    while (read_line(line, sizeof(line))) {
        if (IsDirective(line, "COMP")) {                     // COMP <id> <type>
            int nid = 0; char type[128] = {};
            if (std::sscanf(line, "COMP %d %127s", &nid, type) >= 2) {
                const int idx = FindLoadedIndex(nodes, nid);
                if (idx >= 0) {
                    FLoadedNode& ln = nodes[static_cast<u32>(idx)];
                    if (std::strcmp(type, "ARigidBody2D") == 0) {
                        ln.rigid_slot = ln.comp_count;   // default-ctor 不可 → attach せず物理 request で扱う
                    } else {
                        TUniquePtr<AComponent> c = CreateComponentByName(type);
                        if (c) ln.node->AttachComponent(Move(c));
                    }
                    ++ln.comp_count;                     // 論理 slot は «attach 有無に関わらず» 進める
                }
            }
            continue;
        }
        if (IsDirective(line, "CPROP")) {                    // CPROP <id> <slot> <prop> <x y z w>
            int nid = 0; unsigned slot = 0, prop = 0;
            float v0 = 0, v1 = 0, v2 = 0, v3 = 0;
            if (std::sscanf(line, "CPROP %d %u %u %f %f %f %f", &nid, &slot, &prop, &v0, &v1, &v2, &v3) >= 4) {
                const int idx = FindLoadedIndex(nodes, nid);
                if (idx >= 0) {
                    FLoadedNode& ln = nodes[static_cast<u32>(idx)];
                    if (ln.rigid_slot == static_cast<int>(slot)) {       // ARigidBody2D の prop → 物理 request へ
                        if (prop < 6) ln.rb[prop] = v0;
                    } else {
                        // ARigidBody2D を attach していない分 slot がずれるので補正 (それより後ろは -1)。
                        const u32 actual = (ln.rigid_slot >= 0 && static_cast<int>(slot) > ln.rigid_slot)
                                               ? slot - 1u : slot;
                        AComponent* c = ln.node->ComponentAt(actual);
                        if (c != nullptr) {
                            const f32 v[4] = { v0, v1, v2, v3 };
                            if (APrimitiveRenderer2D* pr = AsPrimitive(c)) {
                                if (prop == 0) pr->SetShape(static_cast<APrimitiveRenderer2D::EShape>(static_cast<int>(v0)));
                            } else {
                                const FTypeDesc* d = CTypeRegistry::Get().FindByName(c->ReflectName());
                                if (d != nullptr && d->fields != nullptr && prop < d->field_count)
                                    ApplyFieldValue(static_cast<void*>(c), d->fields[prop], v);
                            }
                        }
                    }
                }
            }
            continue;
        }
        if (IsDirective(line, "POLY")) {                     // POLY <id> <count> <x0 y0 x1 y1 ...>
            int nid = 0, cnt = 0, consumed = 0;
            if (std::sscanf(line, "POLY %d %d %n", &nid, &cnt, &consumed) >= 2 && cnt >= 3) {
                const int idx = FindLoadedIndex(nodes, nid);
                if (idx >= 0) {
                    if (cnt > static_cast<int>(APolygonRenderer2D::kMaxVerts)) cnt = APolygonRenderer2D::kMaxVerts;
                    FVec2 verts[APolygonRenderer2D::kMaxVerts];
                    const char* q = line + consumed;
                    int parsed = 0;
                    for (; parsed < cnt; ++parsed) {
                        int c2 = 0;
                        if (std::sscanf(q, "%f %f %n", &verts[parsed].x, &verts[parsed].y, &c2) < 2) break;
                        q += c2;
                    }
                    if (parsed >= 3) {
                        FLoadedNode& ln = nodes[static_cast<u32>(idx)];
                        APolygonRenderer2D& poly = ln.node->AddComponent<APolygonRenderer2D>();
                        poly.SetVerts(verts, static_cast<u32>(parsed));
                        poly.SetColor(ln.color);
                        const u32 pc = (static_cast<u32>(parsed) < kMaxPolyVerts) ? static_cast<u32>(parsed) : kMaxPolyVerts;
                        for (u32 k = 0; k < pc; ++k) ln.poly[k] = verts[k];   // 物理 polygon 用に保持
                        ln.polyCount = pc;
                    }
                }
            }
            continue;
        }
        if (out_sprites != nullptr && IsDirective(line, "SPRT")) {   // SPRT <id> <path...>
            int nid = 0, consumed = 0;
            if (std::sscanf(line, "SPRT %d %n", &nid, &consumed) >= 1) {
                const int idx = FindLoadedIndex(nodes, nid);
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                if (idx >= 0 && *path != '\0') {
                    FSpriteRequest req;
                    req.node = nodes[static_cast<u32>(idx)].node;
                    req.size = FVec2{ nodes[static_cast<u32>(idx)].base, nodes[static_cast<u32>(idx)].base };
                    int k = 0; for (; path[k] != '\0' && k < 259; ++k) req.path[k] = path[k];
                    req.path[k] = '\0';
                    out_sprites->Add(req);
                }
            }
            continue;
        }
        if (IsDirective(line, "MAT")) {                      // MAT <id> <path...> (使用マテリアル)
            int nid = 0, consumed = 0;
            if (std::sscanf(line, "MAT %d %n", &nid, &consumed) >= 1) {
                const int idx = FindLoadedIndex(nodes, nid);
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                if (idx >= 0 && *path != '\0' && nodes[static_cast<u32>(idx)].node != nullptr) {
                    FMaterial2D mat;
                    bool material_loaded = false;
                    if (pack != nullptr) {
                        TArray<byte> material_bytes;
                        material_loaded =
                            ReadAssetPackEntry(*pack, path, material_bytes, false) &&
                            material_bytes.Num() <= kMaterial2DMaxTextBytes &&
                            TryParseAcsmatText(
                                reinterpret_cast<const char*>(material_bytes.GetData()),
                                material_bytes.Num(), mat).Succeeded();
                    } else {
                        material_loaded = LoadAcsmatFile(path, mat);
                    }
                    if (pack != nullptr && !material_loaded &&
                        pack_asset_failure != nullptr) {
                        *pack_asset_failure = true;
                    }
                    if (material_loaded) {
                        ANode* mn = nodes[static_cast<u32>(idx)].node;
                        mn->SetMaterial(mat);                          // マテリアルを node へ
                        // PBR の法線マップは後段で GPU 化するため要求に積む。
                        if (out_mat_tex != nullptr && mat.kind == EMaterialKind::Lit
                            && mat.pbr.normalPath[0] != '\0') {
                            FMaterialTexRequest req;
                            req.node = mn;
                            int k = 0; for (; mat.pbr.normalPath[k] != '\0' && k < 259; ++k)
                                req.normalPath[k] = mat.pbr.normalPath[k];
                            req.normalPath[k] = '\0';
                            out_mat_tex->Add(req);
                        }
                    }
                }
            }
            continue;
        }
        if (IsDirective(line, "NFLG")) {                     // NFLG <id> <visible> <enabled> <sortLayer>
            int nid = 0, vis = 1, ena = 1, layer = 0;
            if (std::sscanf(line, "NFLG %d %d %d %d", &nid, &vis, &ena, &layer) >= 2) {
                const int idx = FindLoadedIndex(nodes, nid);
                if (idx >= 0) {
                    FLoadedNode& ln = nodes[static_cast<u32>(idx)];
                    ln.visible   = (vis != 0);
                    ln.enabled   = (ena != 0);
                    ln.sortLayer = layer;
                    ln.hasNflg   = true;
                }
            }
            continue;
        }
        if (IsDirective(line, "RPLY")) {                    // RPLY <id> <count> <x0 y0 ...> (描画用滑らか頂点)
            int nid = 0, cnt = 0, consumed = 0;
            if (std::sscanf(line, "RPLY %d %d %n", &nid, &cnt, &consumed) >= 2 && cnt >= 3) {
                const int idx = FindLoadedIndex(nodes, nid);
                if (idx >= 0) {
                    FLoadedNode& ln = nodes[static_cast<u32>(idx)];
                    if (cnt > static_cast<int>(APolygonRenderer2D::kMaxVerts)) cnt = APolygonRenderer2D::kMaxVerts;
                    const char* q = line + consumed;
                    int parsed = 0;
                    for (; parsed < cnt; ++parsed) {
                        int c2 = 0;
                        if (std::sscanf(q, "%f %f %n", &ln.renderVerts[parsed].x, &ln.renderVerts[parsed].y, &c2) < 2) break;
                        q += c2;
                    }
                    ln.renderCount = static_cast<u32>(parsed);
                }
            }
            continue;
        }
        // SEL はスキップ (スタンドアロンに選択概念は無い)。
    }

    // --- 親付け fixup (順序非依存。Reparent は Local 保持) ---
    bool reparented = false;
    for (u32 i = 0; i < nodes.Num(); ++i) {
        if (nodes[i].parent < 0) continue;
        const int pj = FindLoadedIndex(nodes, nodes[i].parent);
        if (pj >= 0 && nodes[i].node->Parent() != nodes[static_cast<u32>(pj)].node) {
            nodes[i].node->Reparent(*nodes[static_cast<u32>(pj)].node);
            reparented = true;
        }
    }
    if (reparented) root.ResolveStructuralChanges();

    // プレハブ生成用: サブツリーのルート (= 親が無い最初のノード) を返す。
    if (out_root != nullptr) {
        for (u32 i = 0; i < nodes.Num(); ++i) {
            if (nodes[i].parent < 0) { *out_root = nodes[i].node; break; }
        }
    }

    // --- NFLG (ノードフラグ) を適用 ---
    for (u32 i = 0; i < nodes.Num(); ++i) {
        if (!nodes[i].hasNflg) continue;
        ANode* n = nodes[i].node;
        n->SetVisible(nodes[i].visible);
        n->SetEnabled(nodes[i].enabled);
        n->SetDrawLayer(nodes[i].sortLayer);
    }

    // --- RPLY (描画用滑らか頂点) を APolygonRenderer2D に適用 ---
    // POLY で作った APolygonRenderer2D の描画頂点を RPLY の滑らか頂点で上書きする。
    // RPLY が無ければ POLY 頂点のまま (角張った描画)。
    for (u32 i = 0; i < nodes.Num(); ++i) {
        if (nodes[i].renderCount < 3u) continue;
        ANode* n = nodes[i].node;
        for (u32 c = 0; c < n->ComponentCount(); ++c) {
            AComponent* comp = n->ComponentAt(c);
            if (comp != nullptr && comp->ReflectName() != nullptr
                && std::strcmp(comp->ReflectName(), "APolygonRenderer2D") == 0) {
                static_cast<APolygonRenderer2D*>(comp)->SetVerts(nodes[i].renderVerts, nodes[i].renderCount);
                break;
            }
        }
    }

    // --- APrimitiveRenderer2D の見た目をノードの color/base で設定 (editor の描画と一致) ---
    // shape が Box/Circle/Triangle (0-2) なら base サイズで描画。Polygon (>=3) は APolygonRenderer2D
    // が実形状を描くので、ここの箱は size 0 で隠す (二重描画/巨大な箱を防ぐ)。
    for (u32 i = 0; i < nodes.Num(); ++i) {
        ANode* n = nodes[i].node;
        for (u32 c = 0; c < n->ComponentCount(); ++c) {
            if (APrimitiveRenderer2D* pr = AsPrimitive(n->ComponentAt(c))) {
                pr->SetColor(nodes[i].color);
                const bool is_poly = static_cast<int>(pr->Shape()) > 2;
                pr->SetSize(is_poly ? FVec2{ 0.0f, 0.0f } : FVec2{ nodes[i].base, nodes[i].base });
            }
        }
    }

    // --- 剛体 (ARigidBody2D) request を組み立てる (shape は APrimitiveRenderer2D から) ---
    if (out_bodies != nullptr) {
        for (u32 i = 0; i < nodes.Num(); ++i) {
            if (nodes[i].rigid_slot < 0) continue;
            FRigidBodyRequest req;
            req.node        = nodes[i].node;
            req.bodyType    = static_cast<int>(nodes[i].rb[0]);
            req.restitution = nodes[i].rb[1];
            req.friction    = nodes[i].rb[2];
            req.mass        = nodes[i].rb[3];
            req.linDamp     = nodes[i].rb[4];
            req.angDamp     = nodes[i].rb[5];
            req.base        = nodes[i].base;
            req.shape       = 0;   // 既定 box
            for (u32 c = 0; c < nodes[i].node->ComponentCount(); ++c)
                if (APrimitiveRenderer2D* pr = AsPrimitive(nodes[i].node->ComponentAt(c)))
                    req.shape = static_cast<int>(pr->Shape());
            req.polyCount = nodes[i].polyCount;
            for (u32 k = 0; k < nodes[i].polyCount; ++k) req.poly[k] = nodes[i].poly[k];
            out_bodies->Add(req);
        }
    }

    // --- world 境界 (カメラフレーミング用) ---
    for (u32 i = 0; i < nodes.Num(); ++i) {
        const FVec2 wp = nodes[i].node->World2D().position;
        const f32 half = nodes[i].base * 0.5f;
        const FVec2 lo{ wp.x - half, wp.y - half }, hi{ wp.x + half, wp.y + half };
        if (!bounds.valid) { bounds.min = lo; bounds.max = hi; bounds.valid = true; }
        else {
            if (lo.x < bounds.min.x) bounds.min.x = lo.x;
            if (lo.y < bounds.min.y) bounds.min.y = lo.y;
            if (hi.x > bounds.max.x) bounds.max.x = hi.x;
            if (hi.y > bounds.max.y) bounds.max.y = hi.y;
        }
    }
    return bounds;
}

const char* SceneTextLoadErrorName(ESceneTextLoadError error) noexcept {
    switch (error) {
    case ESceneTextLoadError::None: return "None";
    case ESceneTextLoadError::NullText: return "NullText";
    case ESceneTextLoadError::NullPath: return "NullPath";
    case ESceneTextLoadError::TextTooLarge: return "TextTooLarge";
    case ESceneTextLoadError::LineTooLong: return "LineTooLong";
    case ESceneTextLoadError::InvalidHeader: return "InvalidHeader";
    case ESceneTextLoadError::MissingNodeCount: return "MissingNodeCount";
    case ESceneTextLoadError::NodeLimitExceeded: return "NodeLimitExceeded";
    case ESceneTextLoadError::TruncatedNodeList: return "TruncatedNodeList";
    case ESceneTextLoadError::InvalidNodeRecord: return "InvalidNodeRecord";
    case ESceneTextLoadError::DuplicateNodeId: return "DuplicateNodeId";
    case ESceneTextLoadError::InvalidNodeReference: return "InvalidNodeReference";
    case ESceneTextLoadError::HierarchyCycle: return "HierarchyCycle";
    case ESceneTextLoadError::TreeDepthLimitExceeded: return "TreeDepthLimitExceeded";
    case ESceneTextLoadError::DirectiveLimitExceeded: return "DirectiveLimitExceeded";
    case ESceneTextLoadError::ComponentLimitExceeded: return "ComponentLimitExceeded";
    case ESceneTextLoadError::InvalidDirective: return "InvalidDirective";
    case ESceneTextLoadError::PathTooLong: return "PathTooLong";
    case ESceneTextLoadError::TargetPendingDestroy: return "TargetPendingDestroy";
    case ESceneTextLoadError::AllocationFailure: return "AllocationFailure";
    case ESceneTextLoadError::CommitFailed: return "CommitFailed";
    case ESceneTextLoadError::FileOpenFailed: return "FileOpenFailed";
    case ESceneTextLoadError::FileSeekFailed: return "FileSeekFailed";
    case ESceneTextLoadError::FileSizeLimitExceeded: return "FileSizeLimitExceeded";
    case ESceneTextLoadError::FileReadFailed: return "FileReadFailed";
    case ESceneTextLoadError::EmbeddedNul: return "EmbeddedNul";
    }
    return "Unknown";
}

static FSceneTextLoadResult TryLoadAcsceneTextImpl(
    const char* text, ANode& root,
    TArray<FSpriteRequest>* out_sprites,
    TArray<FRigidBodyRequest>* out_bodies,
    TArray<FMaterialTexRequest>* out_mat_tex,
    ANode** out_root,
    IAssetPackReader* pack) noexcept {
    FSceneTextPreflight checked = PreflightAcsceneText(text, root);
    if (!checked.result.Succeeded()) return checked.result;

    // 隔離した owner 配下へ構築する。検証失敗時は部分 tree を attach せず、
    // request-array entry も呼び出し側へ公開しない。
    ANode staging;
    staging.Local() = root.World();
    staging.ManagementAccess().SetSceneServices(root.SceneServices());
    staging.ManagementAccess().SetSubsystems(root.Subsystems());
    TArray<FSpriteRequest> sprites;
    TArray<FRigidBodyRequest> bodies;
    TArray<FMaterialTexRequest> materials;
    ANode* loaded_root = nullptr;
    bool pack_asset_failure = false;
    checked.result.Bounds = LoadAcsceneTextValidated(
        text, staging,
        out_sprites != nullptr ? &sprites : nullptr,
        out_bodies != nullptr ? &bodies : nullptr,
        out_mat_tex != nullptr ? &materials : nullptr,
        &loaded_root, pack, &pack_asset_failure);
    if (pack_asset_failure) {
        checked.result.Error = ESceneTextLoadError::FileReadFailed;
        checked.result.Bounds = {};
        return checked.result;
    }

    // 構造 commit 前に完全な置換 array を構築する。allocation が 1 件でも失敗した場合、
    // capacity と element address を含む呼び出し側 array を変更しない。
    const bool publish_sprites = out_sprites != nullptr && !sprites.IsEmpty();
    const bool publish_bodies = out_bodies != nullptr && !bodies.IsEmpty();
    const bool publish_materials = out_mat_tex != nullptr && !materials.IsEmpty();
    TArray<FSpriteRequest> prepared_sprites(ArrayAllocatorOrDefault(out_sprites));
    TArray<FRigidBodyRequest> prepared_bodies(ArrayAllocatorOrDefault(out_bodies));
    TArray<FMaterialTexRequest> prepared_materials(ArrayAllocatorOrDefault(out_mat_tex));
    if ((publish_sprites && !TryPrepareAppend(*out_sprites, sprites, prepared_sprites)) ||
        (publish_bodies && !TryPrepareAppend(*out_bodies, bodies, prepared_bodies)) ||
        (publish_materials &&
         !TryPrepareAppend(*out_mat_tex, materials, prepared_materials))) {
        checked.result.Error = ESceneTextLoadError::AllocationFailure;
        checked.result.Bounds = {};
        return checked.result;
    }

    const u32 root_children_before = root.ChildCount();
    const u32 top_level_count = staging.ChildCount();
    for (u32 i = 0; i < top_level_count; ++i) {
        ANode* node = staging.Child(i);
        if (node == nullptr) {
            checked.result.Error = ESceneTextLoadError::CommitFailed;
            return checked.result;
        }
        node->Reparent(root);
        if (!node->IsPendingReparent()) {
            checked.result.Error = ESceneTextLoadError::CommitFailed;
            return checked.result;
        }
    }
    staging.ResolveStructuralChanges();
    if (staging.ChildCount() != 0u ||
        root.ChildCount() != root_children_before + top_level_count) {
        checked.result.Error = ESceneTextLoadError::CommitFailed;
        return checked.result;
    }

    if (publish_sprites) *out_sprites = Move(prepared_sprites);
    if (publish_bodies) *out_bodies = Move(prepared_bodies);
    if (publish_materials) *out_mat_tex = Move(prepared_materials);
    if (out_root != nullptr) *out_root = loaded_root;
    return checked.result;
}

FSceneTextLoadResult TryLoadAcsceneText(const char* text, ANode& root,
                                        TArray<FSpriteRequest>* out_sprites,
                                        TArray<FRigidBodyRequest>* out_bodies,
                                        TArray<FMaterialTexRequest>* out_mat_tex,
                                        ANode** out_root) noexcept {
    return TryLoadAcsceneTextImpl(
        text, root, out_sprites, out_bodies, out_mat_tex, out_root, nullptr);
}

FSceneBounds LoadAcsceneText(const char* text, ANode& root,
                             TArray<FSpriteRequest>* out_sprites,
                             TArray<FRigidBodyRequest>* out_bodies,
                             TArray<FMaterialTexRequest>* out_mat_tex,
                             ANode** out_root) noexcept {
    if (out_root != nullptr) *out_root = nullptr;
    const FSceneTextLoadResult result =
        TryLoadAcsceneText(text, root, out_sprites, out_bodies, out_mat_tex, out_root);
    return result.Bounds;
}

FSceneTextLoadResult TryLoadAcsceneFile(const char* path, ANode& root,
                                        TArray<FSpriteRequest>* out_sprites,
                                        TArray<FRigidBodyRequest>* out_bodies,
                                        TArray<FMaterialTexRequest>* out_mat_tex,
                                        ANode** out_root) noexcept {
    FSceneTextLoadResult result;
    if (path == nullptr) {
        result.Error = ESceneTextLoadError::NullPath;
        return result;
    }
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        result.Error = ESceneTextLoadError::FileOpenFailed;
        return result;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        result.Error = ESceneTextLoadError::FileSeekFailed;
        return result;
    }
    const long size = std::ftell(file);
    if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        result.Error = ESceneTextLoadError::FileSeekFailed;
        return result;
    }
    if (static_cast<unsigned long>(size) > static_cast<unsigned long>(kSceneTextMaxBytes)) {
        std::fclose(file);
        result.Error = ESceneTextLoadError::FileSizeLimitExceeded;
        return result;
    }

    TArray<char> buffer;
    buffer.SetNum(static_cast<u32>(size) + 1u);
    const usize read = std::fread(buffer.GetData(), 1, static_cast<usize>(size), file);
    const bool read_failed = read != static_cast<usize>(size) || std::ferror(file) != 0;
    std::fclose(file);
    if (read_failed) {
        result.Error = ESceneTextLoadError::FileReadFailed;
        return result;
    }
    if (std::memchr(buffer.GetData(), '\0', read) != nullptr) {
        result.Error = ESceneTextLoadError::EmbeddedNul;
        return result;
    }
    buffer[static_cast<u32>(read)] = '\0';
    return TryLoadAcsceneText(
        buffer.GetData(), root, out_sprites, out_bodies, out_mat_tex, out_root);
}

FSceneBounds LoadAcsceneFile(const char* path, ANode& root,
                             TArray<FSpriteRequest>* out_sprites,
                             TArray<FRigidBodyRequest>* out_bodies,
                             TArray<FMaterialTexRequest>* out_mat_tex) noexcept {
    return TryLoadAcsceneFile(path, root, out_sprites, out_bodies, out_mat_tex).Bounds;
}

FSceneTextLoadResult TryLoadAcsceneAssetPack(
    IAssetPackReader& pack, const char* virtual_path, ANode& root,
    TArray<FSpriteRequest>* out_sprites,
    TArray<FRigidBodyRequest>* out_bodies,
    TArray<FMaterialTexRequest>* out_mat_tex,
    ANode** out_root) noexcept {
    FSceneTextLoadResult result;
    if (virtual_path == nullptr) {
        result.Error = ESceneTextLoadError::NullPath;
        return result;
    }

    const auto size_result = pack.FileSize(virtual_path);
    if (size_result.IsErr()) {
        result.Error = ESceneTextLoadError::FileOpenFailed;
        return result;
    }
    if (size_result.Value() > static_cast<u64>(kSceneTextMaxBytes)) {
        result.Error = ESceneTextLoadError::FileSizeLimitExceeded;
        return result;
    }

    TArray<byte> bytes;
    if (!ReadAssetPackEntry(pack, virtual_path, bytes, true)) {
        result.Error = ESceneTextLoadError::FileReadFailed;
        return result;
    }
    const usize text_size = bytes.Num() - 1u;
    if (std::memchr(bytes.GetData(), '\0', text_size) != nullptr) {
        result.Error = ESceneTextLoadError::EmbeddedNul;
        return result;
    }
    return TryLoadAcsceneTextImpl(
        reinterpret_cast<const char*>(bytes.GetData()), root,
        out_sprites, out_bodies, out_mat_tex, out_root, &pack);
}

FSceneBounds LoadAcsceneAssetPack(
    IAssetPackReader& pack, const char* virtual_path, ANode& root,
    TArray<FSpriteRequest>* out_sprites,
    TArray<FRigidBodyRequest>* out_bodies,
    TArray<FMaterialTexRequest>* out_mat_tex) noexcept {
    return TryLoadAcsceneAssetPack(
        pack, virtual_path, root, out_sprites, out_bodies, out_mat_tex).Bounds;
}

ANode* SpawnPrefabText(const char* text, ANode& parent) noexcept {
    ANode* spawned = nullptr;
    const FSceneTextLoadResult result =
        TryLoadAcsceneText(text, parent, nullptr, nullptr, nullptr, &spawned);
    return result.Succeeded() ? spawned : nullptr;
}

ANode* SpawnPrefabFile(const char* path, ANode& parent) noexcept {
    ANode* spawned = nullptr;
    const FSceneTextLoadResult result =
        TryLoadAcsceneFile(path, parent, nullptr, nullptr, nullptr, &spawned);
    return result.Succeeded() ? spawned : nullptr;
}

void BuildSceneRigidBodies(CRigidWorld2D& world, const TArray<FRigidBodyRequest>& reqs,
                           TArray<ANode*>& out_nodes, TArray<u32>& out_bodies) noexcept {
    for (u32 i = 0; i < reqs.Num(); ++i) {
        const FRigidBodyRequest& req = reqs[i];
        if (req.node == nullptr) continue;
        const FTransform2D w = req.node->World2D();
        const f32 sx = (w.scale.x != 0.0f) ? w.scale.x : 1.0f;
        const f32 sy = (w.scale.y != 0.0f) ? w.scale.y : 1.0f;
        const FVec2 pos{ w.position.x, w.position.y };
        const bool dynamic = (req.bodyType == 1);
        const f32 mass = (req.mass > 0.001f) ? req.mass : 1.0f;

        u32 bi;
        if (req.shape == 3 && req.polyCount >= 3u) {                    // ポリゴン
            FVec2 lv[kMaxPolyVerts];
            for (u32 k = 0; k < req.polyCount; ++k) lv[k] = FVec2{ req.poly[k].x * sx, req.poly[k].y * sy };
            bi = world.AddPolygon(pos, lv, req.polyCount, dynamic ? mass : 0.0f, req.restitution, req.friction);
        } else if (req.shape == 1) {                                   // 円
            const f32 radius = req.base * 0.5f * ((sx > sy) ? sx : sy);
            bi = world.AddCircle(pos, radius, dynamic ? mass : 0.0f, req.restitution, req.friction);
        } else {                                                       // 箱 (OBB)
            const FVec2 half{ req.base * 0.5f * sx, req.base * 0.5f * sy };
            bi = dynamic ? world.AddDynamicAabb(pos, half, mass, req.restitution, req.friction)
                         : world.AddStaticAabb(pos, half, req.restitution, req.friction);
        }
        world.SetAngle(bi, w.rotation);
        if (dynamic) {
            world.SetDamping(bi, req.linDamp, req.angDamp);
            out_nodes.Add(req.node);
            out_bodies.Add(bi);
        }
    }
}

void StepSceneRigidBodies(CRigidWorld2D& world, const TArray<ANode*>& nodes,
                          const TArray<u32>& bodies, f32 dt, FVec2 gravity) noexcept {
    if (dt > 0.05f) dt = 0.05f;
    world.Step(dt, gravity);
    const u32 n = (nodes.Num() < bodies.Num()) ? nodes.Num() : bodies.Num();
    for (u32 i = 0; i < n; ++i) {
        if (nodes[i] == nullptr) continue;
        nodes[i]->SetPosition2D(world.Position(bodies[i]));
        nodes[i]->SetRotation2D(world.Angle(bodies[i]));
    }
}

void LoadSceneSprites(IRhiDevice& device, const TArray<FSpriteRequest>& reqs,
                      TArray<TUniquePtr<IRhiTexture>>& out_textures) noexcept {
    for (u32 i = 0; i < reqs.Num(); ++i) {
        const FSpriteRequest& req = reqs[i];
        if (req.node == nullptr || req.path[0] == '\0') continue;

        std::FILE* f = std::fopen(req.path, "rb");   // 非 ASCII パスは未対応
        if (f == nullptr) continue;
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz <= 0) { std::fclose(f); continue; }
        TArray<byte> bytes;
        bytes.SetNum(static_cast<u32>(sz));
        const usize rd = std::fread(bytes.GetData(), 1, static_cast<usize>(sz), f);
        std::fclose(f);
        if (rd != static_cast<usize>(sz)) continue;

        CImageAssetLoader loader;
        auto decoded = loader.LoadFromBytes(kInvalidAssetId, bytes);
        if (decoded.IsErr()) continue;
        auto asset = decoded.Value();                 // TSharedPtr を保持
        const AImageAsset* img = static_cast<const AImageAsset*>(asset.Get());
        if (img == nullptr) continue;
        auto tex = UploadTexture(device, *img);
        if (tex.IsErr()) continue;

        ASprite2DComponent& sp = req.node->AddComponent<ASprite2DComponent>(req.size);
        sp.SetTexture(tex.Value().Get());              // raw ptr を bind (所有は out_textures)
        out_textures.Add(Move(tex.Value()));
    }
}

void LoadSceneSpritesFromAssetPack(
    IRhiDevice& device, const TArray<FSpriteRequest>& reqs,
    IAssetPackReader& pack,
    TArray<TUniquePtr<IRhiTexture>>& out_textures) noexcept {
    for (u32 i = 0; i < reqs.Num(); ++i) {
        const FSpriteRequest& req = reqs[i];
        if (req.node == nullptr || req.path[0] == '\0') continue;

        TArray<byte> bytes;
        if (!ReadAssetPackEntry(pack, req.path, bytes, false) ||
            bytes.IsEmpty()) {
            continue;
        }

        CImageAssetLoader loader;
        auto decoded = loader.LoadFromBytes(kInvalidAssetId, bytes);
        if (decoded.IsErr()) continue;
        auto asset = decoded.Value();
        const AImageAsset* img =
            static_cast<const AImageAsset*>(asset.Get());
        if (img == nullptr) continue;
        auto tex = UploadTexture(device, *img);
        if (tex.IsErr()) continue;

        ASprite2DComponent& sp =
            req.node->AddComponent<ASprite2DComponent>(req.size);
        sp.SetTexture(tex.Value().Get());
        out_textures.Add(Move(tex.Value()));
    }
}

void LoadSceneMaterialTextures(IRhiDevice& device, const TArray<FMaterialTexRequest>& reqs,
                               TArray<TUniquePtr<IRhiTexture>>& out_textures) noexcept {
    for (u32 i = 0; i < reqs.Num(); ++i) {
        const FMaterialTexRequest& req = reqs[i];
        if (req.node == nullptr || req.normalPath[0] == '\0') continue;

        std::FILE* f = std::fopen(req.normalPath, "rb");   // 非 ASCII パスは未対応
        if (f == nullptr) continue;
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz <= 0) { std::fclose(f); continue; }
        TArray<byte> bytes;
        bytes.SetNum(static_cast<u32>(sz));
        const usize rd = std::fread(bytes.GetData(), 1, static_cast<usize>(sz), f);
        std::fclose(f);
        if (rd != static_cast<usize>(sz)) continue;

        CImageAssetLoader loader;
        auto decoded = loader.LoadFromBytes(kInvalidAssetId, bytes);
        if (decoded.IsErr()) continue;
        auto asset = decoded.Value();
        const AImageAsset* img = static_cast<const AImageAsset*>(asset.Get());
        if (img == nullptr) continue;
        auto tex = UploadTexture(device, *img);
        if (tex.IsErr()) continue;

        req.node->SetMaterialNormalTex(tex.Value().Get());   // 非所有 ptr を bind (所有は out_textures)
        out_textures.Add(Move(tex.Value()));
    }
}

void LoadSceneMaterialTexturesFromAssetPack(
    IRhiDevice& device, const TArray<FMaterialTexRequest>& reqs,
    IAssetPackReader& pack,
    TArray<TUniquePtr<IRhiTexture>>& out_textures) noexcept {
    for (u32 i = 0; i < reqs.Num(); ++i) {
        const FMaterialTexRequest& req = reqs[i];
        if (req.node == nullptr || req.normalPath[0] == '\0') continue;

        TArray<byte> bytes;
        if (!ReadAssetPackEntry(pack, req.normalPath, bytes, false) ||
            bytes.IsEmpty()) {
            continue;
        }

        CImageAssetLoader loader;
        auto decoded = loader.LoadFromBytes(kInvalidAssetId, bytes);
        if (decoded.IsErr()) continue;
        auto asset = decoded.Value();
        const AImageAsset* img =
            static_cast<const AImageAsset*>(asset.Get());
        if (img == nullptr) continue;
        auto tex = UploadTexture(device, *img);
        if (tex.IsErr()) continue;

        req.node->SetMaterialNormalTex(tex.Value().Get());
        out_textures.Add(Move(tex.Value()));
    }
}

} // namespace acs::game
