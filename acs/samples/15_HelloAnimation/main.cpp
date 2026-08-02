// SPDX-License-Identifier: Apache-2.0
// HelloAnimation — エントリポイント。
//
// 構成:
//   Types.h                     - 共通定数 (kBoneCount / kSegmentCount /
//                                 kRingCount / kHeight / kAnimDuration ...)
//   AnimationScene.{h,cpp}      - シーン描画ロジック + BuildSnake (手続き生成)
//   HelloAnimationApp.{h,cpp}   - CApplication 派生クラス (resource 所有 + 入力)
//
// 動作:
//   ・4 ボーンの長い円柱を生成 (ヘビ風)
//   ・各ボーンの回転を sin 波でアニメーションしてうねうね動く
//   ・地面 + 1 灯ライティング + CSky で雰囲気付け
//
// 操作:
//   ← →   : カメラ周回
//   Space  : 再生 / 一時停止
//   Esc    : 終了
//
// 学習ポイント:
//   ・ASkinnedMeshAsset / FBone / FAnimation の構築
//   ・CSkinnedShader によるスキニング描画
//   ・CAnimationPlayer で時刻 → ボーンパレット
//   ・GPU スキニング: VS で 4 ボーンの加重平均
//
// ACS_DEFINE_MAIN は HelloAnimationApp を main エントリに登録 (CApplication
// 派生 → `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "app/EntryPoint.h"
#include "HelloAnimationApp.h"

ACS_DEFINE_MAIN(helloanim::CHelloAnimationApp)
