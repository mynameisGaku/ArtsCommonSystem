// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B Phase 3 — FNodeId (シーングラフ用 generational handle)
//
// シーングラフ内の Node2D / Node を一意に識別する 32bit パック handle。
// 1 個の `u32` に **24bit index + 8bit generation** を pack する設計で、
// `FShapeId` (CollisionWorld2D.h) と完全に同じパターンを採用している。
//
// 使い方:
//   FNodeId id = scene.CreateNode();          // 何らかのファクトリ経由で取得
//   if (id.IsValid()) {
//       u32 slot = id.Index();               // SoA / プール配列の index
//       u8  gen  = id.Generation();          // 世代カウンタで stale handle 検出
//   }
//   if (a == b) { ... }                       // 比較も constexpr noexcept
//
// 設計選択 (なぜこの形か):
//   ・**RTTI / dynamic_cast 不使用**: ACS は STL / 例外 / RTTI 禁止。型情報は
//     呼び出し側 (Scene 等) が別 array で持つ責務。FNodeId は純粋に「どの slot
//     のどの世代か」だけを表現する POD handle。
//   ・**24bit index = 約 16M slot で十分**: 2D ゲームのシーングラフで同時存在
//     する Node 数は実用上 10K 〜 100K オーダー。16M (= 16,777,216) なら数桁
//     余裕があり、将来 ECS-lite に拡張しても枯渇しない。
//   ・**8bit generation = 256 世代**: slot を recycle してもラップアラウンドで
//     stale handle と区別できる確率が 1/256 と十分高い。**generation == 0 は
//     "初期状態 / 未使用 slot"** を意味し、`packed == 0` がそのまま invalid を
//     表すよう設計されている (`FNodeId()` のデフォルトと一致)。
//   ・**ヘッダオンリ + 全関数 constexpr noexcept**: コンパイル時計算 / 例外
//     伝播ゼロ。`.cpp` 不要なので Module.cmake 改変ゼロでドロップイン可能。
//   ・**operator==/!= のみ**: ハンドルの順序付け (< 等) は仕様として持たない。
//     必要なら呼び出し側が `Index()` を取り出して比較すべき。
//   ・**STL / `<cstdint>` 不使用**: `foundation/Types.h` の `u32 / u8` のみ。
//     ACS 全体の規約に準拠 (Phase 0 codify 済)。
//
// 注意:
//   ・index が 24bit を超える値で構築された場合、上位 bit は黙って捨てられる
//     (`& 0x00FFFFFFu`)。これは FShapeId と完全に同じ挙動。生成側 (NodePool 等)
//     で assert / TResult<E> を入れて 16M 越えを検出する責務。
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/// シーングラフ Node を識別する packed 32bit handle (generational)。
struct FNodeId {
    /// 0 = invalid。layout: low24=index, high8=generation。
    u32 _packed = 0;

    /// 既定構築 = invalid handle (packed == 0)。
    constexpr FNodeId() noexcept = default;

    /// index (24bit) と generation (8bit) を pack して構築する。
    /// index は `& 0x00FFFFFFu` でマスクされるため、24bit 超の値は上位が落ちる。
    constexpr FNodeId(u32 index, u8 gen) noexcept
        : _packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    /// pool / SoA 配列の index を取り出す (0 〜 16,777,215)。
    constexpr u32  Index() const noexcept { return _packed & 0x00FFFFFFu; }

    /// slot の世代カウンタを取り出す (0 〜 255)。stale handle 検出用。
    constexpr u8   Generation() const noexcept {
        return static_cast<u8>(_packed >> 24);
    }

    /// invalid (= packed == 0) でなければ true。
    /// 注意: 「pool に該当 slot が生きているか」は呼び出し側で別途検証すること。
    constexpr bool IsValid() const noexcept { return _packed != 0; }

    /// 完全一致比較 (index + generation の両方が一致した時のみ true)。
    constexpr bool operator==(FNodeId o) const noexcept { return _packed == o._packed; }
    constexpr bool operator!=(FNodeId o) const noexcept { return _packed != o._packed; }
};

} // namespace acs::game
