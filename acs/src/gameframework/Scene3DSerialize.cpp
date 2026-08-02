// SPDX-License-Identifier: Apache-2.0
// GameFramework — 3D シーンのテキストシリアライズ 実装
#include "gameframework/Scene3DSerialize.h"
#include "gameframework/SceneNodeGraph.h"
#include "gameframework/ANode.h"
#include "gameframework/AssetPack.h"
#include "gameframework/CameraComponent3D.h"
#include "gameframework/ComponentFactory.h"
#include "gameframework/Material2D.h"
#include "gameframework/MeshComponent3D.h"
#include "gameframework/Reflect.h"
#include "gameframework/ReflectApply.h"
#include "asset/MeshAsset.h"
#include "container/Array.h"
#include "container/Hash.h"
#include "container/HashMap.h"
#include "container/StringView.h"
#include "foundation/Move.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace acs::game {

namespace {

static_assert(kScene3DSerializeMaxTreeDepth == kNodeMaxTreeDepth,
              "Scene3D text I/O and ANode must enforce the same depth limit");

constexpr u32 kNodeLineCapacity = 768u;

struct FFlattenEntry {
    const ANode* Node = nullptr;
    i32 ParentId = -1;
    u32 Depth = 0u;
    bool Exit = false;
};

struct FParsedNode {
    i32 SourceId = -1;
    i32 ParentId = -1;
    i32 ParentIndex = -1;
    i32 Primitive = -1;
    FVec3 Position{0.0f, 0.0f, 0.0f};
    FVec3 RotationDeg{0.0f, 0.0f, 0.0f};
    FVec3 Scale{1.0f, 1.0f, 1.0f};
    FVec4 Color{1.0f, 1.0f, 1.0f, 1.0f};
    u32 Depth = 0u;
    bool HasMeshPath = false;
    bool HasMaterialPath = false;
    bool HasLegacyMaterial = false;
    bool Visible = true;
    bool Enabled = true;
    bool Empty = false;
    u32 ComponentCount = 0u;
    f32 Metallic = 0.0f;
    f32 Roughness = 0.5f;
    char Name[kScene3DSerializeMaxNameBytes + 1u]{};
    char MeshPath[kScene3DSerializeMaxMeshPathBytes + 1u]{};
    char MaterialPath[kScene3DSerializeMaxMaterialPathBytes + 1u]{};
    TSharedPtr<AAsset> LoadedMesh;
    FMaterial2D LoadedMaterial{};
};

struct FParsedComponent {
    u32 NodeIndex = 0u;
    u32 Slot = 0u;
    char TypeName[128]{};
    TUniquePtr<AComponent> Instance;
};

struct FParsedComponentProperty {
    u32 NodeIndex = 0u;
    u32 Slot = 0u;
    u32 Property = 0u;
    f32 Value[4]{};
};

struct FParsedCamera {
    u32 NodeIndex = 0u;
    i32 NodeId = -1;
    i32 Priority = 0;
    EScene3DCameraProjection Projection =
        EScene3DCameraProjection::Perspective;
    bool ActivePreferred = false;
    f32 FovYDegrees = 60.0f;
    f32 OrthographicHeight = 10.0f;
    f32 NearPlane = 0.05f;
    f32 FarPlane = 1000.0f;
    char StableId[kScene3DSerializeMaxCameraIdBytes + 1u]{};
};

constexpr i32 kScene3DCameraMinPriority = -1000000;
constexpr i32 kScene3DCameraMaxPriority = 1000000;
constexpr f32 kScene3DCameraMinFovDegrees = 1.0f;
constexpr f32 kScene3DCameraMaxFovDegrees = 179.0f;
constexpr f32 kScene3DCameraMinOrthoHeight = 0.001f;
constexpr f32 kScene3DCameraMaxOrthoHeight = 1000000.0f;
constexpr f32 kScene3DCameraMinNearPlane = 0.0001f;
constexpr f32 kScene3DCameraMaxNearPlane = 1000000.0f;
constexpr f32 kScene3DCameraMaxFarPlane = 1000000000.0f;

/** const ノードから最初の AMeshComponent3D を探す。 */
const AMeshComponent3D* FindMesh(const ANode& node) noexcept {
    const void* kind = ComponentKindOf<AMeshComponent3D>();
    for (u32 i = 0; i < node.ComponentCount(); ++i) {
        const AComponent* component = node.ComponentAt(i);
        if (component != nullptr && component->Kind() == kind)
            return static_cast<const AMeshComponent3D*>(component);
    }
    return nullptr;
}

const ACameraComponent3D* FindCamera(const ANode& node) noexcept {
    const void* kind = ComponentKindOf<ACameraComponent3D>();
    for (u32 i = 0; i < node.ComponentCount(); ++i) {
        const AComponent* component = node.ComponentAt(i);
        if (component != nullptr && component->Kind() == kind)
            return static_cast<const ACameraComponent3D*>(component);
    }
    return nullptr;
}

bool HasMultipleCameras(const ANode& node) noexcept {
    const void* kind = ComponentKindOf<ACameraComponent3D>();
    bool found = false;
    for (u32 i = 0u; i < node.ComponentCount(); ++i) {
        const AComponent* component = node.ComponentAt(i);
        if (component == nullptr || component->Kind() != kind) continue;
        if (found) return true;
        found = true;
    }
    return false;
}

bool IsFinite(FVec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool IsFinite(FVec4 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z) && std::isfinite(value.w);
}

bool IsValidPrimitive(i32 primitive) noexcept {
    return primitive == -1
        || (primitive >= static_cast<i32>(EMeshPrimitive3D::Cube)
            && primitive <= static_cast<i32>(EMeshPrimitive3D::Mesh));
}

bool CheckedAdd(u32& value, u32 increment) noexcept {
    constexpr u32 kU32Max = ~u32{0};
    if (increment > kU32Max - value) return false;
    value += increment;
    return true;
}

bool IsSafeTextByte(char ch) noexcept {
    const u8 byte = static_cast<u8>(ch);
    return byte >= 0x20u && byte != 0x7Fu;
}

EScene3DSerializeError CopyField(FStringView source, char* destination, u32 max_bytes,
                                 const char* fallback,
                                 EScene3DSerializeError invalid_error) noexcept {
    FStringView value = source;
    u32 fallback_size = 0u;
    if (value.IsEmpty()) {
        while (fallback[fallback_size] != '\0') ++fallback_size;
        value = FStringView(fallback, fallback_size);
    }
    if (value.Size() > max_bytes) return invalid_error;
    for (u32 i = 0u; i < value.Size(); ++i) {
        if (!IsSafeTextByte(value[i])) return invalid_error;
        destination[i] = value[i];
    }
    destination[value.Size()] = '\0';
    return EScene3DSerializeError::None;
}

EScene3DSerializeError Flatten(
    const ANode& root, TArray<const ANode*>& nodes, TArray<i32>& parents) noexcept {
    TArray<FFlattenEntry> stack;
    THashMap<const ANode*, u8> states;
    if (!stack.TryPushBack(FFlattenEntry{&root, -1, 0u, false}))
        return EScene3DSerializeError::AllocationFailure;

    while (!stack.IsEmpty()) {
        const FFlattenEntry entry = stack.Back();
        stack.PopBack();
        if (entry.Node == nullptr) return EScene3DSerializeError::InvalidParent;

        if (entry.Exit) {
            u8* state = states.Find(entry.Node);
            if (state == nullptr || *state != 1u)
                return EScene3DSerializeError::InvalidParent;
            *state = 2u;
            continue;
        }

        if (const u8* state = states.Find(entry.Node)) {
            return *state == 1u ? EScene3DSerializeError::CyclicNodeGraph
                                : EScene3DSerializeError::DuplicateNodeReference;
        }
        if (entry.Depth > kScene3DSerializeMaxTreeDepth)
            return EScene3DSerializeError::TreeDepthLimitExceeded;
        if (nodes.Size() >= kScene3DSerializeMaxNodeCount)
            return EScene3DSerializeError::NodeLimitExceeded;
        if (!states.TryInsert(entry.Node, 1u)
            || !nodes.TryPushBack(entry.Node)
            || !parents.TryPushBack(entry.ParentId)) {
            return EScene3DSerializeError::AllocationFailure;
        }

        const i32 my_id = static_cast<i32>(nodes.Size() - 1u);
        if (!stack.TryPushBack(
                FFlattenEntry{entry.Node, entry.ParentId, entry.Depth, true}))
            return EScene3DSerializeError::AllocationFailure;
        const u32 child_count = entry.Node->ChildCount();
        if (child_count > kScene3DSerializeMaxNodeCount - static_cast<u32>(nodes.Size()))
            return EScene3DSerializeError::NodeLimitExceeded;
        for (u32 i = child_count; i > 0u; --i) {
            const ANode* child = entry.Node->Child(i - 1u);
            if (child == nullptr) return EScene3DSerializeError::InvalidParent;
            if (!stack.TryPushBack(
                    FFlattenEntry{child, my_id, entry.Depth + 1u, false}))
                return EScene3DSerializeError::AllocationFailure;
        }
    }
    return nodes.IsEmpty() ? EScene3DSerializeError::MissingRoot
                           : EScene3DSerializeError::None;
}

EScene3DSerializeError FormatNodeLine(
    const ANode& node, i32 id, i32 parent_id, char* line, u32 capacity,
    u32& line_size) noexcept {
    const FTransform3D& transform = node.Local();
    const FVec3 rotation = transform.EulerDeg();
    const AMeshComponent3D* mesh = FindMesh(node);
    const i32 primitive = mesh != nullptr ? static_cast<i32>(mesh->Primitive()) : -1;
    const FVec4 color = mesh != nullptr ? mesh->Color() : FVec4{1, 1, 1, 1};
    if (!IsValidPrimitive(primitive)) return EScene3DSerializeError::InvalidPrimitive;
    if (!IsFinite(transform.position) || !IsFinite(rotation)
        || !IsFinite(transform.scale) || !IsFinite(color)) {
        return EScene3DSerializeError::InvalidNumber;
    }

    char name[kScene3DSerializeMaxNameBytes + 1u];
    const EScene3DSerializeError name_error =
        CopyField(node.Name(), name, kScene3DSerializeMaxNameBytes, "Node",
                  EScene3DSerializeError::InvalidName);
    if (name_error != EScene3DSerializeError::None) return name_error;

    const int written = std::snprintf(
        line, static_cast<size_t>(capacity),
        "N3D %d %d %d %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g "
        "%.6g %.6g %.6g %.6g %s\n",
        id, parent_id, primitive,
        static_cast<double>(transform.position.x),
        static_cast<double>(transform.position.y),
        static_cast<double>(transform.position.z),
        static_cast<double>(rotation.x), static_cast<double>(rotation.y),
        static_cast<double>(rotation.z),
        static_cast<double>(transform.scale.x), static_cast<double>(transform.scale.y),
        static_cast<double>(transform.scale.z),
        static_cast<double>(color.x), static_cast<double>(color.y),
        static_cast<double>(color.z), static_cast<double>(color.w), name);
    if (written <= 0 || static_cast<u32>(written) >= capacity)
        return EScene3DSerializeError::SerializedSizeOverflow;
    line_size = static_cast<u32>(written);
    return EScene3DSerializeError::None;
}

EScene3DSerializeError FormatMeshLine(
    const AMeshComponent3D& mesh, i32 id, char* line, u32 capacity,
    u32& line_size) noexcept {
    char path[kScene3DSerializeMaxMeshPathBytes + 1u];
    const EScene3DSerializeError path_error =
        CopyField(mesh.MeshPath(), path, kScene3DSerializeMaxMeshPathBytes, "",
                  EScene3DSerializeError::InvalidMeshPath);
    if (path_error != EScene3DSerializeError::None) return path_error;
    if (path[0] == '\0') return EScene3DSerializeError::InvalidMeshPath;

    const int written = std::snprintf(
        line, static_cast<size_t>(capacity), "MSH3D %d %s\n", id, path);
    if (written <= 0 || static_cast<u32>(written) >= capacity)
        return EScene3DSerializeError::SerializedSizeOverflow;
    line_size = static_cast<u32>(written);
    return EScene3DSerializeError::None;
}

EScene3DSerializeError FormatCameraLine(
    const ACameraComponent3D& camera, i32 id, char* line, u32 capacity,
    u32& line_size) noexcept {
    FScene3DCameraState checked;
    checked.IsAuthored = true;
    checked.IsActivePreferred = camera.IsActivePreferred();
    checked.Priority = camera.Priority();
    checked.Projection = camera.Projection();
    checked.FovYDegrees = camera.FovYDegrees();
    checked.OrthographicHeight = camera.OrthographicHeight();
    checked.NearPlane = camera.NearPlane();
    checked.FarPlane = camera.FarPlane();
    std::snprintf(
        checked.StableId, sizeof(checked.StableId), "%s", camera.StableId());
    ACameraComponent3D validator;
    if (!validator.TrySetAuthoredState(checked))
        return EScene3DSerializeError::InvalidCamera;

    const int written = std::snprintf(
        line, static_cast<size_t>(capacity),
        "CAM3D %d %s %d %d %d %.9g %.9g %.9g %.9g\n",
        id, camera.StableId(), static_cast<int>(camera.Projection()),
        camera.Priority(), camera.IsActivePreferred() ? 1 : 0,
        static_cast<double>(camera.FovYDegrees()),
        static_cast<double>(camera.OrthographicHeight()),
        static_cast<double>(camera.NearPlane()),
        static_cast<double>(camera.FarPlane()));
    if (written <= 0 || static_cast<u32>(written) >= capacity)
        return EScene3DSerializeError::SerializedSizeOverflow;
    line_size = static_cast<u32>(written);
    return EScene3DSerializeError::None;
}

void SkipSpaces(const char*& cursor, const char* end) noexcept {
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) ++cursor;
}

bool ParseI32(const char*& cursor, const char* end, i32& value) noexcept {
    SkipSpaces(cursor, end);
    if (cursor >= end) return false;
    errno = 0;
    char* parsed_end = nullptr;
    const long parsed = std::strtol(cursor, &parsed_end, 10);
    if (parsed_end == cursor || parsed_end > end || errno == ERANGE
        || parsed < (-2147483647L - 1L) || parsed > 2147483647L) {
        return false;
    }
    if (parsed_end < end && *parsed_end != ' ' && *parsed_end != '\t') return false;
    value = static_cast<i32>(parsed);
    cursor = parsed_end;
    return true;
}

bool ParseF32(const char*& cursor, const char* end, f32& value) noexcept {
    SkipSpaces(cursor, end);
    if (cursor >= end) return false;
    errno = 0;
    char* parsed_end = nullptr;
    const f32 parsed = std::strtof(cursor, &parsed_end);
    if (parsed_end == cursor || parsed_end > end || errno == ERANGE
        || !std::isfinite(parsed)) {
        return false;
    }
    if (parsed_end < end && *parsed_end != ' ' && *parsed_end != '\t') return false;
    value = parsed;
    cursor = parsed_end;
    return true;
}

EScene3DSerializeError ParseRemainder(
    const char*& cursor, const char* end, char* destination, u32 max_bytes,
    const char* fallback, EScene3DSerializeError invalid_error) noexcept {
    SkipSpaces(cursor, end);
    const u32 size = static_cast<u32>(end - cursor);
    if (size > max_bytes) return invalid_error;
    if (size == 0u) {
        u32 i = 0u;
        while (fallback[i] != '\0') {
            destination[i] = fallback[i];
            ++i;
        }
        destination[i] = '\0';
        return EScene3DSerializeError::None;
    }
    for (u32 i = 0u; i < size; ++i) {
        if (!IsSafeTextByte(cursor[i])) return invalid_error;
        destination[i] = cursor[i];
    }
    destination[size] = '\0';
    cursor = end;
    return EScene3DSerializeError::None;
}

EScene3DSerializeError ParseNodeLine(
    const char* line, u32 size, bool editor_document,
    TArray<FParsedNode>& nodes, THashMap<i32, u32>& id_to_index) noexcept {
    const char* cursor = line + 4u;
    const char* end = line + size;
    i32 id = -1;
    FParsedNode record;
    if (!ParseI32(cursor, end, id)
        || !ParseI32(cursor, end, record.ParentId)
        || !ParseI32(cursor, end, record.Primitive)) {
        return EScene3DSerializeError::InvalidInteger;
    }
    if (!ParseF32(cursor, end, record.Position.x)
        || !ParseF32(cursor, end, record.Position.y)
        || !ParseF32(cursor, end, record.Position.z)
        || !ParseF32(cursor, end, record.RotationDeg.x)
        || !ParseF32(cursor, end, record.RotationDeg.y)
        || !ParseF32(cursor, end, record.RotationDeg.z)
        || !ParseF32(cursor, end, record.Scale.x)
        || !ParseF32(cursor, end, record.Scale.y)
        || !ParseF32(cursor, end, record.Scale.z)
        || !ParseF32(cursor, end, record.Color.x)
        || !ParseF32(cursor, end, record.Color.y)
        || !ParseF32(cursor, end, record.Color.z)
        || !ParseF32(cursor, end, record.Color.w)) {
        return EScene3DSerializeError::InvalidNumber;
    }
    if (!IsValidPrimitive(record.Primitive))
        return EScene3DSerializeError::InvalidPrimitive;

    if (id < 0) return EScene3DSerializeError::InvalidNodeId;
    if (id_to_index.Contains(id))
        return EScene3DSerializeError::DuplicateNodeId;
    if (nodes.Size() >= kScene3DSerializeMaxNodeCount)
        return EScene3DSerializeError::NodeLimitExceeded;

    const u32 node_index = static_cast<u32>(nodes.Size());
    if (!editor_document && id != static_cast<i32>(node_index))
        return EScene3DSerializeError::InvalidNodeId;
    record.SourceId = id;
    if (editor_document) {
        if (record.ParentId == -1) {
            record.ParentIndex = -1;
            record.Depth = 0u;
        } else {
            const u32* parent_index = id_to_index.Find(record.ParentId);
            if (record.ParentId < 0 || parent_index == nullptr)
                return EScene3DSerializeError::InvalidParent;
            record.ParentIndex = static_cast<i32>(*parent_index);
            record.Depth = nodes[*parent_index].Depth + 1u;
        }
    } else if (node_index == 0u) {
        if (record.ParentId != -1) return EScene3DSerializeError::InvalidParent;
        record.ParentIndex = -1;
        record.Depth = 0u;
    } else {
        if (record.ParentId == -1) return EScene3DSerializeError::MultipleRoots;
        if (record.ParentId < 0
            || static_cast<u32>(record.ParentId) >= node_index)
            return EScene3DSerializeError::InvalidParent;
        record.ParentIndex = record.ParentId;
        record.Depth = nodes[static_cast<u32>(record.ParentIndex)].Depth + 1u;
    }
    if (record.Depth > kScene3DSerializeMaxTreeDepth)
        return EScene3DSerializeError::TreeDepthLimitExceeded;

    const EScene3DSerializeError name_error =
        ParseRemainder(cursor, end, record.Name, kScene3DSerializeMaxNameBytes,
                       (!editor_document && node_index == 0u) ? "Root" : "Node",
                       EScene3DSerializeError::InvalidName);
    if (name_error != EScene3DSerializeError::None) return name_error;
    if (!nodes.TryPushBack(record)) return EScene3DSerializeError::AllocationFailure;
    if (!id_to_index.TryInsert(id, node_index)) {
        nodes.PopBack();
        return EScene3DSerializeError::AllocationFailure;
    }
    return EScene3DSerializeError::None;
}

EScene3DSerializeError ParseMeshLine(
    const char* line, u32 size, TArray<FParsedNode>& nodes,
    THashMap<i32, u32>& id_to_index,
    u32& mesh_path_count) noexcept {
    const char* cursor = line + 6u;
    const char* end = line + size;
    i32 id = -1;
    if (!ParseI32(cursor, end, id)) return EScene3DSerializeError::InvalidInteger;
    const u32* node_index = id_to_index.Find(id);
    if (id < 0 || node_index == nullptr)
        return EScene3DSerializeError::InvalidNodeId;
    FParsedNode& record = nodes[*node_index];
    if (record.HasMeshPath) return EScene3DSerializeError::DuplicateMeshPath;
    if (record.Primitive != static_cast<i32>(EMeshPrimitive3D::Mesh))
        return EScene3DSerializeError::InvalidMeshPath;
    const EScene3DSerializeError path_error =
        ParseRemainder(cursor, end, record.MeshPath,
                       kScene3DSerializeMaxMeshPathBytes, "",
                       EScene3DSerializeError::InvalidMeshPath);
    if (path_error != EScene3DSerializeError::None || record.MeshPath[0] == '\0')
        return EScene3DSerializeError::InvalidMeshPath;
    record.HasMeshPath = true;
    ++mesh_path_count;
    return EScene3DSerializeError::None;
}

bool IsOnlyWhitespace(const char* cursor, const char* end) noexcept {
    SkipSpaces(cursor, end);
    return cursor == end;
}

bool IsCameraIdByte(char value, bool first) noexcept {
    const bool alpha = (value >= 'A' && value <= 'Z')
                    || (value >= 'a' && value <= 'z');
    const bool digit = value >= '0' && value <= '9';
    return alpha || digit || (!first && (value == '_' || value == '.'
                                         || value == '-'));
}

bool ParseCameraId(
    const char*& cursor, const char* end,
    char (&destination)[kScene3DSerializeMaxCameraIdBytes + 1u]) noexcept {
    SkipSpaces(cursor, end);
    const char* begin = cursor;
    while (cursor < end && *cursor != ' ' && *cursor != '\t') ++cursor;
    const u32 size = static_cast<u32>(cursor - begin);
    if (size == 0u || size > kScene3DSerializeMaxCameraIdBytes) return false;
    for (u32 index = 0u; index < size; ++index) {
        if (!IsCameraIdByte(begin[index], index == 0u)) return false;
        destination[index] = begin[index];
    }
    destination[size] = '\0';
    return true;
}

u32* FindNodeIndex(THashMap<i32, u32>& id_to_index, i32 id) noexcept {
    return id >= 0 ? id_to_index.Find(id) : nullptr;
}

EScene3DSerializeError ParseFlagsLine(
    const char* line, u32 size, TArray<FParsedNode>& nodes,
    THashMap<i32, u32>& id_to_index) noexcept {
    const char* cursor = line + 6u;
    const char* end = line + size;
    i32 id = -1, visible = -1, enabled = -1;
    if (!ParseI32(cursor, end, id)
        || !ParseI32(cursor, end, visible)
        || !ParseI32(cursor, end, enabled)
        || !IsOnlyWhitespace(cursor, end)
        || (visible != 0 && visible != 1)
        || (enabled != 0 && enabled != 1)) {
        return EScene3DSerializeError::InvalidNodeFlags;
    }
    const u32* node_index = FindNodeIndex(id_to_index, id);
    if (node_index == nullptr) return EScene3DSerializeError::InvalidNodeId;
    nodes[*node_index].Visible = visible != 0;
    nodes[*node_index].Enabled = enabled != 0;
    return EScene3DSerializeError::None;
}

EScene3DSerializeError ParseEmptyLine(
    const char* line, u32 size, TArray<FParsedNode>& nodes,
    THashMap<i32, u32>& id_to_index) noexcept {
    const char* cursor = line + 8u;
    const char* end = line + size;
    i32 id = -1;
    if (!ParseI32(cursor, end, id) || !IsOnlyWhitespace(cursor, end))
        return EScene3DSerializeError::InvalidLine;
    const u32* node_index = FindNodeIndex(id_to_index, id);
    if (node_index == nullptr) return EScene3DSerializeError::InvalidNodeId;
    nodes[*node_index].Empty = true;
    return EScene3DSerializeError::None;
}

EScene3DSerializeError ParseMaterialLine(
    const char* line, u32 size, TArray<FParsedNode>& nodes,
    THashMap<i32, u32>& id_to_index) noexcept {
    const char* cursor = line + 6u;
    const char* end = line + size;
    i32 id = -1;
    if (!ParseI32(cursor, end, id))
        return EScene3DSerializeError::InvalidInteger;
    const u32* node_index = FindNodeIndex(id_to_index, id);
    if (node_index == nullptr) return EScene3DSerializeError::InvalidNodeId;
    FParsedNode& node = nodes[*node_index];
    if (node.HasMaterialPath || node.HasLegacyMaterial)
        return EScene3DSerializeError::InvalidMaterial;

    const char* value_start = cursor;
    f32 metallic = 0.0f, roughness = 0.5f;
    if (ParseF32(cursor, end, metallic)
        && ParseF32(cursor, end, roughness)
        && IsOnlyWhitespace(cursor, end)) {
        if (metallic < 0.0f || metallic > 1.0f
            || roughness < 0.0f || roughness > 1.0f) {
            return EScene3DSerializeError::InvalidMaterial;
        }
        node.HasLegacyMaterial = true;
        node.Metallic = metallic;
        node.Roughness = roughness;
        return EScene3DSerializeError::None;
    }

    cursor = value_start;
    const EScene3DSerializeError path_error =
        ParseRemainder(cursor, end, node.MaterialPath,
                       kScene3DSerializeMaxMaterialPathBytes, "",
                       EScene3DSerializeError::InvalidMaterial);
    if (path_error != EScene3DSerializeError::None
        || node.MaterialPath[0] == '\0') {
        return EScene3DSerializeError::InvalidMaterial;
    }
    node.HasMaterialPath = true;
    return EScene3DSerializeError::None;
}

EScene3DSerializeError ParseComponentLine(
    const char* line, u32 size, TArray<FParsedNode>& nodes,
    THashMap<i32, u32>& id_to_index,
    TArray<FParsedComponent>& components) noexcept {
    const char* cursor = line + 6u;
    const char* end = line + size;
    i32 id = -1;
    if (!ParseI32(cursor, end, id))
        return EScene3DSerializeError::InvalidInteger;
    const u32* node_index = FindNodeIndex(id_to_index, id);
    if (node_index == nullptr) return EScene3DSerializeError::InvalidNodeId;
    FParsedNode& node = nodes[*node_index];
    if (node.ComponentCount >= kScene3DSerializeMaxComponentsPerNode)
        return EScene3DSerializeError::ComponentLimitExceeded;

    FParsedComponent component;
    component.NodeIndex = *node_index;
    component.Slot = node.ComponentCount++;
    const EScene3DSerializeError type_error =
        ParseRemainder(cursor, end, component.TypeName,
                       static_cast<u32>(sizeof(component.TypeName) - 1u), "",
                       EScene3DSerializeError::InvalidComponent);
    if (type_error != EScene3DSerializeError::None
        || component.TypeName[0] == '\0') {
        return EScene3DSerializeError::InvalidComponent;
    }
    for (u32 i = 0u; component.TypeName[i] != '\0'; ++i) {
        if (component.TypeName[i] == ' ' || component.TypeName[i] == '\t')
            return EScene3DSerializeError::InvalidComponent;
    }
    if (!components.TryPushBack(Move(component)))
        return EScene3DSerializeError::AllocationFailure;
    return EScene3DSerializeError::None;
}

EScene3DSerializeError ParseComponentPropertyLine(
    const char* line, u32 size, TArray<FParsedNode>& nodes,
    THashMap<i32, u32>& id_to_index,
    TArray<FParsedComponentProperty>& properties) noexcept {
    const char* cursor = line + 8u;
    const char* end = line + size;
    i32 id = -1, slot = -1, property = -1;
    FParsedComponentProperty parsed;
    if (!ParseI32(cursor, end, id)
        || !ParseI32(cursor, end, slot)
        || !ParseI32(cursor, end, property)
        || slot < 0 || property < 0) {
        return EScene3DSerializeError::InvalidComponentProperty;
    }
    for (u32 i = 0u; i < 4u; ++i) {
        if (!ParseF32(cursor, end, parsed.Value[i]))
            return EScene3DSerializeError::InvalidComponentProperty;
    }
    if (!IsOnlyWhitespace(cursor, end))
        return EScene3DSerializeError::InvalidComponentProperty;
    const u32* node_index = FindNodeIndex(id_to_index, id);
    if (node_index == nullptr) return EScene3DSerializeError::InvalidNodeId;
    if (static_cast<u32>(slot) >= nodes[*node_index].ComponentCount)
        return EScene3DSerializeError::InvalidComponentProperty;
    parsed.NodeIndex = *node_index;
    parsed.Slot = static_cast<u32>(slot);
    parsed.Property = static_cast<u32>(property);
    if (!properties.TryPushBack(parsed))
        return EScene3DSerializeError::AllocationFailure;
    return EScene3DSerializeError::None;
}

EScene3DSerializeError ParseCameraLine(
    const char* line, u32 size, const TArray<FParsedNode>& nodes,
    THashMap<i32, u32>& id_to_index,
    TArray<FParsedCamera>& cameras) noexcept {
    if (cameras.Size() >= kScene3DSerializeMaxCameraCount)
        return EScene3DSerializeError::CameraLimitExceeded;

    const char* cursor = line + 6u;
    const char* end = line + size;
    i32 node_id = -1;
    i32 projection = -1;
    i32 active = -1;
    FParsedCamera parsed;
    if (!ParseI32(cursor, end, node_id))
        return EScene3DSerializeError::InvalidInteger;
    const u32* node_index = id_to_index.Find(node_id);
    if (node_id < 0 || node_index == nullptr || *node_index >= nodes.Size())
        return EScene3DSerializeError::InvalidNodeId;
    if (!ParseCameraId(cursor, end, parsed.StableId)
        || !ParseI32(cursor, end, projection)
        || !ParseI32(cursor, end, parsed.Priority)
        || !ParseI32(cursor, end, active)
        || !ParseF32(cursor, end, parsed.FovYDegrees)
        || !ParseF32(cursor, end, parsed.OrthographicHeight)
        || !ParseF32(cursor, end, parsed.NearPlane)
        || !ParseF32(cursor, end, parsed.FarPlane)
        || !IsOnlyWhitespace(cursor, end)) {
        return EScene3DSerializeError::InvalidCamera;
    }
    if ((projection != 0 && projection != 1)
        || (active != 0 && active != 1)
        || parsed.Priority < kScene3DCameraMinPriority
        || parsed.Priority > kScene3DCameraMaxPriority
        || parsed.FovYDegrees < kScene3DCameraMinFovDegrees
        || parsed.FovYDegrees > kScene3DCameraMaxFovDegrees
        || parsed.OrthographicHeight < kScene3DCameraMinOrthoHeight
        || parsed.OrthographicHeight > kScene3DCameraMaxOrthoHeight
        || parsed.NearPlane < kScene3DCameraMinNearPlane
        || parsed.NearPlane > kScene3DCameraMaxNearPlane
        || parsed.FarPlane <= parsed.NearPlane
        || parsed.FarPlane > kScene3DCameraMaxFarPlane) {
        return EScene3DSerializeError::InvalidCamera;
    }
    for (u32 index = 0u; index < cameras.Size(); ++index) {
        if (cameras[index].NodeIndex == *node_index
            || std::strcmp(cameras[index].StableId, parsed.StableId) == 0) {
            return EScene3DSerializeError::DuplicateCamera;
        }
    }
    parsed.NodeIndex = *node_index;
    parsed.NodeId = node_id;
    parsed.Projection = static_cast<EScene3DCameraProjection>(projection);
    parsed.ActivePreferred = active != 0;
    if (!cameras.TryPushBack(parsed))
        return EScene3DSerializeError::AllocationFailure;
    return EScene3DSerializeError::None;
}

EScene3DSerializeError PrepareComponents(
    TArray<FParsedComponent>& components,
    const TArray<FParsedComponentProperty>& properties) noexcept {
    THashMap<u64, u32> component_indices;
    for (u32 i = 0u; i < components.Size(); ++i) {
        FParsedComponent& component = components[i];
        component.Instance = CreateComponentByName(component.TypeName);
        if (!component.Instance)
            return EScene3DSerializeError::InvalidComponent;
        const u64 key = (static_cast<u64>(component.NodeIndex) << 32u)
                      | static_cast<u64>(component.Slot);
        if (!component_indices.TryInsert(key, i))
            return EScene3DSerializeError::AllocationFailure;
    }
    for (u32 i = 0u; i < properties.Size(); ++i) {
        const FParsedComponentProperty& property = properties[i];
        const u64 key = (static_cast<u64>(property.NodeIndex) << 32u)
                      | static_cast<u64>(property.Slot);
        const u32* component_index = component_indices.Find(key);
        if (component_index == nullptr)
            return EScene3DSerializeError::InvalidComponentProperty;
        FParsedComponent& component = components[*component_index];
        const FTypeDesc* type =
            CTypeRegistry::Get().FindByName(component.Instance->ReflectName());
        if (type == nullptr || type->fields == nullptr
            || property.Property >= type->field_count) {
            return EScene3DSerializeError::InvalidComponentProperty;
        }
        ApplyFieldValue(component.Instance.Get(),
                        type->fields[property.Property], property.Value);
    }
    return EScene3DSerializeError::None;
}

FScene3DLoadResult LoadFailure(
    EScene3DSerializeError error, u32 bytes, u32 line, u32 node_count,
    u32 mesh_path_count) noexcept {
    return FScene3DLoadResult{error, bytes, node_count, mesh_path_count, line};
}

} // namespace

const char* Scene3DSerializeErrorName(EScene3DSerializeError error) noexcept {
    switch (error) {
    case EScene3DSerializeError::None:                   return "none";
    case EScene3DSerializeError::NullInput:              return "null_input";
    case EScene3DSerializeError::NullOutput:             return "null_output";
    case EScene3DSerializeError::BufferTooSmall:         return "buffer_too_small";
    case EScene3DSerializeError::InputTooLarge:          return "input_too_large";
    case EScene3DSerializeError::LineTooLong:            return "line_too_long";
    case EScene3DSerializeError::InvalidLine:            return "invalid_line";
    case EScene3DSerializeError::InvalidInteger:         return "invalid_integer";
    case EScene3DSerializeError::InvalidNumber:          return "invalid_number";
    case EScene3DSerializeError::InvalidPrimitive:       return "invalid_primitive";
    case EScene3DSerializeError::InvalidNodeId:          return "invalid_node_id";
    case EScene3DSerializeError::DuplicateNodeId:        return "duplicate_node_id";
    case EScene3DSerializeError::InvalidParent:          return "invalid_parent";
    case EScene3DSerializeError::MissingRoot:            return "missing_root";
    case EScene3DSerializeError::MultipleRoots:          return "multiple_roots";
    case EScene3DSerializeError::NodeLimitExceeded:      return "node_limit_exceeded";
    case EScene3DSerializeError::TreeDepthLimitExceeded: return "tree_depth_limit_exceeded";
    case EScene3DSerializeError::InvalidName:            return "invalid_name";
    case EScene3DSerializeError::InvalidMeshPath:        return "invalid_mesh_path";
    case EScene3DSerializeError::DuplicateMeshPath:      return "duplicate_mesh_path";
    case EScene3DSerializeError::AllocationFailure:      return "allocation_failure";
    case EScene3DSerializeError::DuplicateNodeReference: return "duplicate_node_reference";
    case EScene3DSerializeError::CyclicNodeGraph:        return "cyclic_node_graph";
    case EScene3DSerializeError::SerializedSizeOverflow: return "serialized_size_overflow";
    case EScene3DSerializeError::SceneChangedDuringSave: return "scene_changed_during_save";
    case EScene3DSerializeError::InvalidHeader:           return "invalid_header";
    case EScene3DSerializeError::UnsupportedVersion:      return "unsupported_version";
    case EScene3DSerializeError::UnsupportedDirective:    return "unsupported_directive";
    case EScene3DSerializeError::InvalidNodeFlags:        return "invalid_node_flags";
    case EScene3DSerializeError::InvalidMaterial:         return "invalid_material";
    case EScene3DSerializeError::InvalidComponent:        return "invalid_component";
    case EScene3DSerializeError::InvalidComponentProperty:return "invalid_component_property";
    case EScene3DSerializeError::InvalidCamera:           return "invalid_camera";
    case EScene3DSerializeError::DuplicateCamera:         return "duplicate_camera";
    case EScene3DSerializeError::CameraLimitExceeded:     return "camera_limit_exceeded";
    case EScene3DSerializeError::DirectiveLimitExceeded:  return "directive_limit_exceeded";
    case EScene3DSerializeError::ComponentLimitExceeded:  return "component_limit_exceeded";
    case EScene3DSerializeError::FileOpenFailed:          return "file_open_failed";
    case EScene3DSerializeError::FileSeekFailed:          return "file_seek_failed";
    case EScene3DSerializeError::FileSizeLimitExceeded:   return "file_size_limit_exceeded";
    case EScene3DSerializeError::FileReadFailed:          return "file_read_failed";
    case EScene3DSerializeError::EmbeddedNul:             return "embedded_nul";
    case EScene3DSerializeError::AssetPathInvalid:        return "asset_path_invalid";
    case EScene3DSerializeError::AssetMissing:            return "asset_missing";
    case EScene3DSerializeError::AssetDecodeFailed:       return "asset_decode_failed";
    case EScene3DSerializeError::MaterialDecodeFailed:    return "material_decode_failed";
    }
    return "unknown";
}

FScene3DSaveResult TrySaveScene3DText(
    const CSceneNodeGraph& graph, char* out, u32 cap) noexcept {
    FScene3DSaveResult result{};
    TArray<const ANode*> nodes;
    TArray<i32> parents;
    result.Error = Flatten(graph.Root(), nodes, parents);
    if (result.Error != EScene3DSerializeError::None) return result;
    result.NodeCount = static_cast<u32>(nodes.Size());

    u32 text_bytes = 0u;
    char line[kNodeLineCapacity];
    for (u32 i = 0u; i < nodes.Size(); ++i) {
        u32 line_size = 0u;
        result.Error = FormatNodeLine(
            *nodes[i], static_cast<i32>(i), parents[i], line, sizeof(line), line_size);
        if (result.Error != EScene3DSerializeError::None) return result;
        if (!CheckedAdd(text_bytes, line_size)) {
            result.Error = EScene3DSerializeError::SerializedSizeOverflow;
            return result;
        }
        const AMeshComponent3D* mesh = FindMesh(*nodes[i]);
        if (mesh != nullptr && mesh->Primitive() == EMeshPrimitive3D::Mesh
            && !mesh->MeshPath().IsEmpty()) {
            result.Error = FormatMeshLine(
                *mesh, static_cast<i32>(i), line, sizeof(line), line_size);
            if (result.Error != EScene3DSerializeError::None) return result;
            if (!CheckedAdd(text_bytes, line_size)) {
                result.Error = EScene3DSerializeError::SerializedSizeOverflow;
                return result;
            }
            ++result.MeshPathCount;
        }
        const ACameraComponent3D* camera = FindCamera(*nodes[i]);
        if (camera != nullptr) {
            if (HasMultipleCameras(*nodes[i])) {
                result.Error = EScene3DSerializeError::DuplicateCamera;
                return result;
            }
            if (result.CameraCount >= kScene3DSerializeMaxCameraCount) {
                result.Error =
                    EScene3DSerializeError::CameraLimitExceeded;
                return result;
            }
            for (u32 previous = 0u; previous < i; ++previous) {
                const ACameraComponent3D* previous_camera =
                    FindCamera(*nodes[previous]);
                if (previous_camera != nullptr
                    && std::strcmp(
                        previous_camera->StableId(), camera->StableId()) == 0) {
                    result.Error = EScene3DSerializeError::DuplicateCamera;
                    return result;
                }
            }
            result.Error = FormatCameraLine(
                *camera, static_cast<i32>(i), line, sizeof(line), line_size);
            if (result.Error != EScene3DSerializeError::None) return result;
            if (!CheckedAdd(text_bytes, line_size)) {
                result.Error =
                    EScene3DSerializeError::SerializedSizeOverflow;
                return result;
            }
            ++result.CameraCount;
        }
    }
    result.RequiredBytes = text_bytes;
    if (!CheckedAdd(result.RequiredBytes, 1u)) {
        result.Error = EScene3DSerializeError::SerializedSizeOverflow;
        return result;
    }

    if (out == nullptr) {
        result.Error = cap == 0u ? EScene3DSerializeError::BufferTooSmall
                                 : EScene3DSerializeError::NullOutput;
        return result;
    }
    if (cap < result.RequiredBytes) {
        result.Error = EScene3DSerializeError::BufferTooSmall;
        return result;
    }

    u32 cursor = 0u;
    u32 emitted_mesh_paths = 0u;
    u32 emitted_cameras = 0u;
    for (u32 i = 0u; i < nodes.Size(); ++i) {
        u32 line_size = 0u;
        result.Error = FormatNodeLine(
            *nodes[i], static_cast<i32>(i), parents[i], line, sizeof(line), line_size);
        if (result.Error != EScene3DSerializeError::None
            || line_size > cap - cursor - 1u) {
            result.Error = EScene3DSerializeError::SceneChangedDuringSave;
            result.BytesWritten = cursor;
            out[cursor] = '\0';
            return result;
        }
        std::memcpy(out + cursor, line, line_size);
        cursor += line_size;

        const AMeshComponent3D* mesh = FindMesh(*nodes[i]);
        if (mesh != nullptr && mesh->Primitive() == EMeshPrimitive3D::Mesh
            && !mesh->MeshPath().IsEmpty()) {
            result.Error = FormatMeshLine(
                *mesh, static_cast<i32>(i), line, sizeof(line), line_size);
            if (result.Error != EScene3DSerializeError::None
                || line_size > cap - cursor - 1u) {
                result.Error = EScene3DSerializeError::SceneChangedDuringSave;
                result.BytesWritten = cursor;
                out[cursor] = '\0';
                return result;
            }
            std::memcpy(out + cursor, line, line_size);
            cursor += line_size;
            ++emitted_mesh_paths;
        }
        const ACameraComponent3D* camera = FindCamera(*nodes[i]);
        if (camera != nullptr) {
            result.Error = FormatCameraLine(
                *camera, static_cast<i32>(i), line, sizeof(line), line_size);
            if (result.Error != EScene3DSerializeError::None
                || line_size > cap - cursor - 1u) {
                result.Error = EScene3DSerializeError::SceneChangedDuringSave;
                result.BytesWritten = cursor;
                out[cursor] = '\0';
                return result;
            }
            std::memcpy(out + cursor, line, line_size);
            cursor += line_size;
            ++emitted_cameras;
        }
    }
    out[cursor] = '\0';
    result.BytesWritten = cursor;
    if (cursor + 1u != result.RequiredBytes
        || emitted_mesh_paths != result.MeshPathCount
        || emitted_cameras != result.CameraCount) {
        result.Error = EScene3DSerializeError::SceneChangedDuringSave;
        return result;
    }
    result.Error = EScene3DSerializeError::None;
    return result;
}

u32 SaveScene3DText(const CSceneNodeGraph& graph, char* out, u32 cap) noexcept {
    const FScene3DSaveResult result = TrySaveScene3DText(graph, out, cap);
    return result.Succeeded() ? result.BytesWritten : 0u;
}



namespace {

struct FParsedScene3DDocument {
    TArray<FParsedNode> Nodes;
    TArray<FParsedComponent> Components;
    TArray<FParsedComponentProperty> Properties;
    TArray<FParsedCamera> Cameras;
    bool EditorDocument = false;
    u32 MeshPathCount = 0u;
    u32 SourceBytes = 0u;
};

bool CameraRecordPrecedes(
    const FParsedCamera& left, const FParsedCamera& right) noexcept {
    if (left.ActivePreferred != right.ActivePreferred)
        return left.ActivePreferred;
    if (left.Priority != right.Priority) return left.Priority > right.Priority;
    const int identity_order = std::strcmp(left.StableId, right.StableId);
    if (identity_order != 0) return identity_order < 0;
    return left.NodeId < right.NodeId;
}

bool ParsedNodeEffectivelyEnabled(
    const TArray<FParsedNode>& nodes, u32 node_index) noexcept {
    u32 traversed = 0u;
    i32 current = static_cast<i32>(node_index);
    while (current >= 0) {
        if (static_cast<u32>(current) >= nodes.Size()
            || traversed++ > kScene3DSerializeMaxTreeDepth
            || !nodes[static_cast<u32>(current)].Enabled) {
            return false;
        }
        current = nodes[static_cast<u32>(current)].ParentIndex;
    }
    return true;
}

bool NormalizeCameraVector(FVec3 input, FVec3& output) noexcept {
    const f32 length_squared =
        input.x * input.x + input.y * input.y + input.z * input.z;
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-12f)
        return false;
    const f32 inverse_length = 1.0f / std::sqrt(length_squared);
    output = FVec3{
        input.x * inverse_length,
        input.y * inverse_length,
        input.z * inverse_length};
    return IsFinite(output);
}

FVec3 CrossCameraVector(FVec3 left, FVec3 right) noexcept {
    return FVec3{
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x};
}

EScene3DSerializeError BuildSelectedCameraState(
    const FParsedScene3DDocument& document,
    FScene3DCameraState& selected,
    u32& active_preferred_count) noexcept {
    active_preferred_count = 0u;
    const FParsedCamera* best = nullptr;
    for (u32 index = 0u; index < document.Cameras.Size(); ++index) {
        const FParsedCamera& candidate = document.Cameras[index];
        if (candidate.ActivePreferred) ++active_preferred_count;
        if (!ParsedNodeEffectivelyEnabled(document.Nodes, candidate.NodeIndex))
            continue;
        if (best == nullptr || CameraRecordPrecedes(candidate, *best))
            best = &candidate;
    }
    if (best == nullptr) return EScene3DSerializeError::None;

    TArray<FTransform3D> world_transforms;
    if (!world_transforms.TryResize(document.Nodes.Size()))
        return EScene3DSerializeError::AllocationFailure;
    for (u32 index = 0u; index < document.Nodes.Size(); ++index) {
        const FParsedNode& node = document.Nodes[index];
        FTransform3D local;
        local.position = node.Position;
        local.SetEulerDeg(node.RotationDeg);
        local.scale = node.Scale;
        if (node.ParentIndex >= 0) {
            const u32 parent_index = static_cast<u32>(node.ParentIndex);
            if (parent_index >= index)
                return EScene3DSerializeError::InvalidParent;
            world_transforms[index] =
                world_transforms[parent_index].Compose(local);
        } else {
            world_transforms[index] = local;
        }
    }

    const FTransform3D& world = world_transforms[best->NodeIndex];
    if (!IsFinite(world.position))
        return EScene3DSerializeError::InvalidCamera;
    FVec3 forward;
    FVec3 authored_up;
    if (!NormalizeCameraVector(
            Rotate(world.rotation, FVec3{0.0f, 0.0f, 1.0f}), forward)
        || !NormalizeCameraVector(
            Rotate(world.rotation, FVec3{0.0f, 1.0f, 0.0f}), authored_up)) {
        return EScene3DSerializeError::InvalidCamera;
    }
    FVec3 right;
    FVec3 up;
    if (!NormalizeCameraVector(
            CrossCameraVector(authored_up, forward), right)
        || !NormalizeCameraVector(
            CrossCameraVector(forward, right), up)) {
        return EScene3DSerializeError::InvalidCamera;
    }

    selected.IsAuthored = true;
    selected.IsActivePreferred = best->ActivePreferred;
    selected.NodeId = best->NodeId;
    selected.Priority = best->Priority;
    selected.Projection = best->Projection;
    selected.FovYDegrees = best->FovYDegrees;
    selected.OrthographicHeight = best->OrthographicHeight;
    selected.NearPlane = best->NearPlane;
    selected.FarPlane = best->FarPlane;
    selected.Position = world.position;
    selected.Forward = forward;
    selected.Up = up;
    std::memcpy(
        selected.StableId, best->StableId,
        kScene3DSerializeMaxCameraIdBytes + 1u);
    return EScene3DSerializeError::None;
}

bool StartsWith(const char* line, u32 size, const char* prefix, u32 prefix_size) noexcept {
    return size >= prefix_size && std::memcmp(line, prefix, prefix_size) == 0;
}

FScene3DLoadResult ParseScene3DDocument(
    const char* text, u32 size, FParsedScene3DDocument& document) noexcept {
    if (text == nullptr)
        return LoadFailure(EScene3DSerializeError::NullInput, 0u, 0u, 0u, 0u);
    if (size > kScene3DSerializeMaxInputBytes)
        return LoadFailure(EScene3DSerializeError::InputTooLarge, 0u, 0u, 0u, 0u);

    THashMap<i32, u32> id_to_index;
    u32 cursor = 0u;
    u32 line_number = 0u;
    u32 directive_count = 0u;
    bool saw_content = false;
    char line[kScene3DSerializeMaxLineBytes + 1u];
    while (cursor < size) {
        const u32 line_start = cursor;
        ++line_number;
        while (cursor < size && text[cursor] != '\n') {
            if (text[cursor] == '\0') {
                return LoadFailure(
                    EScene3DSerializeError::InvalidLine, cursor, line_number,
                    static_cast<u32>(document.Nodes.Size()),
                    document.MeshPathCount);
            }
            ++cursor;
        }
        u32 line_size = cursor - line_start;
        if (cursor < size && text[cursor] == '\n') ++cursor;
        if (line_size > 0u && text[line_start + line_size - 1u] == '\r')
            --line_size;
        if (line_size > kScene3DSerializeMaxLineBytes) {
            return LoadFailure(
                EScene3DSerializeError::LineTooLong, line_start, line_number,
                static_cast<u32>(document.Nodes.Size()),
                document.MeshPathCount);
        }
        if (line_size == 0u) continue;
        std::memcpy(line, text + line_start, line_size);
        line[line_size] = '\0';

        if (StartsWith(line, line_size, "ACS3D", 5u)) {
            if (saw_content)
                return LoadFailure(
                    EScene3DSerializeError::InvalidHeader, line_start,
                    line_number, static_cast<u32>(document.Nodes.Size()),
                    document.MeshPathCount);
            saw_content = true;
            if (line_size != 8u || std::memcmp(line, "ACS3D v2", 8u) != 0) {
                return LoadFailure(
                    StartsWith(line, line_size, "ACS3D v", 7u)
                        ? EScene3DSerializeError::UnsupportedVersion
                        : EScene3DSerializeError::InvalidHeader,
                    line_start, line_number, 0u, 0u);
            }
            document.EditorDocument = true;
            continue;
        }

        saw_content = true;
        if (++directive_count > kScene3DSerializeMaxDirectiveRecords) {
            return LoadFailure(
                EScene3DSerializeError::DirectiveLimitExceeded, line_start,
                line_number, static_cast<u32>(document.Nodes.Size()),
                document.MeshPathCount);
        }

        EScene3DSerializeError error = EScene3DSerializeError::InvalidLine;
        if (StartsWith(line, line_size, "N3D ", 4u)) {
            error = ParseNodeLine(
                line, line_size, document.EditorDocument,
                document.Nodes, id_to_index);
        } else if (StartsWith(line, line_size, "MSH3D ", 6u)) {
            error = ParseMeshLine(
                line, line_size, document.Nodes, id_to_index,
                document.MeshPathCount);
        } else if (document.EditorDocument
                   && StartsWith(line, line_size, "FLG3D ", 6u)) {
            error = ParseFlagsLine(
                line, line_size, document.Nodes, id_to_index);
        } else if (document.EditorDocument
                   && StartsWith(line, line_size, "MAT3D ", 6u)) {
            error = ParseMaterialLine(
                line, line_size, document.Nodes, id_to_index);
        } else if (document.EditorDocument
                   && StartsWith(line, line_size, "EMPTY3D ", 8u)) {
            error = ParseEmptyLine(
                line, line_size, document.Nodes, id_to_index);
        } else if (document.EditorDocument
                   && StartsWith(line, line_size, "CMP3D ", 6u)) {
            error = ParseComponentLine(
                line, line_size, document.Nodes, id_to_index,
                document.Components);
        } else if (document.EditorDocument
                   && StartsWith(line, line_size, "CPROP3D ", 8u)) {
            error = ParseComponentPropertyLine(
                line, line_size, document.Nodes, id_to_index,
                document.Properties);
        } else if (StartsWith(line, line_size, "CAM3D ", 6u)) {
            error = ParseCameraLine(
                line, line_size, document.Nodes, id_to_index,
                document.Cameras);
        } else if (document.EditorDocument
                   && StartsWith(line, line_size, "SEL3D ", 6u)) {
            const char* selection = line + 6u;
            const char* end = line + line_size;
            i32 selected_id = -1;
            if (!ParseI32(selection, end, selected_id)
                || !IsOnlyWhitespace(selection, end)) {
                error = EScene3DSerializeError::InvalidLine;
            } else if (selected_id < 0
                       || (selected_id > 0
                           && id_to_index.Find(selected_id) == nullptr)) {
                // Editor documents use zero as the explicit "no selection"
                // sentinel.  A positive selection must name a node already
                // present in the validated document; otherwise accepting the
                // file would leave editor state referring to no scene object.
                error = EScene3DSerializeError::InvalidNodeId;
            } else {
                error = EScene3DSerializeError::None;
            }
        } else if (document.EditorDocument
                   && (StartsWith(line, line_size, "SPR3D ", 6u)
                       || StartsWith(line, line_size, "PLY3D ", 6u)
                       || StartsWith(line, line_size, "PFAB3D ", 7u))) {
            error = EScene3DSerializeError::UnsupportedDirective;
        }
        if (error != EScene3DSerializeError::None) {
            return LoadFailure(
                error, line_start, line_number,
                static_cast<u32>(document.Nodes.Size()),
                document.MeshPathCount);
        }
    }

    if (document.Nodes.IsEmpty() && !document.EditorDocument) {
        return LoadFailure(
            EScene3DSerializeError::MissingRoot, cursor, line_number,
            0u, document.MeshPathCount);
    }
    const EScene3DSerializeError component_error =
        PrepareComponents(document.Components, document.Properties);
    if (component_error != EScene3DSerializeError::None) {
        return LoadFailure(
            component_error, cursor, line_number,
            static_cast<u32>(document.Nodes.Size()),
            document.MeshPathCount);
    }
    document.SourceBytes = size;
    return FScene3DLoadResult{
        EScene3DSerializeError::None,
        size,
        static_cast<u32>(document.Nodes.Size())
            + (document.EditorDocument ? 1u : 0u),
        document.MeshPathCount,
        0u,
        0u
    };
}

FScene3DLoadResult CommitScene3DDocument(
    CSceneNodeGraph& graph, FParsedScene3DDocument& document,
    u32 dependencies_loaded) noexcept {
    FScene3DCameraState active_camera;
    u32 active_preferred_camera_count = 0u;
    const EScene3DSerializeError camera_error = BuildSelectedCameraState(
        document, active_camera, active_preferred_camera_count);
    if (camera_error != EScene3DSerializeError::None) {
        return LoadFailure(
            camera_error, document.SourceBytes, 0u,
            static_cast<u32>(document.Nodes.Size()),
            document.MeshPathCount);
    }

    TArray<ANode*> runtime_nodes;
    if (!runtime_nodes.TryReserve(document.Nodes.Size())) {
        return LoadFailure(
            EScene3DSerializeError::AllocationFailure, document.SourceBytes,
            0u, static_cast<u32>(document.Nodes.Size()),
            document.MeshPathCount);
    }

    // Build into a private graph. The caller's graph is not touched until the
    // full node/component/camera commit has succeeded.
    CSceneNodeGraph staged_scene;
    staged_scene.Clear();
    staged_scene.Root().RemoveAllComponents();
    staged_scene.Root().SetName(FStringView("Root"));
    staged_scene.Root().Local() = FTransform3D{};
    staged_scene.Root().SetVisible(true);
    staged_scene.Root().SetEnabled(true);

    for (u32 i = 0u; i < document.Nodes.Size(); ++i) {
        FParsedNode& record = document.Nodes[i];
        ANode* node = nullptr;
        if (!document.EditorDocument && i == 0u) {
            node = &staged_scene.Root();
            node->SetName(FStringView(record.Name));
        } else {
            ANode* parent = &staged_scene.Root();
            if (record.ParentIndex >= 0) {
                const u32 parent_index =
                    static_cast<u32>(record.ParentIndex);
                if (parent_index >= runtime_nodes.Size()) {
                    return LoadFailure(
                        EScene3DSerializeError::InvalidParent,
                        document.SourceBytes, 0u,
                        static_cast<u32>(document.Nodes.Size()),
                        document.MeshPathCount);
                }
                parent = runtime_nodes[parent_index];
            }
            const FScene3DSpawnResult spawned =
                staged_scene.TrySpawn(FStringView(record.Name), parent);
            if (!spawned.Succeeded()) {
                return LoadFailure(
                    EScene3DSerializeError::AllocationFailure,
                    document.SourceBytes, 0u,
                    static_cast<u32>(document.Nodes.Size()),
                    document.MeshPathCount);
            }
            node = spawned.Node;
        }

        node->_SetSerialId(record.SourceId);
        node->Local().position = record.Position;
        node->Local().SetEulerDeg(record.RotationDeg);
        node->Local().scale = record.Scale;
        node->SetVisible(record.Visible);
        node->SetEnabled(record.Enabled);
        if (record.Primitive >= 0 && !record.Empty) {
            AMeshComponent3D& mesh =
                node->GetOrAddComponent<AMeshComponent3D>();
            mesh.SetPrimitive(
                static_cast<EMeshPrimitive3D>(record.Primitive));
            mesh.SetColor(record.Color);
            if (record.HasMeshPath) {
                mesh.SetMeshPath(FStringView(record.MeshPath));
                if (record.LoadedMesh)
                    mesh.SetMeshAsset(record.LoadedMesh);
            }
            if (record.HasMaterialPath) {
                mesh.SetMaterialPath(FStringView(record.MaterialPath));
                mesh.SetMaterial(record.LoadedMaterial);
            } else if (record.HasLegacyMaterial) {
                mesh.MaterialMut().pbr.metallic = record.Metallic;
                mesh.MaterialMut().pbr.roughness = record.Roughness;
                mesh.SetMaterialLoaded(true);
            }
        }
        if (!runtime_nodes.TryPushBack(node)) {
            return LoadFailure(
                EScene3DSerializeError::AllocationFailure,
                document.SourceBytes, 0u,
                static_cast<u32>(document.Nodes.Size()),
                document.MeshPathCount);
        }
    }

    for (u32 i = 0u; i < document.Components.Size(); ++i) {
        FParsedComponent& component = document.Components[i];
        if (component.NodeIndex >= runtime_nodes.Size()
            || !component.Instance) {
            return LoadFailure(
                EScene3DSerializeError::InvalidComponent,
                document.SourceBytes, 0u,
                static_cast<u32>(document.Nodes.Size()),
                document.MeshPathCount);
        }
        runtime_nodes[component.NodeIndex]->AttachComponent(
            Move(component.Instance));
    }

    for (u32 i = 0u; i < document.Cameras.Size(); ++i) {
        const FParsedCamera& camera = document.Cameras[i];
        if (camera.NodeIndex >= runtime_nodes.Size()) {
            return LoadFailure(
                EScene3DSerializeError::InvalidCamera,
                document.SourceBytes, 0u,
                static_cast<u32>(document.Nodes.Size()),
                document.MeshPathCount);
        }
        FScene3DCameraState authored;
        authored.IsAuthored = true;
        authored.IsActivePreferred = camera.ActivePreferred;
        authored.NodeId = camera.NodeId;
        authored.Priority = camera.Priority;
        authored.Projection = camera.Projection;
        authored.FovYDegrees = camera.FovYDegrees;
        authored.OrthographicHeight = camera.OrthographicHeight;
        authored.NearPlane = camera.NearPlane;
        authored.FarPlane = camera.FarPlane;
        std::memcpy(
            authored.StableId, camera.StableId,
            kScene3DSerializeMaxCameraIdBytes + 1u);
        ACameraComponent3D& component =
            runtime_nodes[camera.NodeIndex]
                ->AddComponent<ACameraComponent3D>();
        if (!component.TrySetAuthoredState(authored)) {
            return LoadFailure(
                EScene3DSerializeError::InvalidCamera,
                document.SourceBytes, 0u,
                static_cast<u32>(document.Nodes.Size()),
                document.MeshPathCount);
        }
    }

    FScene3DLoadResult result{
        EScene3DSerializeError::None,
        document.SourceBytes,
        static_cast<u32>(document.Nodes.Size())
            + (document.EditorDocument ? 1u : 0u),
        document.MeshPathCount,
        0u,
        dependencies_loaded
    };
    result.CameraCount = document.Cameras.Size();
    result.ActivePreferredCameraCount = active_preferred_camera_count;
    result.ActiveCamera = active_camera;
    graph.SwapContents(staged_scene);
    return result;
}

bool EndsWithIgnoreCase(const char* path, const char* extension) noexcept {
    if (path == nullptr || extension == nullptr) return false;
    usize path_size = 0u, extension_size = 0u;
    while (path[path_size] != '\0') ++path_size;
    while (extension[extension_size] != '\0') ++extension_size;
    if (extension_size > path_size) return false;
    const char* suffix = path + path_size - extension_size;
    for (usize i = 0u; i < extension_size; ++i) {
        char a = suffix[i], b = extension[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool IsSafeVirtualAssetPath(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0'
        || path[0] == '/' || path[0] == '\\') {
        return false;
    }
    u32 segment_size = 0u;
    char first = '\0', second = '\0';
    for (u32 i = 0u; path[i] != '\0'; ++i) {
        const char ch = path[i];
        if (i >= kScene3DSerializeMaxMeshPathBytes
            || ch == '\\' || ch == ':' || ch == '\r' || ch == '\n') {
            return false;
        }
        if (ch == '/') {
            if (segment_size == 0u
                || (segment_size == 1u && first == '.')
                || (segment_size == 2u && first == '.' && second == '.')) {
                return false;
            }
            segment_size = 0u;
            first = second = '\0';
        } else {
            if (segment_size == 0u) first = ch;
            else if (segment_size == 1u) second = ch;
            ++segment_size;
        }
    }
    return segment_size > 0u
        && !(segment_size == 1u && first == '.')
        && !(segment_size == 2u && first == '.' && second == '.');
}

bool ReadPackEntry(
    IAssetPackReader& pack, const char* path, u64 max_bytes,
    TArray<byte>& bytes) noexcept {
    const auto size_result = pack.FileSize(path);
    if (size_result.IsErr() || size_result.Value() > max_bytes)
        return false;
    const u64 size64 = size_result.Value();
    const usize size = static_cast<usize>(size64);
    if (static_cast<u64>(size) != size64 || !bytes.TryResize(size))
        return false;
    if (size > 0u) {
        const auto read_result =
            pack.ReadFile(path, reinterpret_cast<u8*>(bytes.Data()), size64);
        if (read_result.IsErr()) {
            bytes.Clear();
            return false;
        }
    }
    return true;
}

TResult<TSharedPtr<AAsset>> DecodeMesh(
    const char* path, const TArray<byte>& bytes) noexcept {
    if (EndsWithIgnoreCase(path, ".glb")) {
        CGlbAssetLoader loader;
        return loader.LoadFromBytes(kInvalidAssetId, bytes);
    }
    if (EndsWithIgnoreCase(path, ".gltf")) {
        CGltfAssetLoader loader;
        return loader.LoadFromBytes(kInvalidAssetId, bytes);
    }
    if (EndsWithIgnoreCase(path, ".obj")) {
        CObjAssetLoader loader;
        return loader.LoadFromBytes(kInvalidAssetId, bytes);
    }
    if (EndsWithIgnoreCase(path, ".fbx")) {
        CFbxAssetLoader loader;
        return loader.LoadFromBytes(kInvalidAssetId, bytes);
    }
    return TResult<TSharedPtr<AAsset>>(
        ACS_ERR(Asset, 496, "Scene3D mesh format is unsupported"));
}

/** Scene3D dependency の decode 種別。 */
enum class ESceneDependencyKind : u8 {
    /** メッシュアセット。 */
    Mesh = 0u,
    /** マテリアルアセット。 */
    Material = 1u,
};

/** 同じ path と種別を参照する node 群を一件へまとめた record。 */
struct FSceneDependencyRecord {
    /** allocator と dependency identity を指定して構築する。 */
    FSceneDependencyRecord(IAllocator& Allocator, ESceneDependencyKind InKind, const char* InPath, u64 InHash) noexcept
        : Nodes(Allocator), Kind(InKind), Path(InPath), Hash(InHash)
    {
    }

    /** 所有 node 配列のコピーを禁止する。 */
    FSceneDependencyRecord(const FSceneDependencyRecord&) = delete;
    /** 所有 node 配列のコピー代入を禁止する。 */
    FSceneDependencyRecord& operator=(const FSceneDependencyRecord&) = delete;
    /** 所有 node 配列の move を許可する。 */
    FSceneDependencyRecord(FSceneDependencyRecord&&) noexcept = default;
    /** 所有 node 配列の move 代入を許可する。 */
    FSceneDependencyRecord& operator=(FSceneDependencyRecord&&) noexcept = default;

    /** この依存を参照する node index。 */
    TArray<u32> Nodes;

    /** 依存のデコード種別。 */
    ESceneDependencyKind Kind = ESceneDependencyKind::Mesh;

    /** document 内で寿命が保証された初出パス。 */
    const char* Path = nullptr;

    /** 種別を含む検索用ハッシュ。 */
    u64 Hash = 0u;
};

/** dependency 種別と path から検索用 hash を作る。 */
u64 SceneDependencyHash(ESceneDependencyKind Kind, const char* Path) noexcept
{
    /** NUL を除く path byte 数。 */
    usize Length = 0u;
    while (Path[Length] != '\0') ++Length;
    /** path だけから得た基礎 hash。 */
    const u64 PathHash = HashBytes(Path, Length);
    return PathHash ^ (static_cast<u64>(Kind) + 0x9E3779B97F4A7C15ull + (PathHash << 6u) + (PathHash >> 2u));
}

/** 初出順を維持して dependency または参照 node を追加する。 */
bool AddStableSceneDependency(TArray<FSceneDependencyRecord>& Dependencies, THashMap<u64, u32>& FirstByHash, ESceneDependencyKind Kind, const char* Path, u32 NodeIndex) noexcept
{
    /** 種別を含む dependency hash。 */
    const u64 Hash = SceneDependencyHash(Kind, Path);
    if (FirstByHash.Find(Hash) != nullptr) {
        /** hash 候補を完全一致で確認する添字。 */
        for (usize Index = 0u; Index < Dependencies.Size(); ++Index) {
            /** 現在確認する dependency record。 */
            FSceneDependencyRecord& Existing = Dependencies[Index];
            if (Existing.Hash == Hash && Existing.Kind == Kind && std::strcmp(Existing.Path, Path) == 0) {
                return Existing.Nodes.TryPushBack(NodeIndex);
            }
        }
    }

    /** 新規 record を追加する添字。 */
    const u32 NewIndex = static_cast<u32>(Dependencies.Size());
    /** 追加できた dependency record。 */
    FSceneDependencyRecord* const Record = Dependencies.TryEmplaceBack(*Dependencies.GetAllocator(), Kind, Path, Hash);
    if (Record == nullptr || !Record->Nodes.TryPushBack(NodeIndex)) {
        return false;
    }
    if (FirstByHash.Find(Hash) == nullptr && !FirstByHash.TryInsert(Hash, NewIndex)) {
        Dependencies.PopBack();
        return false;
    }
    return true;
}

/** document の初出順で重複 dependency を集約する。 */
EScene3DSerializeError BuildStableSceneDependencyOrder(FParsedScene3DDocument& Document, bool ValidateVirtualPaths, TArray<FSceneDependencyRecord>& Dependencies) noexcept
{
    /** hash ごとの最初の dependency 添字。 */
    THashMap<u64, u32> FirstByHash;
    if (!Dependencies.TryReserve(static_cast<usize>(Document.Nodes.Size()) * 2u)) {
        return EScene3DSerializeError::AllocationFailure;
    }
    /** document node を初出順に調べる添字。 */
    for (u32 NodeIndex = 0u; NodeIndex < Document.Nodes.Size(); ++NodeIndex) {
        /** 現在 dependency を収集する node。 */
        FParsedNode& Node = Document.Nodes[NodeIndex];
        if (Node.HasMeshPath) {
            if (ValidateVirtualPaths && !IsSafeVirtualAssetPath(Node.MeshPath)) {
                return EScene3DSerializeError::AssetPathInvalid;
            }
            if (!AddStableSceneDependency(Dependencies, FirstByHash, ESceneDependencyKind::Mesh, Node.MeshPath, NodeIndex)) {
                return EScene3DSerializeError::AllocationFailure;
            }
        }
        if (Node.HasMaterialPath) {
            if (ValidateVirtualPaths && !IsSafeVirtualAssetPath(Node.MaterialPath)) {
                return EScene3DSerializeError::AssetPathInvalid;
            }
            if (!AddStableSceneDependency(Dependencies, FirstByHash, ESceneDependencyKind::Material, Node.MaterialPath, NodeIndex)) {
                return EScene3DSerializeError::AllocationFailure;
            }
        }
    }
    return EScene3DSerializeError::None;
}

/** 一件の共有 dependency を decode し、全参照 node へ割り当てる。 */
EScene3DSerializeError DecodeAndAssignSceneDependency(FParsedScene3DDocument& Document, const FSceneDependencyRecord& Dependency, const TArray<byte>& Bytes, u32& DependenciesLoaded) noexcept
{
    if (Dependency.Kind == ESceneDependencyKind::Mesh) {
        /** 共有する mesh decode 結果。 */
        auto Decoded = DecodeMesh(Dependency.Path, Bytes);
        if (Decoded.IsErr() || !Decoded.Value()) {
            return EScene3DSerializeError::AssetDecodeFailed;
        }
        /** mesh を割り当てる参照 node 添字。 */
        for (u32 NodeIndex : Dependency.Nodes) {
            Document.Nodes[NodeIndex].LoadedMesh = Decoded.Value();
            ++DependenciesLoaded;
        }
        return EScene3DSerializeError::None;
    }

    /** 共有する material decode 先。 */
    FMaterial2D Material{};
    /** material text の decode 結果。 */
    const FMaterial2DLoadResult MaterialResult = TryParseAcsmatText(reinterpret_cast<const char*>(Bytes.Data()), Bytes.Size(), Material);
    if (!MaterialResult.Succeeded()) {
        return EScene3DSerializeError::MaterialDecodeFailed;
    }
    /** material を割り当てる参照 node 添字。 */
    for (u32 NodeIndex : Dependency.Nodes) {
        Document.Nodes[NodeIndex].LoadedMaterial = Material;
        ++DependenciesLoaded;
    }
    return EScene3DSerializeError::None;
}

/** pack dependency を安定順序の有界 batch で読み込む。 */
EScene3DSerializeError LoadPackDependencies(IAssetPackReader& pack, FParsedScene3DDocument& document, u32& dependencies_loaded) noexcept {
    /** 初出順に集約した dependency 群。 */
    TArray<FSceneDependencyRecord> Dependencies;
    /** dependency 収集と path 検証の結果。 */
    const EScene3DSerializeError OrderError = BuildStableSceneDependencyOrder(document, true, Dependencies);
    if (OrderError != EScene3DSerializeError::None) return OrderError;

    /** 一回で reader へ渡す最大 dependency 数。 */
    constexpr u32 kBatchEntries = 8u;
    /** 一回で確保する最大 payload byte 数。 */
    constexpr u64 kBatchBytes = 32u * 1024u * 1024u;
    /** batch 内の各 dependency payload。 */
    TArray<byte> BatchBytes[kBatchEntries];
    /** reader へ渡す batch request 群。 */
    FAssetPackReadRequest Requests[kBatchEntries]{};
    /** request と dependency record の対応添字。 */
    u32 DependencyIndices[kBatchEntries]{};
    /** 空 payload request へ渡す有効な出力先。 */
    u8 EmptyDestinations[kBatchEntries]{};

    /** 次に batch へ追加する dependency 添字。 */
    u32 Cursor = 0u;
    while (Cursor < Dependencies.Size()) {
        /** 現在 batch に追加済みの request 数。 */
        u32 BatchCount = 0u;
        /** 現在 batch の payload 合計 byte 数。 */
        u64 BatchSize = 0u;
        while (Cursor < Dependencies.Size() && BatchCount < kBatchEntries) {
            /** 現在 batch へ追加を試す dependency。 */
            const FSceneDependencyRecord& Dependency = Dependencies[Cursor];
            /** dependency 種別ごとの最大 payload byte 数。 */
            const u64 MaxBytes = Dependency.Kind == ESceneDependencyKind::Mesh ? kScene3DAssetMaxBytes : static_cast<u64>(kMaterial2DMaxTextBytes);
            /** pack 内 payload size の取得結果。 */
            const auto SizeResult = pack.FileSize(Dependency.Path);
            if (SizeResult.IsErr() || SizeResult.Value() > MaxBytes) {
                if (BatchCount == 0u)
                    return EScene3DSerializeError::AssetMissing;
                break;
            }

            /** pack が報告した payload byte 数。 */
            const u64 Size64 = SizeResult.Value();
            if (BatchCount > 0u && (BatchSize >= kBatchBytes || Size64 > kBatchBytes - BatchSize)) {
                break;
            }
            /** address space 内で表現した payload byte 数。 */
            const usize Size = static_cast<usize>(Size64);
            if (static_cast<u64>(Size) != Size64 || !BatchBytes[BatchCount].TryResize(Size)) {
                return EScene3DSerializeError::AssetMissing;
            }

            Requests[BatchCount].Name = Dependency.Path;
            Requests[BatchCount].OutBuffer =
                Size > 0u
                    ? reinterpret_cast<u8*>(BatchBytes[BatchCount].Data())
                    : &EmptyDestinations[BatchCount];
            Requests[BatchCount].BufferSize = Size64;
            DependencyIndices[BatchCount] = Cursor;
            BatchSize += Size64;
            ++BatchCount;
            ++Cursor;
        }

        if (BatchCount == 0u) {
            return EScene3DSerializeError::AssetMissing;
        }
        /** reader が完了した先頭 request 数。 */
        u32 CompletedCount = 0u;
        /** 現在 batch の read 結果。 */
        const auto ReadResult = pack.ReadFiles(Requests, BatchCount, &CompletedCount);
        if (CompletedCount > BatchCount) {
            return EScene3DSerializeError::AssetMissing;
        }
        // 後続 read が失敗しても、旧逐次経路と同じく先行 dependency の
        // decode error を先に確定する。外部 Scene への commit はまだ行わない。
        /** read 済み dependency を decode する添字。 */
        for (u32 Index = 0u; Index < CompletedCount; ++Index) {
            /** 現在 dependency の decode と node 割り当て結果。 */
            const EScene3DSerializeError DecodeError = DecodeAndAssignSceneDependency(document, Dependencies[DependencyIndices[Index]], BatchBytes[Index], dependencies_loaded);
            if (DecodeError != EScene3DSerializeError::None)
                return DecodeError;
            BatchBytes[Index].Clear();
        }
        if (ReadResult.IsErr() || CompletedCount != BatchCount) {
            return EScene3DSerializeError::AssetMissing;
        }
    }
    return EScene3DSerializeError::None;
}

EScene3DSerializeError ReadLooseFile(
    const char* path, u64 max_bytes, TArray<byte>& bytes) noexcept {
    std::FILE* file = path != nullptr ? std::fopen(path, "rb") : nullptr;
    if (file == nullptr) return EScene3DSerializeError::FileOpenFailed;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return EScene3DSerializeError::FileSeekFailed;
    }
    const long length = std::ftell(file);
    if (length < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return EScene3DSerializeError::FileSeekFailed;
    }
    if (static_cast<u64>(length) > max_bytes) {
        std::fclose(file);
        return EScene3DSerializeError::FileSizeLimitExceeded;
    }
    const usize size = static_cast<usize>(length);
    if (!bytes.TryResize(size)) {
        std::fclose(file);
        return EScene3DSerializeError::AllocationFailure;
    }
    const usize read = size > 0u
        ? std::fread(bytes.Data(), 1u, size, file)
        : 0u;
    const bool failed = read != size || std::ferror(file) != 0;
    std::fclose(file);
    return failed ? EScene3DSerializeError::FileReadFailed
                  : EScene3DSerializeError::None;
}

bool ResolveLooseDependencyPath(
    const char* scene_path, const char* reference,
    char* output, u32 capacity) noexcept {
    if (scene_path == nullptr || reference == nullptr
        || output == nullptr || capacity == 0u
        || reference[0] == '\0') {
        return false;
    }
    const bool absolute =
        reference[0] == '/' || reference[0] == '\\'
        || (reference[0] != '\0' && reference[1] == ':');
    if (absolute) {
        const int written =
            std::snprintf(output, capacity, "%s", reference);
        return written > 0 && static_cast<u32>(written) < capacity;
    }
    const char* last_slash = std::strrchr(scene_path, '/');
    const char* last_backslash = std::strrchr(scene_path, '\\');
    const char* separator =
        last_slash != nullptr && (last_backslash == nullptr
                                  || last_slash > last_backslash)
            ? last_slash : last_backslash;
    const usize directory_size =
        separator != nullptr ? static_cast<usize>(separator - scene_path + 1)
                             : 0u;
    usize reference_size = 0u;
    while (reference[reference_size] != '\0') ++reference_size;
    if (directory_size + reference_size + 1u > capacity) return false;
    if (directory_size > 0u)
        std::memcpy(output, scene_path, directory_size);
    std::memcpy(output + directory_size, reference, reference_size + 1u);
    return true;
}

EScene3DSerializeError LoadLooseDependencies(const char* scene_path, FParsedScene3DDocument& document, u32& dependencies_loaded) noexcept {
    /** 初出順に集約した loose dependency 群。 */
    TArray<FSceneDependencyRecord> Dependencies;
    /** dependency 収集結果。 */
    const EScene3DSerializeError OrderError = BuildStableSceneDependencyOrder(document, false, Dependencies);
    if (OrderError != EScene3DSerializeError::None) return OrderError;

    TArray<byte> bytes;
    char resolved[2048]{};
    /** 初出順に読み込む dependency。 */
    for (const FSceneDependencyRecord& Dependency : Dependencies) {
        if (!ResolveLooseDependencyPath(scene_path, Dependency.Path, resolved, static_cast<u32>(sizeof(resolved)))) {
            return EScene3DSerializeError::AssetPathInvalid;
        }
        /** dependency 種別ごとの最大 payload byte 数。 */
        const u64 MaxBytes = Dependency.Kind == ESceneDependencyKind::Mesh ? kScene3DAssetMaxBytes : static_cast<u64>(kMaterial2DMaxTextBytes);
        /** loose file の read 結果。 */
        const EScene3DSerializeError ReadError = ReadLooseFile(resolved, MaxBytes, bytes);
        if (ReadError != EScene3DSerializeError::None)
            return ReadError;
        /** dependency の decode と node 割り当て結果。 */
        const EScene3DSerializeError DecodeError = DecodeAndAssignSceneDependency(document, Dependency, bytes, dependencies_loaded);
        if (DecodeError != EScene3DSerializeError::None)
            return DecodeError;
        bytes.Clear();
    }
    return EScene3DSerializeError::None;
}

} // namespace

FScene3DLoadResult TryLoadScene3DText(
    CSceneNodeGraph& graph, const char* text, u32 size) noexcept {
    FParsedScene3DDocument document;
    const FScene3DLoadResult parsed =
        ParseScene3DDocument(text, size, document);
    if (!parsed.Succeeded()) return parsed;
    return CommitScene3DDocument(graph, document, 0u);
}


FScene3DLoadResult TryLoadScene3DFile(
    CSceneNodeGraph& graph, const char* path) noexcept {
    if (path == nullptr)
        return LoadFailure(
            EScene3DSerializeError::NullInput, 0u, 0u, 0u, 0u);
    TArray<byte> bytes;
    const EScene3DSerializeError read_error =
        ReadLooseFile(path, kScene3DSerializeMaxInputBytes, bytes);
    if (read_error != EScene3DSerializeError::None)
        return LoadFailure(read_error, 0u, 0u, 0u, 0u);
    FParsedScene3DDocument document;
    const FScene3DLoadResult parsed =
        ParseScene3DDocument(
            reinterpret_cast<const char*>(bytes.Data()),
            static_cast<u32>(bytes.Size()), document);
    if (!parsed.Succeeded()) return parsed;
    u32 dependencies_loaded = 0u;
    const EScene3DSerializeError dependency_error =
        LoadLooseDependencies(path, document, dependencies_loaded);
    if (dependency_error != EScene3DSerializeError::None) {
        return LoadFailure(
            dependency_error, static_cast<u32>(bytes.Size()), 0u,
            parsed.NodeCount, parsed.MeshPathCount);
    }
    return CommitScene3DDocument(
        graph, document, dependencies_loaded);
}


FScene3DLoadResult TryLoadScene3DAssetPack(
    CSceneNodeGraph& graph, IAssetPackReader& pack,
    const char* virtual_path) noexcept {
    if (!IsSafeVirtualAssetPath(virtual_path)) {
        return LoadFailure(
            virtual_path == nullptr
                ? EScene3DSerializeError::NullInput
                : EScene3DSerializeError::AssetPathInvalid,
            0u, 0u, 0u, 0u);
    }
    const auto size_result = pack.FileSize(virtual_path);
    if (size_result.IsErr())
        return LoadFailure(
            EScene3DSerializeError::FileOpenFailed, 0u, 0u, 0u, 0u);
    if (size_result.Value() > kScene3DSerializeMaxInputBytes)
        return LoadFailure(
            EScene3DSerializeError::FileSizeLimitExceeded,
            0u, 0u, 0u, 0u);
    TArray<byte> bytes;
    if (!ReadPackEntry(
            pack, virtual_path, kScene3DSerializeMaxInputBytes, bytes)) {
        return LoadFailure(
            EScene3DSerializeError::FileReadFailed, 0u, 0u, 0u, 0u);
    }
    FParsedScene3DDocument document;
    const FScene3DLoadResult parsed =
        ParseScene3DDocument(
            reinterpret_cast<const char*>(bytes.Data()),
            static_cast<u32>(bytes.Size()), document);
    if (!parsed.Succeeded()) return parsed;
    u32 dependencies_loaded = 0u;
    const EScene3DSerializeError dependency_error =
        LoadPackDependencies(pack, document, dependencies_loaded);
    if (dependency_error != EScene3DSerializeError::None) {
        return LoadFailure(
            dependency_error, static_cast<u32>(bytes.Size()), 0u,
            parsed.NodeCount, parsed.MeshPathCount);
    }
    return CommitScene3DDocument(
        graph, document, dependencies_loaded);
}


bool LoadScene3DText(CSceneNodeGraph& graph, const char* text) noexcept {
    if (text == nullptr) return false;
    u32 size = 0u;
    while (size <= kScene3DSerializeMaxInputBytes && text[size] != '\0') ++size;
    if (size > kScene3DSerializeMaxInputBytes) return false;
    return TryLoadScene3DText(graph, text, size).Succeeded();
}


} // namespace acs::game
