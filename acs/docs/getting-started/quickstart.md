# クイックスタート

ACS 本体、ツール、テストを Windows x64 で生成し、動作を確認する手順です。

## 必要な環境

- Windows 10 または Windows 11 x64
- C++デスクトップ開発ワークロードを含む Visual Studio
- CMake 3.24 以上
- Python 3.8 以上
- Editor をビルドする場合は .NET 10 SDK
- PowerShell
- Git
- 初回の依存取得に利用できるネットワーク

## CMake 構成

現在のソースツリーでは学習用サンプルの移行が保留中のため、ビルド対象に含めず `ACS_BUILD_SAMPLES=OFF` を指定します。登録と置換の条件は[学習サンプル移行](../operations/samples/learning-sample-migration.md)で確認できます。

```pwsh
cd C:\path\to\acs
cmake -S .\engine -B .\Intermediate\vs `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DACS_BUILD_SAMPLES=OFF `
  -DACS_BUILD_TESTS=ON `
  -DACS_BUILD_TOOLS=ON
```

別の Visual Studio ジェネレーターを使う場合は、`cmake --help` の一覧に表示される名前を指定します。

## ビルドとテスト

```pwsh
cmake --build .\Intermediate\vs --config Debug
ctest --test-dir .\Intermediate\vs -C Debug --output-on-failure
```

Release 構成は `--config Release` でビルドします。実行ファイルと DLL は `Binaries/<構成>/`、中間生成物は `Intermediate/` に置かれます。

## Editor のビルド

```pwsh
dotnet build .\editor\AcsEditor\AcsEditor.csproj `
  --configuration Release `
  --runtime win-x64
```

Raw DX12 のネイティブプレビューを使う場合は、先に CMake の Release 構成で `acs_editor_abi.dll` をビルドします。Editor プロジェクトは `Binaries/Release/acs_editor_abi.dll` が存在すると出力ディレクトリへコピーします。

## ツールの実行

`ACS_BUILD_TOOLS=ON` では `acs_assetpack` が生成されます。

```pwsh
.\Binaries\Debug\acs_assetpack.exe help
```

## 次に読む文書

- [チュートリアル](../tutorials/README.md)
- [アーキテクチャ](../architecture/README.md)
- [アセットパック](../guides/assets/asset-pack.md)
- [機能・API リファレンス](../reference/index.html)
- [トラブルシューティング](troubleshooting.md)
- [学習サンプル移行](../operations/samples/learning-sample-migration.md)
