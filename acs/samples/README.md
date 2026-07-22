# ACS サンプル集 — 学習ガイド

ここには ACS の使い方を学ぶための **28 個のサンプル**が、学習しやすい順に
`00_` 〜 `27_` の番号付きで並んでいます。**番号順に読み進める**のが基本です。

いちばん簡単なのは `00_HelloEasy`（`acs::easy` レイヤ）です。クラスも継承も
使わず、関数を呼ぶだけで 2D ゲームが書けます。`01_` 以降は「`FApplication` を
継承して 4 つの関数を書く」本格的な書き方になります。まずは
[`../docs/QUICKSTART.md`](../docs/QUICKSTART.md) でその骨組みを確認してから
`01_HelloWindow` を起動してみてください。

ビルド方法はリポジトリ直下の [`README.md`](../../README.md) を参照。

---

## 学習トラック

サンプルは難易度順にグループ分けされています。

### (0) 最も簡単な入口 — `acs::easy` レイヤ　`00`
クラス・継承・エラー型を使わず、手続き的に関数を呼ぶだけで 2D ゲームが書けます。
プログラミング自体にまだ慣れていない人は、まずここから。

| # | サンプル | 学べること |
|---|---|---|
| 00 | `00_HelloEasy` | `acs::easy` で図形・文字・入力。`OpenWindow` と `while(NextFrame())` だけの最小構成 |

### (1) 入門 — 2D とゲームの基礎　`01`〜`09`
シェーダ（HLSL）を一切書きません。`FSpriteBatch` などの高レベル API だけで
2D ゲームに必要な要素を学びます。

| # | サンプル | 学べること |
|---|---|---|
| 01 | `01_HelloWindow` | ウィンドウ・入力・`FApplication` のライフサイクル（最初の 1 本、73 行） |
| 02 | `02_HelloSprite` | 2D スプライト・矩形・α ブレンド（`FSpriteBatch`） |
| 03 | `03_HelloText` | TTF フォント・UTF-8・漢字テキスト描画 |
| 04 | `04_HelloECS` | ECS（`FWorld` / `TQueryView`）・並列イテレーション・`FMessageBroker`・`FTimerManager` |
| 05 | `05_HelloSave` | `FStorage` で設定・セーブデータを INI 形式で永続化 |
| 06 | `06_HelloLocalization` | 多言語対応（i18n）・ja / en / fr 切替 |
| 07 | `07_HelloAudio` | XAudio2 で WAV / MP3 を再生 |
| 08 | `08_HelloPhysics2D` | 2D 円衝突・重力・マウスでボール発射 |
| 09 | `09_HelloParticles` | 2D パーティクル（火 / 火花 / 噴水 / 煙） |

### (2) 3D 入門 — FStandardShader　`10`〜`15`
ここも HLSL を書きません。`FStandardShader` が「光が当たった 3D」を
シェーダ記述なしで描いてくれます。

| # | サンプル | 学べること |
|---|---|---|
| 10 | `10_HelloModel` | `FStandardShader` で 3D ライティング描画・手続きプリミティブ・非同期ロード（最初の 3D） |
| 11 | `11_HelloRaycast3D` | 3D レイキャスト・マルチライト・Blinn-Phong |
| 12 | `12_HelloLights` | 暗い部屋を 4 色の点光源で動的ライティング |
| 13 | `13_HelloSky` | 手続き生成スカイ（昼 / 夕焼け / 夜プリセット） |
| 14 | `14_HelloShadows` | シャドウマップ・PCF・太陽方向アニメ |
| 15 | `15_HelloAnimation` | スキンメッシュ・GPU ボーンスキニング |

### (3) 低レベル RHI — HLSL を直接書く　`16`〜`18`
**ここからは発展編**です。`FStandardShader` を使わず、シェーダ（HLSL）と
GPU リソースを自分で組み立てます。グラフィックスが初めてなら、(1)(2) に
十分慣れてから進んでください。

| # | サンプル | 学べること |
|---|---|---|
| 16 | `16_HelloTriangle` | HLSL シェーダ・頂点バッファ・パイプラインの最小構成 |
| 17 | `17_HelloMesh` | 3D キューブ・定数バッファ・深度テスト・カメラ |
| 18 | `18_HelloTextured` | テクスチャ・サンプラ・UV |

### (4) UI・ツール・通信　`19`〜`22`

| # | サンプル | 学べること |
|---|---|---|
| 19 | `19_HelloUI` | ACS 純正 UI フレームワーク（FWidget + TObservable バインディング） |
| 20 | `20_HelloMVVM` | MVVM データバインディング（TObservable / Binder / FCommand） |
| 21 | `21_HelloImGui` | Dear ImGui 統合（デバッグ UI） |
| 22 | `22_HelloNet` | TCP echo（`FApplication` を使わない素の例） |

### (5) 上級グラフィックス　`23`〜`26`

| # | サンプル | 学べること |
|---|---|---|
| 23 | `23_HelloPbr` | PBR マテリアル（Cook-Torrance BRDF、metallic × roughness グリッド） |
| 24 | `24_HelloBloom` | HDR シーン・Bloom・ACES トーンマップ |
| 25 | `25_HelloIbl` | Image-Based Lighting（環境マップ照明の統合デモ） |
| 26 | `26_HelloLightmap` | Cornell box・CPU パストレースのライトマップベイク |
| 27 | `27_HelloShowcase` | Cinematic showcase（PBR + IBL + ガラス屈折 + 全 post-process、auto-orbit カメラ） |

> `24`〜`27` は `FApplication::OnCustomFrame()` を override し、フレーム描画を
> 自前で制御します（HDR レンダーターゲット + ポストプロセスを組むため）。
> 通常の `OnRender` モデルとは別の、上級者向けの経路です。

---

## ビルド要件（重要）

サンプルによって必要なレンダラバックエンドが異なります。プリセットの違いは
リポジトリ直下の `README.md` を参照してください。

| サンプル | 必要なプリセット |
|---|---|
| `01`〜`19`, `22`, `23` | `dx12-*` / `diligent-*` どちらでも可 |
| `20_HelloMVVM`, `21_HelloImGui` | `dx12-*`（ImGui は DX12 raw backend 専用。`diligent-*` 単独ビルドでは生成されません） |
| `24_HelloBloom`, `25_HelloIbl`, `26_HelloLightmap` | `diligent-*`（HDR / cubemap など Diligent 専用機能を使用） |

ビルド対象に入っていないサンプルは `cmake-build-*/samples/` に生成されません。
作りたいサンプルが見つからないときは、まずプリセットを確認してください。

---

## 自分のゲームを作る

サンプルに慣れたら、`01_HelloWindow` をフォルダごとコピーして自分のゲームの
出発点にするのが簡単です。

1. `samples/01_HelloWindow` を任意の場所にコピーし、フォルダ名を変える。
2. コピー先の `CMakeLists.txt` を編集する。**`ACS::Game`** は標準的なゲームに
   必要な 10 モジュールをまとめた集約ターゲットです：

   ```cmake
   add_executable(my_game WIN32 main.cpp)
   acs_apply_compiler_options(my_game)
   target_link_libraries(my_game PRIVATE ACS::Game)

   # 音声・ImGui・UI などを使う場合だけ、追加モジュールを足す:
   #   target_link_libraries(my_game PRIVATE ACS::Game ACS::Audio)
   ```

3. `main.cpp` で `FApplication` を継承し、`OnStart` / `OnUpdate` / `OnRender` /
   `OnShutdown` を実装する（[`../docs/QUICKSTART.md`](../docs/QUICKSTART.md) 参照）。
4. 描画系のヘッダは、個別に並べる代わりに **`#include "render/Render.h"`** 一行で
   RHI と高レベルヘルパ（`FStandardShader` / `FSpriteBatch` / `FFont` など）を
   まとめて取り込めます（各サンプルは何を使っているか分かるよう個別に
   include しています。自作ゲームでは集約ヘッダが手軽です）。

つまずいたら [`../docs/TROUBLESHOOTING.md`](../docs/TROUBLESHOOTING.md) を
参照してください。
