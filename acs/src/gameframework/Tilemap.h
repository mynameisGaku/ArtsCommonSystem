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
// 設計 (Pillar Q):
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

/**
 * 1 セルを表すタイル ID (u16)。0 を「空 (no tile)」として予約する。
 *
 * @details
 * 65535 種類までのタイルを許容し、描画側が atlas index として解釈するか辞書
 * lookup するかは利用者責任。0 は背景透過扱いの空セルとして予約される。
 */
struct FTileId {
    /** タイル ID 本体 (0 = 空セル)。 */
    u16 value = 0;

    /** 空 (value = 0) のタイル ID を構築する。 */
    constexpr FTileId() noexcept = default;

    /**
     * 指定 ID のタイルを構築する。
     *
     * @param v タイル ID (0 = 空)。
     */
    constexpr explicit FTileId(u16 v) noexcept : value(v) {}

    /**
     * 空セルかどうかを返す。
     *
     * @return value == 0 なら true。
     */
    constexpr bool IsEmpty() const noexcept { return value == 0; }

    /**
     * 2 つのタイル ID が等しいかを返す。
     *
     * @param o 比較対象のタイル ID。
     * @return value が一致すれば true。
     */
    constexpr bool operator==(FTileId o) const noexcept { return value == o.value; }

    /**
     * 2 つのタイル ID が異なるかを返す。
     *
     * @param o 比較対象のタイル ID。
     * @return value が異なれば true。
     */
    constexpr bool operator!=(FTileId o) const noexcept { return value != o.value; }
};

/** 検証付き tilemap 構築・読み込みが返す安定した失敗理由。 */
enum class ETilemapLoadError : u16 {
    None = 0,
    NullInput = 1420,
    EmptyInput = 1421,
    InputTooLarge = 1422,
    EmbeddedNul = 1423,
    JsonDepthExceeded = 1424,
    JsonStringTooLong = 1425,
    JsonNodeLimitExceeded = 1426,
    JsonSyntaxError = 1427,
    RootTypeMismatch = 1428,
    DuplicateMember = 1429,
    MissingMember = 1430,
    MemberTypeMismatch = 1431,
    InvalidInteger = 1432,
    NonFiniteNumber = 1433,
    InvalidDimensions = 1434,
    DimensionLimitExceeded = 1435,
    CellCountOverflow = 1436,
    LayerLimitExceeded = 1437,
    MissingTileLayer = 1438,
    DataLengthMismatch = 1439,
    AllocationFailure = 1440,
};

/** TryInit と TryLoadTiledJson が返す allocation-free の結果。 */
struct FTilemapLoadResult {
    ETilemapLoadError Error = ETilemapLoadError::None;
    u16 JsonSubcode = 0u;
    u32 Layer = 0u;
    u32 Element = 0u;

    bool Succeeded() const noexcept { return Error == ETilemapLoadError::None; }
    explicit operator bool() const noexcept { return Succeeded(); }
};

/** ETilemapLoadError に対応する安定した診断名。 */
const char* TilemapLoadErrorName(ETilemapLoadError error) noexcept;

/**
 * 2D グリッド上に FTileId を並べる data-only コンテナ (レイヤー対応)。
 *
 * @details
 * レイヤー数 N に対して N 本の row-major (y * width + x) フラット配列を持ち、
 * tile↔world 座標変換と Fill / FillRect ユーティリティを提供する。tile_size は
 * world 単位での 1 tile の辺長 (典型 16.0f / 32.0f)。シーン所有 / TPool 経由を
 * 想定した non-copy / non-move 型。
 */
class FTilemap {
public:
    static constexpr usize kMaxTiledJsonBytes = 8u * 1024u * 1024u;
    static constexpr u32 kMaxJsonDepth = 64u;
    static constexpr usize kMaxJsonStringBytes = 4096u;
    static constexpr u32 kMaxJsonNodes = 1100000u;
    static constexpr u32 kMaxJsonObjectMembers = 4096u;
    static constexpr u32 kMaxLayerRecords = 256u;
    static constexpr u32 kMaxMapDimension = 2048u;
    static constexpr usize kMaxCellsPerLayer = 262144u;
    static constexpr u32 kMaxTileLayers = 32u;
    static constexpr usize kMaxTotalCells = 1048576u;

    /** 空のタイルマップを構築する (グリッドは Init で確保)。 */
    FTilemap() noexcept = default;

    /** 外側の layer array に呼び出し側所有の allocator を使う。 */
    explicit FTilemap(IAllocator& allocator) noexcept : m_Layers(allocator) {}

    /** 破棄する (レイヤーバッファは TArray が解放)。 */
    ~FTilemap() noexcept = default;

    /** コピー禁止 (シーンで単独所有するため)。 */
    FTilemap(const FTilemap&)            = delete;

    /** コピー代入も禁止。 */
    FTilemap& operator=(const FTilemap&) = delete;

    /** ムーブ禁止。 */
    FTilemap(FTilemap&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FTilemap& operator=(FTilemap&&)      = delete;

    /**
     * グリッドを (width x height) で初期化し、全 tile を 0 (空) でクリアする。
     *
     * @details
     * layer_count レイヤー分のバッファを確保する。不正値 (0) は安全な既定
     * (width/height は 1、layer_count は 1、tile_size は 16.0f) にフォールバックする。
     * @param width グリッドの横セル数。
     * @param height グリッドの縦セル数。
     * @param layer_count レイヤー数 (既定 1)。
     * @param tile_size world 単位での 1 tile の辺長 (既定 16.0f)。
     */
    void Init(u32 width, u32 height, u32 layer_count = 1, f32 tile_size = 16.0f) noexcept;

    /** Init に対応する厳密かつ transactional な API。 */
    FTilemapLoadResult TryInit(
        u32 width, u32 height, u32 layer_count = 1u,
        f32 tile_size = 16.0f) noexcept;

    /**
     * Tiled Map Editor の JSON マップを読み込む。
     *
     * @details
     * map の width/height/tilewidth と "layers" 配列中の各 "tilelayer" の "data"
     * (row-major GID 配列) を取り込む。tilelayer のみ対象 (objectgroup 等は無視)、
     * GID 上位 3bit の flip フラグは除去して FTileId(u16) に clamp する (GID 0 = 空)。
     * data[k] を (x = k%w, y = k/w) に配置する。内部で Init を呼ぶ。
     * 解析失敗 / width=0 / tilelayer 欠如は ACS_ERR を返す。
     * @param json_text Tiled の JSON テキスト。
     * @param len json_text のバイト長。
     * @return 成功なら空の TResult、解析失敗ならエラー。
     */
    TResult<void> LoadTiledJson(const char* json_text, usize len) noexcept;

    /**
     * Tiled JSON document を厳密に検証し、transactional に読み込む。
     *
     * forward-compatible な入力として未知の Tiled 拡張 member は維持し、
     * 重複 member と既知 member の不正形式は拒否する。
     */
    FTilemapLoadResult TryLoadTiledJson(
        const char* json_text, usize len) noexcept;

    /**
     * 個別タイルを設定する。
     *
     * @details 範囲外 (x / y / layer) は no-op。
     * @param x 列インデックス。
     * @param y 行インデックス。
     * @param tile 設定するタイル ID。
     * @param layer 対象レイヤー (既定 0)。
     */
    void SetTile(u32 x, u32 y, FTileId tile, u32 layer = 0) noexcept;

    /**
     * 個別タイルを取得する。
     *
     * @param x 列インデックス。
     * @param y 行インデックス。
     * @param layer 対象レイヤー (既定 0)。
     * @return 指定セルのタイル ID (範囲外なら空 FTileId{0})。
     */
    FTileId GetTile(u32 x, u32 y, u32 layer = 0) const noexcept;

    /**
     * 指定レイヤー全体を tile で埋める。
     *
     * @details 範囲外 layer は no-op。
     * @param tile 埋めるタイル ID。
     * @param layer 対象レイヤー (既定 0)。
     */
    void Fill(FTileId tile, u32 layer = 0) noexcept;

    /**
     * 閉区間 [x0..x1] x [y0..y1] の矩形を tile で塗る。
     *
     * @details
     * 半開区間ではなく閉区間。x0 > x1 / y0 > y1 は swap 扱いとし、グリッド境界で
     * clamp する。範囲外 layer は no-op。
     * @param x0 矩形の左端列。
     * @param y0 矩形の上端行。
     * @param x1 矩形の右端列。
     * @param y1 矩形の下端行。
     * @param tile 塗るタイル ID。
     * @param layer 対象レイヤー (既定 0)。
     */
    void FillRect(u32 x0, u32 y0, u32 x1, u32 y1, FTileId tile, u32 layer = 0) noexcept;

    /** 全レイヤーを 0 (空) で埋める (サイズは保持)。 */
    void Clear() noexcept;

    /**
     * グリッドの横セル数を返す。
     *
     * @return 幅 (セル数)。
     */
    u32 Width()      const noexcept { return m_Width; }

    /**
     * グリッドの縦セル数を返す。
     *
     * @return 高さ (セル数)。
     */
    u32 Height()     const noexcept { return m_Height; }

    /**
     * レイヤー数を返す。
     *
     * @return 確保済みレイヤー数。
     */
    u32 LayerCount() const noexcept { return static_cast<u32>(m_Layers.Num()); }

    /**
     * 1 tile の world 単位での辺長を返す。
     *
     * @return tile_size。
     */
    f32 TileSize()   const noexcept { return m_TileSize; }

    /**
     * tile (x,y) の中心 world 座標を返す。
     *
     * @details
     * 範囲外 (x>=width / y>=height) でも計算式そのものを返す (左上原点での連続を
     * 期待する利用者のため明示的にチェックしない)。+Y=画面下なので row 0 が画面上端。
     * @param x 列インデックス。
     * @param y 行インデックス。
     * @return tile の中心の world 座標。
     */
    FVec2 TileToWorld(u32 x, u32 y) const noexcept;

    /**
     * world 座標を tile インデックスに変換する。
     *
     * @details 成功時のみ out_x / out_y を書き込む。
     * @param world 変換元の world 座標。
     * @param out_x 列インデックスの出力先。
     * @param out_y 行インデックスの出力先。
     * @return グリッド内なら true、範囲外 (グリッド外 / 負値) なら false。
     */
    bool WorldToTile(FVec2 world, u32& out_x, u32& out_y) const noexcept;

    /**
     * 指定レイヤーの描画用 raw データ先頭を返す。
     *
     * @details size = Width() * Height()、layout = row-major (y * width + x)。
     * @param layer 対象レイヤー。
     * @return レイヤー先頭ポインタ (範囲外 layer なら nullptr)。
     */
    const FTileId* LayerData(u32 layer) const noexcept;

private:
    /** レイヤーごとの row-major (w*h) タイル配列 (m_Layers[L] が L 番レイヤー)。 */
    TArray<TArray<FTileId>> m_Layers {};

    /** グリッドの横セル数。 */
    u32                  m_Width      = 0;

    /** グリッドの縦セル数。 */
    u32                  m_Height     = 0;

    /** world 単位での 1 tile の辺長。 */
    f32                  m_TileSize  = 16.0f;
};

} // namespace acs::game
