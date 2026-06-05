// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar (Genre Kit: Roguelike) — BSP ベース ランダムダンジョン生成
//
// 2D グリッド上に部屋 (Room) と廊下 (Corridor) を配置する古典的な
// **BSP (Binary Space Partitioning) ダンジョン生成** の実装。
// 全体領域を再帰的に 2 分割してリーフごとに 1 部屋を置き、兄弟リーフ間を
// L 字廊下で接続する。Roguelike の「毎フロア違うレイアウト」をシード再現可能に。
//
// 使い方:
//   DungeonGenConfig cfg{};
//   cfg.width             = 80;
//   cfg.height            = 50;
//   cfg.min_room_size     = 4;
//   cfg.max_room_size     = 10;
//   cfg.min_partition_size = 8;
//   cfg.seed              = 0xDEADBEEFu;
//   cfg.max_depth         = 5;
//
//   FDungeonGenerator gen;
//   gen.Generate(cfg);
//
//   // 描画ループ
//   for (u32 y = 0; y < gen.Height(); ++y) {
//       for (u32 x = 0; x < gen.Width(); ++x) {
//           switch (gen.At(x, y)) {
//               case ETileKind::Wall:     DrawSprite(wall, x, y); break;
//               case ETileKind::Floor:    DrawSprite(floor, x, y); break;
//               case ETileKind::Door:     DrawSprite(door, x, y); break;
//               case ETileKind::Corridor: DrawSprite(floor, x, y); break;
//               case ETileKind::Stairs:   DrawSprite(stairs, x, y); break;
//           }
//       }
//   }
//
//   // プレイヤースポーン位置 = 最初の部屋の中心
//   u32 px, py;
//   gen.GetRoomCenter(0, px, py);
//
// 設計選択:
//   ・**ETileKind = u8**: 5 種類しか無いので u8 で十分。row-major 1D の
//     acs::TArray<ETileKind> に格納 (FTilemap と異なる layered 構造は不要)。
//   ・**Room = 矩形 + id**: id は生成順 0..N-1。重複しない部屋を保証。
//   ・**BSP partition tree は temp**: 生成中だけ必要なので TArray<BspNode>
//     を ローカル変数 として持ち、Generate 終了で破棄する。永続データは
//     grid + rooms のみ。
//   ・**廊下は L 字**: 2 部屋の中心を結ぶ (横→縦 もしくは 縦→横)。
//     ランダムにどちらかを選ぶことで分岐感を出す。
//   ・**Stairs**: 最後の部屋にランダム配置 (典型的に始点≠終点で
//     プレイヤーが進む動機になる)。
//   ・**STL 不使用 / 全 noexcept / 非コピー・非ムーブ**: 規約準拠。
//
// 制限 (今 Phase):
//   ・部屋の形は矩形のみ (円形 / 多角形は未対応)。
//   ・廊下の幅は 1 セル固定。
//   ・モンスター / アイテム配置は呼び出し側責務 (このクラスは「地形」のみ)。
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
struct Room {
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
struct DungeonGenConfig {
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
 * 接続する。生成結果は row-major のタイルグリッドと部屋配列として保持する。Scene が
 * 所有する想定の非コピー・非ムーブ型。
 */
class FDungeonGenerator {
public:
    /** 空状態で構築する (グリッド・部屋は Generate で生成)。 */
    FDungeonGenerator() noexcept  = default;

    /** 破棄する (グリッド・部屋配列は TArray が解放)。 */
    ~FDungeonGenerator() noexcept = default;

    /** コピー禁止 (大きな配列の暗黙複製を防ぐ)。 */
    FDungeonGenerator(const FDungeonGenerator&)            = delete;

    /** コピー代入も禁止。 */
    FDungeonGenerator& operator=(const FDungeonGenerator&) = delete;

    /** ムーブ禁止 (Scene が単独所有する想定)。 */
    FDungeonGenerator(FDungeonGenerator&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FDungeonGenerator& operator=(FDungeonGenerator&&)      = delete;

    /**
     * config に従ってダンジョン全体を再生成する (既存状態は完全破棄)。
     *
     * @details
     * 不正値はサイレントに安全な既定へフォールバックして必ず有効なグリッドを生成する。
     * グリッドを Wall で塗り、BSP 分割 → リーフへの部屋配置 → 兄弟間 L 字廊下 →
     * 階段 1 個の配置、の順で構築する。
     * @param config 生成パラメータ。
     */
    void Generate(const DungeonGenConfig& config) noexcept;

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
    u32 RoomCount() const noexcept { return static_cast<u32>(m_Rooms.Size()); }

    /**
     * index 番目の部屋を返す。
     *
     * @param index 部屋インデックス。
     * @return 部屋へのポインタ。範囲外なら nullptr。
     */
    const Room* GetRoom(u32 index) const noexcept;

    /**
     * 全部屋配列の生ポインタを返す。
     *
     * @param out_count 部屋件数を書き込む先。
     * @return 部屋配列の先頭ポインタ。空なら nullptr。
     */
    const Room* AllRooms(u32& out_count) const noexcept;

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
    TArray<Room>      m_Rooms;

    /** 直近 Generate の seed (FindRandomFloor 等で再利用)。 */
    u32              m_Seed   = 0;
};

} // namespace acs::game
