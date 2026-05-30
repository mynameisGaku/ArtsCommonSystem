# ACS 2D ゲーム チュートリアル

ACS で 2D ゲームを作るための、**実際に動く（検証済み）API** の実用ガイド集です。
各章は実ヘッダのシグネチャに忠実な最小例から始め、`samples/` の動くサンプル
（スクショ確認済み）に紐づけてあります。壊れている／未配線のスタブは扱いません
（下の「まだ整っていない物」参照）。

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
| 08 | [当たり判定・物理・トリガー](08_collision_physics_triggers.md) | FCollisionWorld2D / FPhysicsBody2D / FTriggerComponent | 08, 54, 57 |
| 09 | [シーン遷移・ゲーム構造・セーブ](09_scene_flow_save.md) | TransitionTo / AppState / GameFlow / FSaveArchive | 38, 58 |
| 10 | [エフェクト（水・光・ステンシル・文字）](10_effects_light_stencil_text.md) | Effects2D / Light2D / Stencil / Font | 47, 59〜61 |

## まだ整っていない物（このチュートリアルでは扱わない）

以下は「使い方以前に未完成」なので、あえて含めていません。2D ゲームを“完成”させる
前に別途これらを塞ぐ必要があります（監査結果 = `acs/Saved/foundation_synth.txt`）。

- **自分の絵／タイルを読むインポータ（Aseprite / Tiled）— 未実装。** 画像＋メタから
  スプライトシート／タイルマップを一括で読む経路が無い。今は `SetTile` / `AddFrameUv`
  を手で書くか手続き生成する。`FAssetBundle::BeginLoad` は中身が偽物（同期で全部
  Loaded 扱い）なので使わないこと。
- **音 — XAudio2 バックエンドが未配線で鳴らない。** `FAudioDirector` は状態機械として
  は出来ているが、どのサンプルもバックエンドを attach しておらず、名前→clip 解決も
  WAV→PCM デコードも未実装。**現状ゲーム内で音は出せない。**
- **ゲーム内 UI / メニュー — gameframework の `FUiLayer` はスタブ**（`Init` / `HandleInput`
  が TODO の空実装）。動く UI は `src/ui/`（`19_HelloUI`、FUiRenderer + MVVM）にあるが
  `FScene2D` 未統合。メニュー・HUD は現状この橋渡しから作る必要がある。
- **描画順 / Y ソート / レイヤー — 無い。** `FNode2D::DrawTree` はツリー順に描くだけで
  z / layer / world.y ソートが無い。見下ろしでキャラが Y で前後しない。
- **設定の永続化 — `FSettings::Save/Load` が no-op**（ディスクに書かない）。

> ⚠️ 重要: `src/gameframework/` の大半（約 216 ファイル）は「コンパイルは通る」だけで
> 一度も実行検証されていません（ジャンルキット 10 種なども全部未検証）。「ファイルが
> ある＝動く」ではありません。実際に使う前に、小さな縦切りゲームに組み込んで
> スクショ検証してから信頼してください。

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
