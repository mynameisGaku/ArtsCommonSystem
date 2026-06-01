# ACS 2D ゲーム チュートリアル

ACS で 2D ゲームを作るための、**実際に動く（検証済み）API** の実用ガイド集です。
各章は実ヘッダのシグネチャに忠実な最小例から始め、`samples/` の動くサンプル
（スクショ確認済み）に紐づけてあります。残る注意点は下の「実装状況」を参照。

順番に読むなら 01 → 10 が素直です。困ったら該当章だけ拾い読みでも構いません。
名前空間は `acs`（2D ランタイム基盤）と `acs::game`（ノード/コンポーネント層）。
プラットフォームは Windows / DX12、STL 不使用・`Result<T,E>` 系で扱います。

## 目次

| # | 章 | 内容 | 対応サンプル |
|---|---|---|---|
| 01 | [はじめに（FGame + FScene2D）](01_getting_started.md) | 最小アプリ・ライフサイクル・座標系 | 55 |
| 02 | [ノード & コンポーネント](02_nodes_components.md) | FNode2D ツリー / 自作コンポーネント / Transform | 28, 55, 59 |
| 03 | [スプライト描画](03_sprites.md) | FSprite2DComponent / SpriteBatch / テクスチャ読込 | 02, 55, 56 |
| 04 | [スプライトアニメ](04_sprite_anim.md) | FSpriteAnimComponent / シートアニメ | 56 |
| 05 | [タイルマップ](05_tilemap.md) | FTilemapComponent / 当たり判定生成 | 58 |
| 06 | [カメラ](06_camera.md) | 追従 / ズーム / シェイク / ピッキング | 55 |
| 07 | [入力](07_input.md) | 素の Input:: と FInputMap アクション | 55, 59〜61 |
| 08 | [当たり判定・物理・トリガー](08_collision_physics_triggers.md) | FCollisionWorld2D（AABB/円/凸ポリ/**OBB**）/ FPhysicsBody2D（collide-and-slide）/ FTriggerComponent | 08, 54, 57, 63 |
| 09 | [シーン遷移・ゲーム構造・セーブ](09_scene_flow_save.md) | ChangeScene / TransitionTo / AppState / FSettings / FSaveArchive | 38, 58, 63 |
| 10 | [エフェクト（水・光・ステンシル・文字）](10_effects_light_stencil_text.md) | Effects2D / Light2D / Stencil / Font | 47, 59〜61 |

## 実装状況（2026-06 更新：旧「未完成」項目はほぼ解消）

このガイド初版で「未完成」としていた中核ギャップは、その後の基盤強化で **実装＋
サンプル検証済み** になりました。最新の正規座標規約は **Y-down（左上原点・+X 右・
+Y 下、章 01 参照）** です。

- **自分の絵／タイルを読むインポータ — 実装済み。** `FSpritePack::LoadAtlasJson`
  （Aseprite hash / TexturePacker array 両対応）、`FTilemap::LoadTiledJson`（Tiled
  `.tmj`）、STL-free JSON パーサ `container/Json.h`。ファイル読込は
  `FileSystem::ReadAllText` で行い、その文字列を渡す。検証 = `62_HelloPersistVerify`。
- **ゲーム内 UI / メニュー — 実装済み。** `FUiLayer` を本実装化（クリック可能な
  ボタン、hover／押下、consume-on-read）。`FScene2D` の `OnEvent→HandleInput` /
  `OnDrawHud→Draw(rc)` で配線。検証 = `62`（click ロジック）/ `63`（タイトル・ポーズ・
  ゲームオーバーの実メニュー）。
- **描画順 / Y ソート / レイヤー — 実装済み。** `FNode2D` に
  `EChildDrawOrder{Tree/Layer/LayerThenY}` + `SetSortLayer` / `SetYSortBias`。
  見下ろし Y 遮蔽は LayerThenY（+Y=画面下なので小さい y=奥 を先に描画）。検証 = `62`。
- **設定の永続化 — 実装済み。** `FSettings::Save/Load`（INI 風、atomic write）。
  検証 = `62`（round-trip）/ `63`（ハイスコアを保存→次回起動でロード）。
- **音 — backend コードはあるが、サンプルでは未接続（要注意）。**
  `audio_backend/XAudio2Backend` + `FAudioDirector` の name→clip 解決と dispatch は
  実装済み（`62` で mock 検証）。ただしどのサンプルも実バックエンドを attach せず
  WAV も供給していないため **実際の発音は未確認**。鳴らすには自分で backend を attach
  して clip を登録する必要がある。

> ⚠️ 中核 2D パス（FGame/FScene2D・ノード/コンポーネント・スプライト/アニメ・
> タイルマップ・当たり判定/物理/トリガー・**UI・描画順・セーブ**・エフェクト）は
> `samples/55〜63` でスクショ／ヘッドレス検証済みです。一方 `src/gameframework/` の
> ジャンルキットや多くの上位システムは「コンパイルは通る」だけで未検証のものが
> 残ります（「ファイルがある＝動く」ではない）。実使用前に小さな縦切りに組み込んで
> 検証してください — **`63_HelloVerticalSlice` がその縦切りの実例**（title→play→pause→
> game over→save、Y-down、UI・atlas・tilemap・collide-and-slide を 1 本に統合）。

## サンプル早見表（動く＝検証済み）

| サンプル | 何が見られるか |
|---|---|
| `02_HelloSprite` | スプライト描画の最小 |
| `08_HelloPhysics2D` | 重力 + AABB 衝突 |
| `28_HelloGameFramework` | FGame / Scene / ノードツリー / コンポーネント |
| `38_HelloFullGame` | 完結ゲーム（Health/Inventory/Weapon/Score/セーブ往復/game-feel） |
| `47_HelloLight2D` | 2D 点光源 + ソフトシャドウ |
| `55_HelloScene2D` | FScene2D スターター（スプライト/カメラ追従/入力） |
| `56_HelloSpriteAnim` | シートアニメ + HUD テキスト |
| `57_HelloTriggers` | コリジョン layer/mask + トリガー |
| `58_HelloTilemap` | タイルマップ + フェードシーン遷移 |
| `59_HelloEffects2D` | 水（横視点＋反射）/炎/トレイル |
| `60_HelloStencilMask` | 任意形状ステンシルマスク |
| `61_HelloWaterTopDown` | 見下ろし水面（コースティクス/岸泡） |
| `62_HelloPersistVerify` | （コンソール）基盤の round-trip / ロジック検証ハーネス |
| `63_HelloVerticalSlice` | **縦スライス完結**: title→play→pause→game over→save、UI・atlas・tilemap・collide-and-slide・OBB を 1 本に統合（Y-down） |
