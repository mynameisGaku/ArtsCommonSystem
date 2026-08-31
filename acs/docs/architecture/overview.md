# ACS アーキテクチャ

ACS は、Windows x64 / ARM64 を受理する C++20 ゲームフレームワークと、Windows x64 向け Editor で構成されます。C++ 側は小さなモジュールを依存関係で組み合わせ、ゲーム固有コードから描画、アセット、入力、シーン管理を選択して利用できる構造です。

## レイヤー

| レイヤー | 主なモジュール | 責務 |
|---|---|---|
| 基盤 | `Foundation`, `Container`, `Math` | 基本型、エラー、文字列、コンテナ、数学 |
| 実行基盤 | `Platform`, `Threading`, `Memory` | OS 境界、並行実行、メモリ管理 |
| ゲーム基盤 | `Asset`, `Ecs`, `Render`, `App` | アセット、エンティティ、描画、アプリケーションループ |
| ゲーム構造 | `GameFramework`, `Collision`, `Audio`, `Ui`, `Mvvm` | シーン、ノード、ゲーム機能、衝突、音声、UI |
| 任意バックエンド | `Steamworks`, `Scripting`, `MlOnnx`, `OpenXr`, `CrashWin`, `TelemetryFile`, `LocalMatch` | 外部ランタイムや実装差し替え |
| 編集 | `EditorAbi`, `AcsEditor` | ネイティブプレビューと WPF Editor |

`src/<module>/Module.cmake` が各 C++ モジュールのソース、公開ヘッダー、依存先を定義します。`engine/CMakeLists.txt` はこれらを検出し、`engine/modules.cmake` の有効化設定と照合してCMakeターゲットを生成します。

## 標準リンク境界

`ACS::Game` は次の標準モジュールをまとめる `INTERFACE` ターゲットです。

`ACS::Game` は `Foundation`、`Threading`、`Memory`、`Container`、`Math`、`Platform`、`Asset`、`Ecs`、`Render`、`App` を直接リンクします。各モジュール間の依存関係は、それぞれの `Module.cmake` が定義します。

`GameFramework`、`Audio`、`Network`、`Ui` などはゲームが必要に応じて追加します。任意バックエンドは対応する `ACS_BUILD_*` オプションが `ON` のときだけ実装ターゲットを生成します。

## 実行時の責務

1. `App` がウィンドウ、イベント、フレーム進行を管理します。
2. `Render` が RHI と描画機能を提供します。
3. `FGame` と `FSceneManager` がシーンの更新、固定更新、描画、遷移を管理します。
4. `FScene` がワールドスコープのサービスとサブシステムを所有します。
5. `ANode` が親子ツリーと `FTransform3D` を持ち、`AComponent` を所有します。

所有権を移す API と参照だけを借りる API は型で区別します。寿命を持つオブジェクトは `TObjectPtr`、単独所有は `TUniquePtr`、寿命を延長しない参照は `TWeakObjectPtr` または非所有ポインタを使います。

## 描画バックエンド

Raw DX12は既定で有効です。Diligentバックエンドは `ACS_RENDER_DILIGENT=ON` で組み込みます。`FDeviceConfig::backend == Auto` はD3D12を選択します。Vulkanを使用するには `ACS_DILIGENT_VULKAN=ON` でビルドし、`backend` に `ERhiBackendKind::Vulkan` を指定します。Vulkanを組み込んでいない構成で明示指定した場合は初期化に失敗します。Raw DX12とDiligentの選択はビルド時に確定します。

詳細は[描画バックエンド](render-backends.md)を参照してください。

## Editor 境界

`AcsEditor` は `net10.0-windows`、WPF、x64 のアプリケーションです。C++ 側の `acs_editor_abi.dll` を P/Invoke し、Raw DX12 の描画面を Editor のウィンドウへホストします。Editor の詳細は[Editor アーキテクチャ](editor.md)を参照してください。

## 関連文書

- [ソース配置](source-layout.md)
- [基盤機能の所有権](foundation-ownership.md)
- [ゲームフレームワーク](game-framework.md)
- [ノードモデル](node-model.md)
- [機能・API リファレンス](../reference/index.html)
