// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar Q — Tilemap 実装
#include "gameframework/Tilemap.h"
#include "math/Math.h"

namespace acs::game {

void Tilemap::Init(u32 width, u32 height, u32 layer_count, f32 tile_size) noexcept {
    // 不正値はサイレントに安全な既定にフォールバック (Init を呼んだのに
    // ゼロサイズで沈黙、より、最小サイズで動かしておきデバッグ可能にする)。
    if (width  == 0) width  = 1;
    if (height == 0) height = 1;
    if (layer_count == 0) layer_count = 1;
    if (!(tile_size > 0.0f)) tile_size = 16.0f;   // NaN / 負値も弾く

    _width     = width;
    _height    = height;
    _tile_size = tile_size;

    // 旧レイヤーを破棄してから layer_count 個に作り直す
    _layers.Clear();
    _layers.Resize(layer_count);

    const usize cells = static_cast<usize>(width) * static_cast<usize>(height);
    for (u32 L = 0; L < layer_count; ++L) {
        _layers[L].Resize(cells);                 // TileId は trivially-constructible → 0 埋め
    }
}

void Tilemap::SetTile(u32 x, u32 y, TileId tile, u32 layer) noexcept {
    if (x >= _width || y >= _height) return;
    if (layer >= _layers.Size()) return;
    _layers[layer][static_cast<usize>(y) * static_cast<usize>(_width) + static_cast<usize>(x)] = tile;
}

TileId Tilemap::GetTile(u32 x, u32 y, u32 layer) const noexcept {
    if (x >= _width || y >= _height) return TileId{};
    if (layer >= _layers.Size()) return TileId{};
    return _layers[layer][static_cast<usize>(y) * static_cast<usize>(_width) + static_cast<usize>(x)];
}

void Tilemap::Fill(TileId tile, u32 layer) noexcept {
    if (layer >= _layers.Size()) return;
    TArray<TileId>& buf = _layers[layer];
    const usize n = buf.Size();
    for (usize i = 0; i < n; ++i) buf[i] = tile;
}

void Tilemap::FillRect(u32 x0, u32 y0, u32 x1, u32 y1, TileId tile, u32 layer) noexcept {
    if (layer >= _layers.Size()) return;
    if (_width == 0 || _height == 0) return;

    // 反転していたら swap
    if (x0 > x1) { u32 t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { u32 t = y0; y0 = y1; y1 = t; }

    // グリッド境界で clamp (閉区間)
    const u32 max_x = _width  - 1u;
    const u32 max_y = _height - 1u;
    if (x0 > max_x) return;       // 矩形全体がマップ右外 → no-op
    if (y0 > max_y) return;       // 矩形全体がマップ下外 → no-op
    if (x1 > max_x) x1 = max_x;
    if (y1 > max_y) y1 = max_y;

    TArray<TileId>& buf = _layers[layer];
    for (u32 y = y0; y <= y1; ++y) {
        const usize row = static_cast<usize>(y) * static_cast<usize>(_width);
        for (u32 x = x0; x <= x1; ++x) {
            buf[row + static_cast<usize>(x)] = tile;
        }
    }
}

void Tilemap::Clear() noexcept {
    const u32 layer_count = static_cast<u32>(_layers.Size());
    for (u32 L = 0; L < layer_count; ++L) {
        TArray<TileId>& buf = _layers[L];
        const usize n = buf.Size();
        for (usize i = 0; i < n; ++i) buf[i] = TileId{};
    }
}

FVec2 Tilemap::TileToWorld(u32 x, u32 y) const noexcept {
    // tile の **中心** world 位置。原点を tile(0,0) の中心に置く慣習。
    // +0.5 オフセットで grid line ではなく cell centroid を返す。
    const f32 fx = (static_cast<f32>(x) + 0.5f) * _tile_size;
    const f32 fy = (static_cast<f32>(y) + 0.5f) * _tile_size;
    return FVec2{fx, fy};
}

bool Tilemap::WorldToTile(FVec2 world, u32& out_x, u32& out_y) const noexcept {
    if (!(_tile_size > 0.0f)) return false;
    // 早期 reject: 負値や原点未満 (TileToWorld は半セルオフセットなので
    // world.x < 0 は確実にグリッド外、x = 0 〜 width*tile_size を有効範囲とする)。
    if (world.x < 0.0f || world.y < 0.0f) return false;

    const f32 fx = Floor(world.x / _tile_size);
    const f32 fy = Floor(world.y / _tile_size);
    // f32 → u32 cast は範囲外で UB。floor 結果が負でないことは上で確認済み。
    // 上限を u32 max でクランプチェックしてから cast。
    if (fx >= static_cast<f32>(_width))  return false;
    if (fy >= static_cast<f32>(_height)) return false;

    out_x = static_cast<u32>(fx);
    out_y = static_cast<u32>(fy);
    return true;
}

const TileId* Tilemap::LayerData(u32 layer) const noexcept {
    if (layer >= _layers.Size()) return nullptr;
    return _layers[layer].Data();
}

} // namespace acs::game
