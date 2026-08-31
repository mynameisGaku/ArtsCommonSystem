# Steamworks 連携

ACS は `ISteamworksBridge` を Game Framework の安定した境界として提供し、既定の代替実装と任意の実バックエンドを切り替えます。Steamworks SDK が不要な構成でも、同じインターフェースを使ったゲームコードをビルドできます。

## 構成

| 構成 | CMake | 実装 |
|---|---|---|
| 代替実装 | `ACS_BUILD_STEAMWORKS=OFF` | `FSteamworksBridgeStub` |
| 実バックエンド | `ACS_BUILD_STEAMWORKS=ON` | `CSteamworksBridgeImpl` と `ACS::Steamworks` |

実バックエンドは Steamworks SDK の `public/steam/steam_api.h`、`steam_api64.lib`、`steam_api64.dll` を使用します。SDK は `ACS_STEAMWORKS_SDK_DIR`、`ACS_STEAMWORKS_SDK_URL`、または `ACS_STEAMWORKS_SDK_GIT` と `ACS_STEAMWORKS_SDK_GIT_TAG` で指定できます。

```pwsh
cmake -S .\engine -B .\Intermediate\steam `
  -DACS_BUILD_SAMPLES=OFF `
  -DACS_BUILD_STEAMWORKS=ON `
  -DACS_STEAMWORKS_SDK_DIR=C:\SDK\Steamworks
cmake --build .\Intermediate\steam --config Release
```

## ビルド対象への追加

```cmake
target_link_libraries(my_game PRIVATE ACS::Game ACS::GameFramework ACS::Steamworks)
acs_steamworks_runtime(my_game APPID 480)
```

`acs_steamworks_runtime` は実バックエンドのときだけ次を設定します。

- `steam_api64.dll` をビルド後に実行ファイルの隣へ複製
- `steam_appid.txt` を出力ディレクトリへ生成
- インストール規則へ DLL と `steam_appid.txt` を登録

`APPID` を省略した場合は開発用の既定値480を使用します。配布構成では対象アプリケーションの App ID を明示します。

## 初期化と更新

実バックエンドを既定値として使う場合は、起動時に `acs::steamworks::InstallSteamworksAsDefault()` を呼びます。その後は `acs::game::GetDefaultSteamworksBridge()` で接続境界を取得し、`IsInitialized()` で利用可否を確認します。アプリケーションの所有者は毎フレーム `Tick()` でコールバックを進め、終了時に `Shutdown()` を呼びます。

接続境界が返す借用文字列は、少なくとも次の `Tick()` までは有効です。それより長く保持する場合は呼び出し側で複製します。

## API の領域

`ISteamworksBridge` は次の領域をまとめます。

- ローカルプレイヤーとリッチプレゼンス
- 実績と統計
- ランキング操作
- DLC と利用権
- フレンド情報
- クラウドファイルと容量
- Workshop の購読操作
- 音声録音と入力

個別の関数と結果型は[機能・API リファレンス](../../reference/index.html)から参照できます。

## 代替実装の用途

`FSteamworksBridgeStub` は SDK、Steam クライアント、アカウントに依存しない検査とオフライン実行に使用します。未初期化は `kSubSteamworksNotInitialized`、未実装操作は `kSubSteamworksNotImplemented` を返します。代替実装と実バックエンドは同じインターフェースを共有しますが、Steam 側の状態は再現しません。
