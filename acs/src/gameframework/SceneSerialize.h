// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — シーン (FNode2D ツリー) のシリアライズ (SceneSerialize)
// -----------------------------------------------------------------------------
// FNode2D の階層構造 + 各ノードの FTransform2D + 描画フラグ (enabled/visible/
// sortLayer/ySortBias/childOrder) を自己記述バイト列へ往復させる「シーンの骨格」永続化。
// レベル配置 (どこに何があるか) をディスク/メモリに保存・復元する土台。
//
// フォーマット (version 2):
//   [u32 magic][u32 version][u32 node_count]
//   per node (DFS pre-order = 親が子より先):
//     [i32 parent_index (-1=root)]
//     [f32 px][f32 py][f32 rot][f32 sx][f32 sy]
//     [u8 enabled][u8 visible][i32 sortLayer][f32 ySortBias][u8 childOrder]
//     [u32 component_count]
//     per component (ReflectName を持つもののみ):
//       [u8 name_len][reflect_name…][u32 payload_len][payload (ReflectSerialize の出力)]
//
// コンポーネント:
//   ・ComponentFactory + 非テンプレート attach + ReflectName 橋で「型を知らずに」復元する。
//   ・値は ReflectSerialize で往復する。public フィールドを ACS_RFIELD 反射した
//     コンポーネントは値も完全に復元される。private メンバのみ (ACS_RPROP スキーマ) の
//     同梱コンポーネントは payload が空となり factory 既定値で復元される (attach は保つ)。
//   ・ReflectName を持たない (未対応の) コンポーネント / Abstract で実体化できない型は
//     save 時にスキップ (= 復元されない)。
//
// 範囲:
//   ・ノードは素の FNode2D として復元する (派生ノード型のロジックは持たない。
//     データ駆動シーン = 素ノード + コンポーネントを想定)。
//
// 規約: no-STL / no-exceptions / 全 noexcept / 固定バッファ。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "memory/UniquePtr.h"

namespace acs::game {

class FNode2D;

/** シーン骨格フォーマットの識別 + バージョン。 */
inline constexpr u32 kSceneSerializeMagic   = 0xAC5F2002u;
inline constexpr u32 kSceneSerializeVersion = 2u;   // v2: コンポーネント (attach + 値) を含む

/**
 * root とその子孫を骨格バイト列へ直列化する (構造 + transform + 描画フラグ)。
 *
 * @param root 直列化するツリーの根 (この root 自身も含む)。
 * @param buf  出力バッファ。
 * @param cap  buf の容量。
 * @return 書き込んだバイト数。引数不正 / cap 不足なら 0。
 */
u32 SaveNodeTree(const FNode2D* root, u8* buf, u32 cap) noexcept;

/**
 * SaveNodeTree のバイト列からツリーを復元する (素の FNode2D で再構築)。
 *
 * @param data 直列化データ。
 * @param size data のバイト数。
 * @return 復元したツリーの根 (所有権は呼び出し側)。失敗 / 空は null。
 */
TUniquePtr<FNode2D> LoadNodeTree(const u8* data, u32 size) noexcept;

} // namespace acs::game
