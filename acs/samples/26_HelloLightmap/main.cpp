// SPDX-License-Identifier: Apache-2.0
// HelloLightmap — エントリポイント。multi-bounce path-traced lightmap。
//
// 構成:
//   LightmapTypes.h           - FQuad / FRng / 数学ヘルパ / constexpr 定数 (inline)
//   LightmapBaker.{h,cpp}     - PathTrace + BakeLightmaps (CPU 焼き)
//   CornellBox.{h,cpp}        - Cornell box シーンの構築
//   FHelloLightmapApp.{h,cpp}  - FApplication 派生 (OnStart/OnCustomFrame ほか)
//
// 動作:
//   ・古典的な Cornell box (床 / 天井 / 奥壁 / 左壁(赤) / 右壁(緑)) を構築
//   ・天井を発光面とみなし、各面の lightmap テクセルへ CPU で multi-bounce GI を
//     パストレースでベイクする (cosine-weighted hemisphere sampling + 軸並行
//     平面交差 + 最大 5 バウンス)。固定係数の擬似 1-bounce ではなく、壁どうしの
//     多重反射 color bleeding が物理的に焼き込まれる
//   ・MC ノイズは 3x3 box blur で均し、HDR テクスチャ (R32G32B32A32F) 化して
//     FPbrShader の lightmap slot 経由で表示する
//   ・シーンは HDR RT に描画 → Bloom + ACES tonemap で LDR 出力。lightmap が
//     HDR なので天井 (光源) が飽和せず、tonemap で自然にロールオフし bloom で光る
//   ・WASD でカメラ移動、矢印で視点回転、L で lightmap on/off、Esc 終了
//
// 学習ポイント:
//   ・動的ライティング無しでも、事前計算した間接光で「赤/緑の壁の照り返しが
//     床に色づく」color bleeding が表現できる
//   ・cosine-weighted path tracing なら throughput に albedo を畳むだけで
//     多重反射が自然に積算される (π が pdf と相殺するので係数調整が不要)
//   ・lightmap は mesh の uv で引く (このサンプルは 1 面 = 1 テクスチャ)
#include "HelloLightmapApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(hellolightmap::FHelloLightmapApp)
