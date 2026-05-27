// SPDX-License-Identifier: Apache-2.0
// HelloTriangle — エントリポイント。
//
// 構成:
//   Types.h               - 頂点フォーマット + 三角形 3 頂点
//   Shaders.h             - HLSL ソース (VS + PS)
//   HelloTriangleApp.{h,cpp} - FApplication 派生クラス
//
// 操作:
//   Esc : 終了
//
// ACS_DEFINE_MAIN は HelloTriangleApp を main エントリに登録
// (FApplication 派生 → `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
//
// 低レベル RHI トラックの 1 本目。HLSL を直接書く発展編なので、まず
// 01〜15 の高レベル API サンプル (FStandardShader / FSpriteBatch) に慣れて
// から読んでください。
#include "HelloTriangleApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(hellotri::HelloTriangleApp)
