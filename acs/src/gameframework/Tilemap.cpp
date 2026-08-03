// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar Q — FTilemap 実装と Tiled JSON 境界。
#include "gameframework/Tilemap.h"

#include "container/Json.h"
#include "foundation/Error.h"
#include "foundation/Move.h"
#include "math/Math.h"

#include <cfloat>

namespace acs::game {

namespace {

FTilemapLoadResult TileFailure(
    ETilemapLoadError error, u32 layer = 0u, u32 element = 0u,
    u16 json_subcode = 0u) noexcept {
    FTilemapLoadResult result{};
    result.Error = error;
    result.JsonSubcode = json_subcode;
    result.Layer = layer;
    result.Element = element;
    return result;
}

ETilemapLoadError ValidateEnvelope(
    const char* text, usize length) noexcept {
    if (text == nullptr) return ETilemapLoadError::NullInput;
    if (length == 0u) return ETilemapLoadError::EmptyInput;
    if (length > FTilemap::kMaxTiledJsonBytes) {
        return ETilemapLoadError::InputTooLarge;
    }

    u32 depth = 0u;
    usize string_length = 0u;
    bool in_string = false;
    bool escaped = false;
    for (usize i = 0u; i < length; ++i) {
        const char byte = text[i];
        if (byte == '\0') return ETilemapLoadError::EmbeddedNul;
        if (in_string) {
            if (escaped) {
                escaped = false;
                ++string_length;
                if (string_length > FTilemap::kMaxJsonStringBytes) {
                    return ETilemapLoadError::JsonStringTooLong;
                }
                continue;
            }
            if (byte == '\\') {
                escaped = true;
                ++string_length;
                if (string_length > FTilemap::kMaxJsonStringBytes) {
                    return ETilemapLoadError::JsonStringTooLong;
                }
                continue;
            }
            if (byte == '"') {
                in_string = false;
                continue;
            }
            ++string_length;
            if (string_length > FTilemap::kMaxJsonStringBytes) {
                return ETilemapLoadError::JsonStringTooLong;
            }
            continue;
        }
        if (byte == '"') {
            in_string = true;
            escaped = false;
            string_length = 0u;
        } else if (byte == '{' || byte == '[') {
            ++depth;
            if (depth > FTilemap::kMaxJsonDepth) {
                return ETilemapLoadError::JsonDepthExceeded;
            }
        } else if ((byte == '}' || byte == ']') && depth != 0u) {
            --depth;
        }
    }
    return ETilemapLoadError::None;
}

ETilemapLoadError ValidateDom(
    const FJsonValue& value, u32 depth, u32& node_count) noexcept {
    if (depth > FTilemap::kMaxJsonDepth) {
        return ETilemapLoadError::JsonDepthExceeded;
    }
    if (node_count == FTilemap::kMaxJsonNodes) {
        return ETilemapLoadError::JsonNodeLimitExceeded;
    }
    ++node_count;

    if (value.IsNumber()) {
        const f64 number = value.AsNumber();
        if (!(number == number) || number > DBL_MAX || number < -DBL_MAX) {
            return ETilemapLoadError::NonFiniteNumber;
        }
    } else if (value.IsString()) {
        const FStringView text = value.AsString();
        if (text.Size() > FTilemap::kMaxJsonStringBytes) {
            return ETilemapLoadError::JsonStringTooLong;
        }
        for (usize i = 0u; i < text.Size(); ++i) {
            if (text.Data()[i] == '\0') {
                return ETilemapLoadError::EmbeddedNul;
            }
        }
    } else if (value.IsObject()) {
        const u32 members = value.MemberCount();
        if (members > FTilemap::kMaxJsonObjectMembers) {
            return ETilemapLoadError::JsonNodeLimitExceeded;
        }
        for (u32 i = 0u; i < members; ++i) {
            const FStringView key = value.MemberKey(i);
            if (key.Size() > FTilemap::kMaxJsonStringBytes) {
                return ETilemapLoadError::JsonStringTooLong;
            }
            for (usize byte = 0u; byte < key.Size(); ++byte) {
                if (key.Data()[byte] == '\0') {
                    return ETilemapLoadError::EmbeddedNul;
                }
            }
            for (u32 prior = 0u; prior < i; ++prior) {
                if (key == value.MemberKey(prior)) {
                    return ETilemapLoadError::DuplicateMember;
                }
            }
            const ETilemapLoadError child_error =
                ValidateDom(value.At(i), depth + 1u, node_count);
            if (child_error != ETilemapLoadError::None) return child_error;
        }
    } else if (value.IsArray()) {
        const u32 elements = value.Size();
        for (u32 i = 0u; i < elements; ++i) {
            const ETilemapLoadError child_error =
                ValidateDom(value.At(i), depth + 1u, node_count);
            if (child_error != ETilemapLoadError::None) return child_error;
        }
    }
    return ETilemapLoadError::None;
}

bool TryReadU32(const FJsonValue& value, u32& output) noexcept {
    if (!value.IsNumber()) return false;
    const f64 number = value.AsNumber();
    if (!(number == number) || number < 0.0 || number > 4294967295.0) {
        return false;
    }
    const u32 converted = static_cast<u32>(number);
    if (static_cast<f64>(converted) != number) return false;
    output = converted;
    return true;
}

bool TryReadPositiveF32(const FJsonValue& value, f32& output) noexcept {
    if (!value.IsNumber()) return false;
    const f64 number = value.AsNumber();
    if (!(number == number) || number <= 0.0 ||
        number > static_cast<f64>(FLT_MAX)) {
        return false;
    }
    output = static_cast<f32>(number);
    return output > 0.0f && output == output;
}

} // namespace

const char* TilemapLoadErrorName(ETilemapLoadError error) noexcept {
    switch (error) {
        case ETilemapLoadError::None: return "None";
        case ETilemapLoadError::NullInput: return "NullInput";
        case ETilemapLoadError::EmptyInput: return "EmptyInput";
        case ETilemapLoadError::InputTooLarge: return "InputTooLarge";
        case ETilemapLoadError::EmbeddedNul: return "EmbeddedNul";
        case ETilemapLoadError::JsonDepthExceeded: return "JsonDepthExceeded";
        case ETilemapLoadError::JsonStringTooLong: return "JsonStringTooLong";
        case ETilemapLoadError::JsonNodeLimitExceeded: return "JsonNodeLimitExceeded";
        case ETilemapLoadError::JsonSyntaxError: return "JsonSyntaxError";
        case ETilemapLoadError::RootTypeMismatch: return "RootTypeMismatch";
        case ETilemapLoadError::DuplicateMember: return "DuplicateMember";
        case ETilemapLoadError::MissingMember: return "MissingMember";
        case ETilemapLoadError::MemberTypeMismatch: return "MemberTypeMismatch";
        case ETilemapLoadError::InvalidInteger: return "InvalidInteger";
        case ETilemapLoadError::NonFiniteNumber: return "NonFiniteNumber";
        case ETilemapLoadError::InvalidDimensions: return "InvalidDimensions";
        case ETilemapLoadError::DimensionLimitExceeded: return "DimensionLimitExceeded";
        case ETilemapLoadError::CellCountOverflow: return "CellCountOverflow";
        case ETilemapLoadError::LayerLimitExceeded: return "LayerLimitExceeded";
        case ETilemapLoadError::MissingTileLayer: return "MissingTileLayer";
        case ETilemapLoadError::DataLengthMismatch: return "DataLengthMismatch";
        case ETilemapLoadError::AllocationFailure: return "AllocationFailure";
    }
    return "Unknown";
}

FTilemapLoadResult FTilemap::TryInit(
    u32 width, u32 height, u32 layer_count, f32 tile_size) noexcept {
    if (width == 0u || height == 0u || layer_count == 0u ||
        !(tile_size > 0.0f) || !(tile_size <= FLT_MAX)) {
        return TileFailure(ETilemapLoadError::InvalidDimensions);
    }
    if (width > kMaxMapDimension || height > kMaxMapDimension) {
        return TileFailure(ETilemapLoadError::DimensionLimitExceeded);
    }
    if (layer_count > kMaxTileLayers) {
        return TileFailure(ETilemapLoadError::LayerLimitExceeded);
    }
    if (height > kMaxCellsPerLayer / static_cast<usize>(width)) {
        return TileFailure(ETilemapLoadError::CellCountOverflow);
    }
    const usize cells =
        static_cast<usize>(width) * static_cast<usize>(height);
    if (cells > kMaxCellsPerLayer ||
        layer_count > kMaxTotalCells / cells) {
        return TileFailure(ETilemapLoadError::CellCountOverflow);
    }

    TArray<TArray<FTileId>> staged_layers(*m_Layers.GetAllocator());
    if (!staged_layers.TryReserve(layer_count)) {
        return TileFailure(ETilemapLoadError::AllocationFailure);
    }
    for (u32 layer = 0u; layer < layer_count; ++layer) {
        TArray<FTileId> staged_layer(*m_Layers.GetAllocator());
        if (!staged_layer.TrySetNum(cells) ||
            !staged_layers.TryAdd(Move(staged_layer))) {
            return TileFailure(
                ETilemapLoadError::AllocationFailure, layer);
        }
    }

    m_Layers = Move(staged_layers);
    m_Width = width;
    m_Height = height;
    m_TileSize = tile_size;
    return FTilemapLoadResult{};
}

void FTilemap::Init(
    u32 width, u32 height, u32 layer_count, f32 tile_size) noexcept {
    if (width == 0u) width = 1u;
    if (height == 0u) height = 1u;
    if (layer_count == 0u) layer_count = 1u;
    if (!(tile_size > 0.0f) || !(tile_size <= FLT_MAX)) tile_size = 16.0f;
    (void)TryInit(width, height, layer_count, tile_size);
}

FTilemapLoadResult FTilemap::TryLoadTiledJson(
    const char* json_text, usize len) noexcept {
    const ETilemapLoadError envelope_error = ValidateEnvelope(json_text, len);
    if (envelope_error != ETilemapLoadError::None) {
        return TileFailure(envelope_error);
    }

    auto parsed = ParseJson(json_text, len);
    if (parsed.IsErr()) {
        return TileFailure(
            ETilemapLoadError::JsonSyntaxError, 0u, 0u,
            parsed.Error().subcode);
    }
    const FJsonValue& root = parsed.Value();
    u32 node_count = 0u;
    const ETilemapLoadError dom_error = ValidateDom(root, 0u, node_count);
    if (dom_error != ETilemapLoadError::None) return TileFailure(dom_error);
    if (!root.IsObject()) {
        return TileFailure(ETilemapLoadError::RootTypeMismatch);
    }

    const FJsonValue* width_value = root.Find("width");
    const FJsonValue* height_value = root.Find("height");
    const FJsonValue* tile_width_value = root.Find("tilewidth");
    const FJsonValue* layers = root.Find("layers");
    if (width_value == nullptr || height_value == nullptr ||
        tile_width_value == nullptr || layers == nullptr) {
        return TileFailure(ETilemapLoadError::MissingMember);
    }

    u32 width = 0u;
    u32 height = 0u;
    f32 tile_size = 0.0f;
    if (!width_value->IsNumber() || !height_value->IsNumber()) {
        return TileFailure(ETilemapLoadError::MemberTypeMismatch);
    }
    if (!TryReadU32(*width_value, width) ||
        !TryReadU32(*height_value, height)) {
        return TileFailure(ETilemapLoadError::InvalidInteger);
    }
    if (!tile_width_value->IsNumber()) {
        return TileFailure(ETilemapLoadError::MemberTypeMismatch);
    }
    if (!TryReadPositiveF32(*tile_width_value, tile_size)) {
        return TileFailure(ETilemapLoadError::InvalidDimensions);
    }
    const FJsonValue* tile_height_value = root.Find("tileheight");
    if (tile_height_value != nullptr) {
        f32 tile_height = 0.0f;
        if (!tile_height_value->IsNumber()) {
            return TileFailure(ETilemapLoadError::MemberTypeMismatch);
        }
        if (!TryReadPositiveF32(*tile_height_value, tile_height) ||
            tile_height != tile_size) {
            return TileFailure(ETilemapLoadError::InvalidDimensions);
        }
    }
    if (!layers->IsArray()) {
        return TileFailure(ETilemapLoadError::MemberTypeMismatch);
    }
    if (layers->Size() > kMaxLayerRecords) {
        return TileFailure(ETilemapLoadError::LayerLimitExceeded);
    }
    if (width == 0u || height == 0u) {
        return TileFailure(ETilemapLoadError::InvalidDimensions);
    }
    if (width > kMaxMapDimension || height > kMaxMapDimension) {
        return TileFailure(ETilemapLoadError::DimensionLimitExceeded);
    }
    if (height > kMaxCellsPerLayer / static_cast<usize>(width)) {
        return TileFailure(ETilemapLoadError::CellCountOverflow);
    }
    const usize cells =
        static_cast<usize>(width) * static_cast<usize>(height);
    if (cells > kMaxCellsPerLayer) {
        return TileFailure(ETilemapLoadError::CellCountOverflow);
    }

    const FStringView tile_layer_type("tilelayer");
    u32 tile_layer_count = 0u;
    for (u32 index = 0u; index < layers->Size(); ++index) {
        const FJsonValue& layer = layers->At(index);
        if (!layer.IsObject()) {
            return TileFailure(
                ETilemapLoadError::MemberTypeMismatch, index);
        }
        const FJsonValue* type = layer.Find("type");
        if (type == nullptr) {
            return TileFailure(ETilemapLoadError::MissingMember, index);
        }
        if (!type->IsString()) {
            return TileFailure(
                ETilemapLoadError::MemberTypeMismatch, index);
        }
        if (type->AsString() != tile_layer_type) continue;
        if (tile_layer_count == kMaxTileLayers) {
            return TileFailure(
                ETilemapLoadError::LayerLimitExceeded, index);
        }
        const FJsonValue* data = layer.Find("data");
        if (data == nullptr) {
            return TileFailure(ETilemapLoadError::MissingMember, index);
        }
        if (!data->IsArray()) {
            return TileFailure(
                ETilemapLoadError::MemberTypeMismatch, index);
        }
        if (static_cast<usize>(data->Size()) != cells) {
            return TileFailure(
                ETilemapLoadError::DataLengthMismatch, index,
                data->Size());
        }
        const FJsonValue* layer_width = layer.Find("width");
        const FJsonValue* layer_height = layer.Find("height");
        if (layer_width != nullptr) {
            u32 value = 0u;
            if (!layer_width->IsNumber() ||
                !TryReadU32(*layer_width, value) || value != width) {
                return TileFailure(
                    ETilemapLoadError::InvalidDimensions, index);
            }
        }
        if (layer_height != nullptr) {
            u32 value = 0u;
            if (!layer_height->IsNumber() ||
                !TryReadU32(*layer_height, value) || value != height) {
                return TileFailure(
                    ETilemapLoadError::InvalidDimensions, index);
            }
        }
        for (u32 element = 0u; element < data->Size(); ++element) {
            u32 gid = 0u;
            if (!TryReadU32(data->At(element), gid)) {
                return TileFailure(
                    data->At(element).IsNumber()
                        ? ETilemapLoadError::InvalidInteger
                        : ETilemapLoadError::MemberTypeMismatch,
                    index, element);
            }
        }
        ++tile_layer_count;
    }
    if (tile_layer_count == 0u) {
        return TileFailure(ETilemapLoadError::MissingTileLayer);
    }
    if (tile_layer_count > kMaxTotalCells / cells) {
        return TileFailure(ETilemapLoadError::CellCountOverflow);
    }

    TArray<TArray<FTileId>> staged_layers(*m_Layers.GetAllocator());
    if (!staged_layers.TryReserve(tile_layer_count)) {
        return TileFailure(ETilemapLoadError::AllocationFailure);
    }
    for (u32 layer = 0u; layer < tile_layer_count; ++layer) {
        TArray<FTileId> staged_layer(*m_Layers.GetAllocator());
        if (!staged_layer.TrySetNum(cells) ||
            !staged_layers.TryAdd(Move(staged_layer))) {
            return TileFailure(
                ETilemapLoadError::AllocationFailure, layer);
        }
    }

    u32 output_layer = 0u;
    for (u32 index = 0u; index < layers->Size(); ++index) {
        const FJsonValue& layer = layers->At(index);
        if (layer.Get("type").AsString() != tile_layer_type) continue;
        const FJsonValue& data = layer.Get("data");
        for (u32 element = 0u; element < data.Size(); ++element) {
            u32 gid = 0u;
            (void)TryReadU32(data.At(element), gid);
            gid &= 0x1FFFFFFFu;
            if (gid > 0xFFFFu) gid = 0xFFFFu;
            staged_layers[output_layer][element] =
                FTileId(static_cast<u16>(gid));
        }
        ++output_layer;
    }

    m_Layers = Move(staged_layers);
    m_Width = width;
    m_Height = height;
    m_TileSize = tile_size;
    return FTilemapLoadResult{};
}

TResult<void> FTilemap::LoadTiledJson(
    const char* json_text, usize len) noexcept {
    const FTilemapLoadResult result = TryLoadTiledJson(json_text, len);
    if (result) return Ok();
    const EErrCategory category =
        result.Error == ETilemapLoadError::AllocationFailure
        ? EErrCategory::Memory
        : EErrCategory::Generic;
    return FErrorCode(
        category, static_cast<u16>(result.Error),
        TilemapLoadErrorName(result.Error), FSourceLoc::Current());
}

void FTilemap::SetTile(
    u32 x, u32 y, FTileId tile, u32 layer) noexcept {
    if (x >= m_Width || y >= m_Height || layer >= m_Layers.Num()) return;
    m_Layers[layer][
        static_cast<usize>(y) * static_cast<usize>(m_Width) + x] = tile;
}

FTileId FTilemap::GetTile(u32 x, u32 y, u32 layer) const noexcept {
    if (x >= m_Width || y >= m_Height || layer >= m_Layers.Num()) {
        return FTileId{};
    }
    return m_Layers[layer][
        static_cast<usize>(y) * static_cast<usize>(m_Width) + x];
}

void FTilemap::Fill(FTileId tile, u32 layer) noexcept {
    if (layer >= m_Layers.Num()) return;
    TArray<FTileId>& buffer = m_Layers[layer];
    for (usize i = 0u; i < buffer.Num(); ++i) buffer[i] = tile;
}

void FTilemap::FillRect(
    u32 x0, u32 y0, u32 x1, u32 y1, FTileId tile, u32 layer) noexcept {
    if (layer >= m_Layers.Num() || m_Width == 0u || m_Height == 0u) return;
    if (x0 > x1) { const u32 value = x0; x0 = x1; x1 = value; }
    if (y0 > y1) { const u32 value = y0; y0 = y1; y1 = value; }

    const u32 max_x = m_Width - 1u;
    const u32 max_y = m_Height - 1u;
    if (x0 > max_x || y0 > max_y) return;
    if (x1 > max_x) x1 = max_x;
    if (y1 > max_y) y1 = max_y;

    TArray<FTileId>& buffer = m_Layers[layer];
    for (u32 y = y0; y <= y1; ++y) {
        const usize row = static_cast<usize>(y) * m_Width;
        for (u32 x = x0; x <= x1; ++x) buffer[row + x] = tile;
    }
}

void FTilemap::Clear() noexcept {
    for (usize layer = 0u; layer < m_Layers.Num(); ++layer) {
        TArray<FTileId>& buffer = m_Layers[layer];
        for (usize i = 0u; i < buffer.Num(); ++i) buffer[i] = FTileId{};
    }
}

FVec2 FTilemap::TileToWorld(u32 x, u32 y) const noexcept {
    return FVec2{
        (static_cast<f32>(x) + 0.5f) * m_TileSize,
        (static_cast<f32>(y) + 0.5f) * m_TileSize};
}

bool FTilemap::WorldToTile(
    FVec2 world, u32& out_x, u32& out_y) const noexcept {
    if (!(m_TileSize > 0.0f) ||
        !(world.x == world.x) || !(world.y == world.y) ||
        world.x < 0.0f || world.y < 0.0f) {
        return false;
    }
    const f32 tile_x = Floor(world.x / m_TileSize);
    const f32 tile_y = Floor(world.y / m_TileSize);
    if (!(tile_x >= 0.0f) || !(tile_y >= 0.0f) ||
        tile_x >= static_cast<f32>(m_Width) ||
        tile_y >= static_cast<f32>(m_Height)) {
        return false;
    }
    out_x = static_cast<u32>(tile_x);
    out_y = static_cast<u32>(tile_y);
    return true;
}

const FTileId* FTilemap::LayerData(u32 layer) const noexcept {
    return layer < m_Layers.Num() ? m_Layers[layer].GetData() : nullptr;
}

} // namespace acs::game
