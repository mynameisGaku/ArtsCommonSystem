// SPDX-License-Identifier: Apache-2.0
// HelloVertexSSS — エントリポイント。
//
// 頂点空間サブサーフェススキャタリング (Vertex-Space SSS) のデモ。
//   ・同じ肌色の球を 2 つ並べ、左 = 生 Lambert / 右 = 頂点空間 SSS で照明
//   ・右側は terminator (明暗境界) を赤い光が回り込んで «にじむ» = 皮膚らしい内部散乱
//   ・散乱は FVertexScatter (メッシュ隣接グラフ上の RGB 別熱拡散) で CPU 計算し、
//     per-vertex の散乱後放射照度を動的頂点バッファへ毎フレーム書き込む
//   ・方向光が旋回するので terminator の移動で散乱の効果が分かりやすい
//   ・Esc 終了
#include "app/EntryPoint.h"
#include "HelloVertexSssApp.h"

ACS_DEFINE_MAIN(hellovertexsss::HelloVertexSssApp)
