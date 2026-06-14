// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — 3D シーン (FNode3D ツリー) のテキストシリアライズ
// -----------------------------------------------------------------------------
// FScene3D の階層 + 各ノードの FTransform3D (pos/euler/scale) + FMeshComponent3D
// (prim/color/mesh path) を行ベースのテキストへ往復させる。editor_abi の 3D ビュー
// ポートの scene3d_serialize/load_text が委譲する «正準フォーマット» (移行後)。
//
// フォーマット (行ベース、editor の N3D/MSH3D を階層対応に拡張):
//   N3D <id> <parent> <prim> <px py pz> <rx ry rz(度)> <sx sy sz> <r g b a> <name>
//   MSH3D <id> <mesh_path>
//   ・id        = DFS pre-order の通し番号 (root=0)。
//   ・parent    = 親の id (-1 = root 自身)。
//   ・prim      = EMeshPrimitive3D の整数 (FMeshComponent3D 無しは -1)。
//   ・rot       = FTransform3D::EulerDeg() (度、XYZ。|Y|<90° で往復一致)。
//   ・MSH3D     = prim==Mesh かつ mesh path を持つノードのみ (アセット実体のロードは
//                 呼び出し側の責務。本シリアライザはパスのみ往復する)。
//
// 規約: no-STL (C stdio の sscanf/snprintf のみ) / 全 noexcept / 固定バッファ。
// =============================================================================
#pragma once

#include "foundation/Types.h"

namespace acs::game {

class FScene3D;

/**
 * FScene3D をテキストへ直列化する (root + 全子孫、構造 + transform + メッシュ記述)。
 *
 * @param scene 直列化するシーン。
 * @param out 出力バッファ (null 終端される)。
 * @param cap out の容量。
 * @return 書き込んだ文字数 (null 終端を除く)。引数不正 / cap 不足は途中で打ち切る。
 */
u32 SaveScene3DText(const FScene3D& scene, char* out, u32 cap) noexcept;

/**
 * SaveScene3DText のテキストから FScene3D を復元する (既存内容を置き換える)。
 *
 * @details
 * scene.Clear() で空にしてから N3D / MSH3D 行を解析して再構築する。メッシュアセットの
 * 実体ロードは行わず、FMeshComponent3D にパスを設定するのみ (呼び出し側が SetMeshAsset)。
 * @param scene 復元先のシーン (内容は置き換わる)。
 * @param text 直列化テキスト。
 * @return 解析が成立したら true (text==null は false)。
 */
bool LoadScene3DText(FScene3D& scene, const char* text) noexcept;

} // namespace acs::game
