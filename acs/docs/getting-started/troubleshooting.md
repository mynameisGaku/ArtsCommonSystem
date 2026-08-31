# トラブルシューティング

## 登録されたサンプルが存在しない

`ACS_BUILD_SAMPLES=ON` の構成で、登録されたサンプルディレクトリが存在しない場合に停止します。現在のソースツリーをビルドする場合はサンプルを無効にします。

```pwsh
cmake -S .\engine -B .\Intermediate\vs -DACS_BUILD_SAMPLES=OFF
```

既存キャッシュに `ON` が残っている場合は、同じビルドディレクトリへ `OFF` を明示して再構成します。

登録と実在ディレクトリの全件監査、および保留解除の条件は[学習サンプル移行](../operations/samples/learning-sample-migration.md)を参照してください。

## Visual Studioジェネレーターが見つからない

インストール済みのVisual Studioに合うジェネレーターを指定します。

```pwsh
cmake -S .\engine -B .\Intermediate\vs -G "Visual Studio 17 2022" -A x64 -DACS_BUILD_SAMPLES=OFF
```

`cmake --help` のジェネレーター一覧に対象がない場合はCMakeを更新します。

## 任意バックエンドのSDKが見つからない

任意バックエンドごとに取得方法と実行条件が異なります。

- Steamworks はSDKを必要とし、`ACS_STEAMWORKS_SDK_DIR`、`ACS_STEAMWORKS_SDK_URL`、またはGit取得設定で指定します。
- ML ONNXは構成時に必要なパッケージを取得します。
- OpenXRはローダーのソースを取得します。ランタイムはOpenXR機能の実行時に必要です。

利用しないバックエンドは `OFF` にし、必要なバックエンドだけを個別に有効化します。

## `acs_editor_abi.dll` が読み込めない

次を確認します。

1. CMakeのRelease構成で `acs_editor_abi` がビルドされている。
2. `ACS_RENDER_DX12_RAW=ON` で構成されている。
3. `Binaries/Release/acs_editor_abi.dll` が存在する。
4. Editor と DLL がともに x64 である。
5. Editorを再ビルドし、DLLが出力ディレクトリへコピーされている。

## CMakeキャッシュが古い

バックエンド、ジェネレーター、SDKパスを変更した後に古い値が残る場合は、別の `Intermediate` サブディレクトリへ構成します。

```pwsh
cmake -S .\engine -B .\Intermediate\clean-check -DACS_BUILD_SAMPLES=OFF
```

既存ビルドディレクトリを削除する前に、必要な構成ログと生成物がないことを確認します。

## テストの失敗理由を確認したい

```pwsh
ctest --test-dir .\Intermediate\vs -C Debug --output-on-failure
ctest --test-dir .\Intermediate\vs -C Debug --show-only
```

個別の実行ファイルがある場合は `Binaries/Debug/` から直接実行し、最初の失敗を確認します。

## リファレンスの画面が崩れる

`docs/reference/index.html` は相対URLだけで構成され、ローカルでも閲覧できます。検索や画面遷移を含めて確認する場合は `docs/` をHTTPサーバーのルートとして開きます。

```pwsh
python -m http.server 8000 --directory .\docs
```

ブラウザーで `http://127.0.0.1:8000/reference/` を開きます。

リファレンスを再生成するときはHTTPサーバーを停止し、生成と検査の完了後に再開します。手順は[リファレンスの生成と復旧](../operations/reference/generation.md)を参照してください。
