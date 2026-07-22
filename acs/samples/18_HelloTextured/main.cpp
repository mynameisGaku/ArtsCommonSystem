// SPDX-License-Identifier: Apache-2.0
// HelloTextured — エントリポイント。
//
// 構成:
//   Types.h                  - 頂点フォーマット + キューブ 24 頂点 + UV + テクスチャサイズ
//   Shaders.h                - HLSL ソース (VS + PS + texture sampling)
//   TextureGen.{h,cpp}       - 手続き生成テクスチャ (チェッカーボード + 月)
//   HelloTexturedApp.{h,cpp} - FApplication 派生クラス
//
// 操作:
//   矢印キー : カメラを左右回転
//   Esc      : 終了
//
// ACS_DEFINE_MAIN は HelloTexturedApp を main エントリに登録
// (FApplication 派生 → `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
//
// 低レベル RHI トラックの 3 本目 (テクスチャ + 固定サンプラ)。HLSL を直接書く
// 発展編なので、まず 01〜15 の高レベル API サンプルから進めてください。
#include "HelloTexturedApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(hellotextured::FHelloTexturedApp)
