// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar (Genre Kit: Roguelike) — BSP ベース ランダムダンジョン生成
//
// 2D グリッド上に部屋 (FRoom) と廊下 (Corridor) を配置する古典的な
// **BSP (Binary Space Partitioning) ダンジョン生成** の実装。
// 全体領域を再帰的に 2 分割してリーフごとに 1 部屋を置き、兄弟リーフ間を
// L 字廊下で接続する。Roguelike の「毎フロア違うレイアウト」をシード再現可能に。
//
// 使い方:
//   FDungeonGenConfig cfg{};
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
//   ・**FRoom = 矩形 + id**: id は生成順 0..N-1。重複しない部屋を保証。
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

// ---- Tile 種別 -------------------------------------------------------------
// At() が返す値。u8 で 5 種類を表現。
enum class ETileKind : u8 {
    Wall     = 0, // 通行不可
    Floor    = 1, // 部屋の床
    Door     = 2, // 部屋⇔廊下の境界 (将来拡張用、現状は使用しないが API は保持)
    Corridor = 3, // 廊下 (部屋外の通路)
    Stairs   = 4, // 次フロアへの階段
};

// ---- FRoom ------------------------------------------------------------------
// 部屋を表す軸並行矩形。(x,y) は左上、(w,h) は幅・高さ。
struct FRoom {
    u32 x  = 0;
    u32 y  = 0;
    u32 w  = 0;
    u32 h  = 0;
    u32 id = 0;
};

// ---- 生成パラメータ --------------------------------------------------------
// Generate() に渡す設定値。0 や不整合な値は内部で安全な既定にフォールバック。
struct FDungeonGenConfig {
    u32 width              = 64;     // ダンジョン全体の幅 (tile)
    u32 height             = 48;     // ダンジョン全体の高さ (tile)
    u32 min_room_size      = 4;      // 部屋の最小辺 (内側、壁含まず)
    u32 max_room_size      = 10;     // 部屋の最大辺
    u32 min_partition_size = 8;      // BSP がこれ以下のパーティションを再分割しない
    u32 seed               = 0;      // 再現用 PRNG seed
    u32 max_depth          = 5;      // BSP 再帰の最大深さ (リーフ数 ≤ 2^max_depth)
};

// ---- 生成器 ----------------------------------------------------------------
class FDungeonGenerator {
public:
    FDungeonGenerator() noexcept  = default;
    ~FDungeonGenerator() noexcept = default;

    // 非コピー・非ムーブ (FScene 所有想定、サイズが大きいので明示複製を強制)
    FDungeonGenerator(const FDungeonGenerator&)            = delete;
    FDungeonGenerator& operator=(const FDungeonGenerator&) = delete;
    FDungeonGenerator(FDungeonGenerator&&)                 = delete;
    FDungeonGenerator& operator=(FDungeonGenerator&&)      = delete;

    // ---- 生成 ----------------------------------------------------------------
    // config に従って全体を再生成 (既存状態は完全破棄)。
    // 不正値はサイレントに既定にフォールバックして必ず有効なグリッドを生成。
    void Generate(const FDungeonGenConfig& config) noexcept;

    // grid / rooms / size を全部リセット。
    void Clear() noexcept;

    // ---- 形状 / 範囲 ---------------------------------------------------------
    u32 Width()  const noexcept { return _width;  }
    u32 Height() const noexcept { return _height; }

    // ---- グリッド読み書き ----------------------------------------------------
    // 範囲外は Wall (通行不可) を返す。
    ETileKind At(u32 x, u32 y) const noexcept;
    // 範囲外は no-op。
    void SetTile(u32 x, u32 y, ETileKind kind) noexcept;

    // ---- 部屋情報 -----------------------------------------------------------
    u32 RoomCount() const noexcept { return static_cast<u32>(_rooms.Size()); }

    // 範囲外 index は nullptr。
    const FRoom* GetRoom(u32 index) const noexcept;

    // 全部屋配列の生ポインタを返す (out_count に件数を書き込む)。空なら nullptr。
    const FRoom* AllRooms(u32& out_count) const noexcept;

    // 範囲外 index は (0,0) を書き込む (defensive)。
    void GetRoomCenter(u32 room_index, u32& out_x, u32& out_y) const noexcept;

    // ---- 通行判定 -----------------------------------------------------------
    // Floor / Door / Corridor / Stairs を通行可とみなす。範囲外 / Wall は false。
    bool IsWalkable(u32 x, u32 y) const noexcept;

    // ランダムに Floor タイルを探して座標を out に書き込む。最大 100 試行で
    // 見つからなければ (0,0) を書いて 0 を返す (試行回数)。
    // 成功時は実際の試行回数 (1..100) を返す。
    u32 FindRandomFloor(u32& out_x, u32& out_y) const noexcept;

private:
    // ---- 永続データ ---------------------------------------------------------
    u32              _width  = 0;
    u32              _height = 0;
    TArray<ETileKind>  _grid;   // row-major (width * height)
    TArray<FRoom>      _rooms;  // 生成順
    u32              _seed   = 0; // 直近 Generate の seed (FindRandomFloor 等で再利用)
};

} // namespace acs::game
