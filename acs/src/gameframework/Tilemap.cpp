// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar Q — FTilemap 実装
#include "gameframework/Tilemap.h"
#include "container/Json.h"
#include "foundation/Error.h"
#include "math/Math.h"

namespace acs::game {

TResult<void> FTilemap::LoadTiledJson(const char* json_text, usize len) noexcept {
    const auto pr = ParseJson(json_text, len);
    if (pr.IsErr()) return pr.Error();
    const FJsonValue& root = pr.Value();

    const u32 w  = root.Get("width").AsU32();
    const u32 h  = root.Get("height").AsU32();
    const f32 ts = root.Get("tilewidth").AsF32(16.0f);
    if (w == 0 || h == 0) return ACS_ERR(Generic, 1420, "Tiled JSON: width/height is 0");

    const FJsonValue& layers = root.Get("layers");
    if (!layers.IsArray()) return ACS_ERR(Generic, 1421, "Tiled JSON: 'layers' is not an array");

    // tilelayer のみ数える。
    const FStringView kTileLayer("tilelayer");
    u32 tile_layer_count = 0;
    for (u32 i = 0; i < layers.Size(); ++i) {
        if (layers.At(i).Get("type").AsString() == kTileLayer) ++tile_layer_count;
    }
    if (tile_layer_count == 0) return ACS_ERR(Generic, 1422, "Tiled JSON: no tilelayer found");

    Init(w, h, tile_layer_count, ts);

    u32 layer_idx = 0;
    for (u32 i = 0; i < layers.Size(); ++i) {
        const FJsonValue& L = layers.At(i);
        if (L.Get("type").AsString() != kTileLayer) continue;
        const FJsonValue& data = L.Get("data");
        if (data.IsArray()) {
            const u32 cap = w * h;
            const u32 n   = data.Size();
            const u32 lim = n < cap ? n : cap;
            for (u32 k = 0; k < lim; ++k) {
                u32 gid = data.At(k).AsU32() & 0x1FFFFFFFu;   // 上位 flip フラグ除去
                if (gid > 0xFFFFu) gid = 0xFFFFu;             // FTileId(u16) に clamp
                SetTile(k % w, k / w, FTileId(static_cast<u16>(gid)), layer_idx);
            }
        }
        ++layer_idx;
    }
    return Ok();
}

void FTilemap::Init(u32 width, u32 height, u32 layer_count, f32 tile_size) noexcept {
    // 不正値はサイレントに安全な既定にフォールバック (Init を呼んだのに
    // ゼロサイズで沈黙、より、最小サイズで動かしておきデバッグ可能にする)。
    if (width  == 0) width  = 1;
    if (height == 0) height = 1;
    if (layer_count == 0) layer_count = 1;
    if (!(tile_size > 0.0f)) tile_size = 16.0f;   // NaN / 負値も弾く

    m_Width     = width;
    m_Height    = height;
    m_TileSize = tile_size;

    // 旧レイヤーを破棄してから layer_count 個に作り直す
    m_Layers.Clear();
    m_Layers.Resize(layer_count);

    const usize cells = static_cast<usize>(width) * static_cast<usize>(height);
    for (u32 L = 0; L < layer_count; ++L) {
        m_Layers[L].Resize(cells);                 // FTileId は trivially-constructible → 0 埋め
    }
}

void FTilemap::SetTile(u32 x, u32 y, FTileId tile, u32 layer) noexcept {
    if (x >= m_Width || y >= m_Height) return;
    if (layer >= m_Layers.Size()) return;
    m_Layers[layer][static_cast<usize>(y) * static_cast<usize>(m_Width) + static_cast<usize>(x)] = tile;
}

FTileId FTilemap::GetTile(u32 x, u32 y, u32 layer) const noexcept {
    if (x >= m_Width || y >= m_Height) return FTileId{};
    if (layer >= m_Layers.Size()) return FTileId{};
    return m_Layers[layer][static_cast<usize>(y) * static_cast<usize>(m_Width) + static_cast<usize>(x)];
}

void FTilemap::Fill(FTileId tile, u32 layer) noexcept {
    if (layer >= m_Layers.Size()) return;
    TArray<FTileId>& buf = m_Layers[layer];
    const usize n = buf.Size();
    for (usize i = 0; i < n; ++i) buf[i] = tile;
}

void FTilemap::FillRect(u32 x0, u32 y0, u32 x1, u32 y1, FTileId tile, u32 layer) noexcept {
    if (layer >= m_Layers.Size()) return;
    if (m_Width == 0 || m_Height == 0) return;

    // 反転していたら swap
    if (x0 > x1) { u32 t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { u32 t = y0; y0 = y1; y1 = t; }

    // グリッド境界で clamp (閉区間)
    const u32 max_x = m_Width  - 1u;
    const u32 max_y = m_Height - 1u;
    if (x0 > max_x) return;       // 矩形全体がマップ右外 → no-op
    if (y0 > max_y) return;       // 矩形全体がマップ下外 → no-op
    if (x1 > max_x) x1 = max_x;
    if (y1 > max_y) y1 = max_y;

    TArray<FTileId>& buf = m_Layers[layer];
    for (u32 y = y0; y <= y1; ++y) {
        const usize row = static_cast<usize>(y) * static_cast<usize>(m_Width);
        for (u32 x = x0; x <= x1; ++x) {
            buf[row + static_cast<usize>(x)] = tile;
        }
    }
}

void FTilemap::Clear() noexcept {
    const u32 layer_count = static_cast<u32>(m_Layers.Size());
    for (u32 L = 0; L < layer_count; ++L) {
        TArray<FTileId>& buf = m_Layers[L];
        const usize n = buf.Size();
        for (usize i = 0; i < n; ++i) buf[i] = FTileId{};
    }
}

FVec2 FTilemap::TileToWorld(u32 x, u32 y) const noexcept {
    // tile の **中心** world 位置。原点を tile(0,0) の中心に置く慣習。
    // +0.5 オフセットで grid line ではなく cell centroid を返す。
    const f32 fx = (static_cast<f32>(x) + 0.5f) * m_TileSize;
    const f32 fy = (static_cast<f32>(y) + 0.5f) * m_TileSize;
    return FVec2{fx, fy};
}

bool FTilemap::WorldToTile(FVec2 world, u32& out_x, u32& out_y) const noexcept {
    if (!(m_TileSize > 0.0f)) return false;
    // 早期 reject: 負値や原点未満 (TileToWorld は半セルオフセットなので
    // world.x < 0 は確実にグリッド外、x = 0 〜 width*tile_size を有効範囲とする)。
    if (world.x < 0.0f || world.y < 0.0f) return false;

    const f32 fx = Floor(world.x / m_TileSize);
    const f32 fy = Floor(world.y / m_TileSize);
    // f32 → u32 cast は範囲外で UB。floor 結果が負でないことは上で確認済み。
    // 上限を u32 max でクランプチェックしてから cast。
    if (fx >= static_cast<f32>(m_Width))  return false;
    if (fy >= static_cast<f32>(m_Height)) return false;

    out_x = static_cast<u32>(fx);
    out_y = static_cast<u32>(fy);
    return true;
}

const FTileId* FTilemap::LayerData(u32 layer) const noexcept {
    if (layer >= m_Layers.Size()) return nullptr;
    return m_Layers[layer].Data();
}

} // namespace acs::game
