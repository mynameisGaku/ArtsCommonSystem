// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * タイル種別。At() が返す値で、u8 で 5 種類を表現する。
 */
enum class ETileKind : u8 {
    /** 通行不可の壁。 */
    Wall     = 0,

    /** 部屋の床。 */
    Floor    = 1,

    /** 部屋⇔廊下の境界 (将来拡張用、現状は未使用だが API は保持)。 */
    Door     = 2,

    /** 廊下 (部屋外の通路)。 */
    Corridor = 3,

    /** 次フロアへの階段。 */
    Stairs   = 4,
};

/**
 * 部屋を表す軸並行矩形。
 *
 * @details (x,y) は左上座標、(w,h) は幅・高さ。id は生成順の通し番号。
 */
struct FRoom {
    /** 左上 x 座標 (tile)。 */
    u32 x  = 0;

    /** 左上 y 座標 (tile)。 */
    u32 y  = 0;

    /** 幅 (tile)。 */
    u32 w  = 0;

    /** 高さ (tile)。 */
    u32 h  = 0;

    /** 生成順の部屋 id (0..N-1)。 */
    u32 id = 0;
};

/**
 * Generate() に渡す生成パラメータ。
 *
 * @details 0 や不整合な値は内部で安全な既定にフォールバックされる。
 */
struct FDungeonGenConfig {
    /** ダンジョン全体の幅 (tile)。 */
    u32 width              = 64;

    /** ダンジョン全体の高さ (tile)。 */
    u32 height             = 48;

    /** 部屋の最小辺 (内側、壁含まず)。 */
    u32 min_room_size      = 4;

    /** 部屋の最大辺。 */
    u32 max_room_size      = 10;

    /** BSP がこれ以下のパーティションを再分割しない下限辺長。 */
    u32 min_partition_size = 8;

    /** 再現用 PRNG seed。 */
    u32 seed               = 0;

    /** BSP 再帰の最大深さ (リーフ数 ≤ 2^max_depth)。 */
    u32 max_depth          = 5;
};

/**
 * BSP ベースのランダムダンジョン生成器。
 *
 * @details
 * 全体領域を再帰的に 2 分割してリーフごとに 1 部屋を置き、兄弟リーフ間を L 字廊下で
 * 接続する。生成結果は row-major のタイルグリッドと部屋配列として保持する。AScene が
 * 所有する想定の非コピー・非ムーブ型。
 */
class CDungeonGenerator {
public:
    /** 空状態で構築する (グリッド・部屋は Generate で生成)。 */
    CDungeonGenerator() noexcept  = default;

    /** 破棄する (グリッド・部屋配列は TArray が解放)。 */
    ~CDungeonGenerator() noexcept = default;

    /** コピー禁止 (大きな配列の暗黙複製を防ぐ)。 */
    CDungeonGenerator(const CDungeonGenerator&)            = delete;

    /** コピー代入も禁止。 */
    CDungeonGenerator& operator=(const CDungeonGenerator&) = delete;

    /** ムーブ禁止 (AScene が単独所有する想定)。 */
    CDungeonGenerator(CDungeonGenerator&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CDungeonGenerator& operator=(CDungeonGenerator&&)      = delete;

    /**
     * config に従ってダンジョン全体を再生成する (既存状態は完全破棄)。
     *
     * @details
     * 不正値はサイレントに安全な既定へフォールバックして必ず有効なグリッドを生成する。
     * グリッドを Wall で塗り、BSP 分割 → リーフへの部屋配置 → 兄弟間 L 字廊下 →
     * 階段 1 個の配置、の順で構築する。
     * @param config 生成パラメータ。
     */
    void Generate(const FDungeonGenConfig& config) noexcept;

    /** グリッド・部屋配列・サイズ・seed を全てリセットする。 */
    void Clear() noexcept;

    /**
     * ダンジョンの幅を返す。
     *
     * @return 幅 (tile)。
     */
    u32 Width()  const noexcept { return m_Width;  }

    /**
     * ダンジョンの高さを返す。
     *
     * @return 高さ (tile)。
     */
    u32 Height() const noexcept { return m_Height; }

    /**
     * 指定座標のタイル種別を返す。
     *
     * @param x x 座標 (tile)。
     * @param y y 座標 (tile)。
     * @return タイル種別。範囲外は Wall (通行不可)。
     */
    ETileKind At(u32 x, u32 y) const noexcept;

    /**
     * 指定座標にタイルを書き込む。
     *
     * @param x x 座標 (tile)。
     * @param y y 座標 (tile)。
     * @param kind 書き込むタイル種別。範囲外なら no-op。
     */
    void SetTile(u32 x, u32 y, ETileKind kind) noexcept;

    /**
     * 部屋の数を返す。
     *
     * @return 部屋数。
     */
    u32 RoomCount() const noexcept { return static_cast<u32>(m_Rooms.Num()); }

    /**
     * index 番目の部屋を返す。
     *
     * @param index 部屋インデックス。
     * @return 部屋へのポインタ。範囲外なら nullptr。
     */
    const FRoom* GetRoom(u32 index) const noexcept;

    /**
     * 全部屋配列の生ポインタを返す。
     *
     * @param out_count 部屋件数を書き込む先。
     * @return 部屋配列の先頭ポインタ。空なら nullptr。
     */
    const FRoom* AllRooms(u32& out_count) const noexcept;

    /**
     * index 番目の部屋の中心座標を取得する。
     *
     * @param room_index 部屋インデックス。
     * @param out_x 中心 x 座標を書き込む先。
     * @param out_y 中心 y 座標を書き込む先。範囲外 index は (0,0) を書き込む (defensive)。
     */
    void GetRoomCenter(u32 room_index, u32& out_x, u32& out_y) const noexcept;

    /**
     * 指定座標が通行可能かを返す。
     *
     * @details Floor / Door / Corridor / Stairs を通行可とみなす。
     * @param x x 座標 (tile)。
     * @param y y 座標 (tile)。
     * @return 通行可なら true。範囲外 / Wall は false。
     */
    bool IsWalkable(u32 x, u32 y) const noexcept;

    /**
     * ランダムに Floor タイルを探して座標を書き込む。
     *
     * @details 最大 100 試行する。直近 Generate の seed を撹拌した一時 PRNG を使う。
     * @param out_x 見つけた x 座標を書き込む先 (失敗時 0)。
     * @param out_y 見つけた y 座標を書き込む先 (失敗時 0)。
     * @return 成功時は実際の試行回数 (1..100)、見つからなければ 0。
     */
    u32 FindRandomFloor(u32& out_x, u32& out_y) const noexcept;

private:
    /** ダンジョンの幅 (tile)。 */
    u32              m_Width  = 0;

    /** ダンジョンの高さ (tile)。 */
    u32              m_Height = 0;

    /** タイルグリッド (row-major、width * height)。 */
    TArray<ETileKind>  m_Grid;

    /** 部屋配列 (生成順)。 */
    TArray<FRoom>      m_Rooms;

    /** 直近 Generate の seed (FindRandomFloor 等で再利用)。 */
    u32              m_Seed   = 0;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FDungeonGenerator = CDungeonGenerator;

} // namespace acs::game
