# ACS 2D ゲーム チュートリアル

ACS で 2D ゲームを組み立てるための実用ガイドです。各章は公開 API の責務、
最小コード、失敗条件を本文内で説明します。順番に読む場合は 01 から 10 へ進みます。

名前空間は `acs` が runtime 基盤、`acs::game` が scene、node、component 層です。
2D world は画面中央を基準に +Y を下向きとし、HUD / screen は左上原点の pixel 座標を
使います。world と screen の変換は `AScene::ScreenToWorld` が担当します。

## 目次

| # | 章 | 中心となる責務 |
|---|---|---|
| 01 | [はじめに（CGame + AScene）](01_getting_started.md) | application、scene lifecycle、座標系 |
| 02 | [ノード & コンポーネント](02_nodes_components.md) | `ANode` tree、`AComponent`、`FTransform3D` |
| 03 | [スプライト描画](03_sprites.md) | `ASprite2DComponent`、`CSpriteBatch`、texture |
| 04 | [スプライトアニメ](04_sprite_anim.md) | `ASpriteAnimComponent`、UV frame |
| 05 | [タイルマップ](05_tilemap.md) | `FTilemap`、`ATilemapComponent` |
| 06 | [カメラ](06_camera.md) | `CCamera2D`、追従、zoom、picking |
| 07 | [入力](07_input.md) | `CInput`、`FInputMap`、action |
| 08 | [当たり判定・物理・トリガー](08_collision_physics_triggers.md) | `CCollisionWorld2D`、`APhysicsBody2D`、trigger |
| 09 | [シーン遷移・ゲーム構造・セーブ](09_scene_flow_save.md) | scene stack、fade、flow、settings、save slot |
| 10 | [エフェクト（水・光・ステンシル・文字）](10_effects_light_stencil_text.md) | Effects2D、Light2D、stencil、font |

## 共通の実装境界

- asset parser は外部文字列を検証し、失敗時に既存状態を維持します。
- scene は必要な service を `WantedServices()` で要求します。
- node は component を単独所有します。sprite / tilemap texture は非所有参照、
  `AMeshComponent3D` の asset は shared ownership で保持するため、各 component の
  resource 契約に従います。
- UI、描画順、保存、音声は独立した責務として game 側で組み合わせます。
- subsystem は共有 owner、寿命、更新または終了処理を持つ service に限定します。
- 値計算と局所状態は subsystem へ移さず、値型または所有 object に置きます。

学習用実行例は現在同梱していません。再導入候補は
[`LearningSamplesMigrationPlan.md`](../LearningSamplesMigrationPlan.md) に集約します。
