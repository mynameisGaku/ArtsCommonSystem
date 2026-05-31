// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar Q — FTilemap (2D タイルマップ data structure)
//
// 2D グリッド上に `FTileId` を並べる data-only コンテナ。レイヤー対応
// (背景 / フォアグラウンド / コリジョン用などをそれぞれ別 grid として
// 持てる)、tile↔world 座標変換、Fill / FillRect ユーティリティを提供する。
//
// 使い方:
//   FTilemap map;
//   map.Init(/*width=*/64, /*height=*/48, /*layer_count=*/2, /*tile_size=*/16.0f);
//
//   map.Fill(FTileId{1}, /*layer=*/0);          // layer 0 を tile 1 で埋める
//   map.SetTile(10, 5, FTileId{2}, /*layer=*/0); // 個別タイル設定
//   map.FillRect(0, 0, 4, 4, FTileId{3}, 1);     // layer 1 に 4x4 矩形塗り
//
//   // 描画ループ
//   const FTileId* layer0 = map.LayerData(0);
//   for (u32 y = 0; y < map.Height(); ++y) {
//       for (u32 x = 0; x < map.Width(); ++x) {
//           FTileId t = layer0[y * map.Width() + x];
//           if (t.IsEmpty()) continue;
//           FVec2 wpos = map.TileToWorld(x, y);
//           // DrawSprite(t, wpos)...
//       }
//   }
//
//   // hit-test (例: マウス座標が tile のどれを指しているか)
//   u32 tx, ty;
//   if (map.WorldToTile(mouse_world, tx, ty)) {
//       FTileId hovered = map.GetTile(tx, ty, 0);
//   }
//
// 設計 (Phase 1 = Pillar Q v1):
//   ・**FTileId = u16**: 65535 種類のタイル ID を許容。0 = 空 (背景透過扱い)。
//     描画側が atlas index として解釈するか辞書 lookup するかは利用者責任。
//   ・**レイヤー = 独立した TArray<FTileId>**: layer 数 N に対して N 本の
//     row-major (`y * width + x`) フラット配列。layer 0 が最背面、
//     layer_count-1 が最前面という慣習だが順序は描画側で自由に決めて良い。
//     `LayerData(L)` で生ポインタを返すので GPU upload / tile renderer
//     から直接舐められる。
//   ・**tile_size**: world unit / tile (典型的に px = world unit のとき 16, 32)。
//     座標変換は `TileToWorld` が tile (x,y) の **中心** world 位置 (一致しやすい
//     スプライト描画基準)。
//   ・**WorldToTile**: world.x / tile_size を floor。範囲外 (負値含む) は
//     false を返す。usize→u32 cast の安全性のため負値は早期 reject。
//   ・**非コピー・非ムーブ**: シーン所有 / TPool 経由想定。複製したい場合は
//     利用者側で明示的に Clone (今は提供しない)。
//   ・**全 noexcept / STL 不使用 / acs::TArray のみ**: 規約準拠。
//
// 範囲外 (将来拡張):
//   ・auto-tiling / wang tiles の lookup table。
//   ・per-tile flags (flip x/y, rotate90, collision-type) ─ 必要なら別配列で
//     追加して FTileId 自体は純粋 ID のままにする方針。
//   ・スパース / chunk 化 (巨大マップ向け)。
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs::game {

// 1 セル = 1 個の FTileId。0 を「空 (no tile)」として予約。
struct FTileId {
    u16 value = 0;
    constexpr FTileId() noexcept = default;
    constexpr explicit FTileId(u16 v) noexcept : value(v) {}
    constexpr bool IsEmpty() const noexcept { return value == 0; }
    constexpr bool operator==(FTileId o) const noexcept { return value == o.value; }
    constexpr bool operator!=(FTileId o) const noexcept { return value != o.value; }
};

class FTilemap {
public:
    FTilemap() noexcept = default;
    ~FTilemap() noexcept = default;

    // 非コピー・非ムーブ
    FTilemap(const FTilemap&)            = delete;
    FTilemap& operator=(const FTilemap&) = delete;
    FTilemap(FTilemap&&)                 = delete;
    FTilemap& operator=(FTilemap&&)      = delete;

    // グリッドを (width x height) で初期化、`layer_count` レイヤー分の
    // バッファを確保し全 tile を 0 (空) でゼロクリア。`tile_size` は world
    // 単位での 1 tile の辺長 (典型 16.0f / 32.0f)。
    // 不正値 (0) は安全な既定 (width/height は 1、layer_count は 1、
    // tile_size は 16.0f) にフォールバック。
    void Init(u32 width, u32 height, u32 layer_count = 1, f32 tile_size = 16.0f) noexcept;

    // ----- Tiled (.tmj/.json) ローダ (content pipeline) -----
    // Tiled Map Editor の JSON マップを読み込む。map の width/height/tilewidth と
    // "layers" 配列中の各 "tilelayer" の "data" (row-major GID 配列) を取り込む。
    //   ・tilelayer のみ対象 (objectgroup 等は無視)。GID 上位 3bit の flip フラグは
    //     除去し、FTileId(u16) に clamp する (GID 0 = 空)。
    //   ・data[k] を (x = k%w, y = k/w) に配置 (Tiled の行順)。
    // 解析失敗 / width=0 / tilelayer 欠如は ACS_ERR。Init をこの中で呼ぶ。
    TResult<void> LoadTiledJson(const char* json_text, usize len) noexcept;

    // 個別タイル設定。範囲外 (x / y / layer) は no-op。
    void SetTile(u32 x, u32 y, FTileId tile, u32 layer = 0) noexcept;

    // 個別タイル取得。範囲外なら空 FTileId{0}。
    FTileId GetTile(u32 x, u32 y, u32 layer = 0) const noexcept;

    // 指定レイヤー全体を tile で埋める。範囲外 layer は no-op。
    void Fill(FTileId tile, u32 layer = 0) noexcept;

    // 半開区間ではなく **閉区間** [x0..x1] x [y0..y1] を塗る。
    // x0 > x1 / y0 > y1 は swap 扱い、グリッド境界で clamp。
    // 範囲外 layer は no-op。
    void FillRect(u32 x0, u32 y0, u32 x1, u32 y1, FTileId tile, u32 layer = 0) noexcept;

    // 全レイヤーを 0 (空) で埋める (サイズは保持)。
    void Clear() noexcept;

    u32 Width()      const noexcept { return m_Width; }
    u32 Height()     const noexcept { return m_Height; }
    u32 LayerCount() const noexcept { return static_cast<u32>(m_Layers.Size()); }
    f32 TileSize()   const noexcept { return m_TileSize; }

    // tile (x,y) の **中心** world 座標。範囲外 (x>=width / y>=height) でも
    // 計算式そのものを返す (デバッグ用に左上原点で連続を期待する利用者が
    // 居るため明示的にチェックしない。+Y=画面下なので row 0 が画面上端)。
    FVec2 TileToWorld(u32 x, u32 y) const noexcept;

    // world → tile。範囲外 (グリッド外 / 負値) は false。
    // 成功時のみ out_x / out_y を書き込む。
    bool WorldToTile(FVec2 world, u32& out_x, u32& out_y) const noexcept;

    // 描画用 raw データ。size = Width() * Height()、layout = row-major
    // (`y * width + x`)。範囲外 layer は nullptr。
    const FTileId* LayerData(u32 layer) const noexcept;

private:
    TArray<TArray<FTileId>> m_Layers {};      // m_Layers[L] = row-major (w*h)
    u32                  m_Width      = 0;
    u32                  m_Height     = 0;
    f32                  m_TileSize  = 16.0f;
};

} // namespace acs::game
