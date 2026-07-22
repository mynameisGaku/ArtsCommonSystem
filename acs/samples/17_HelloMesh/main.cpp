// SPDX-License-Identifier: Apache-2.0
// HelloMesh — エントリポイント。
//
// 構成:
//   Types.h              - 頂点フォーマット + キューブ 24 頂点 / 36 indices
//   Shaders.h            - HLSL ソース (VS + PS + MVP cbuffer)
//   HelloMeshApp.{h,cpp} - FApplication 派生クラス
//
// 操作:
//   矢印キー : カメラを左右回転
//   Esc      : 終了
//
// ACS_DEFINE_MAIN は HelloMeshApp を main エントリに登録
// (FApplication 派生 → `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
//
// 低レベル RHI トラックの 2 本目 (3D + 定数バッファ + 深度バッファ)。
// HLSL を直接書く発展編なので、まず 01〜15 の高レベル API サンプルから
// 進めてください。
#include "HelloMeshApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(hellomesh::FHelloMeshApp)
