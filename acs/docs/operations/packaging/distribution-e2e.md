# 配布E2E検証

`distribution-e2e`は、実際のパッケージ作成、検証、解析、比較、改変検出を1つの配布境界として確認します。

```pwsh
dotnet run --project tools/acspackage -- distribution-e2e
```

## 保存先

検証用プロジェクトと生成物はOSの`TEMP`直下にある新しい専用ディレクトリへ保存します。`--artifacts`を指定する場合も、`TEMP`より下にある未作成の通常ディレクトリでなければなりません。既存パス、`TEMP`外のパス、再解析ポイントを含む祖先は拒否します。

```pwsh
dotnet run --project tools/acspackage -- distribution-e2e `
  --artifacts "$env:TEMP\acs-distribution-e2e-check"
```

## 検証手順

1. 正規の3Dシーンと製品情報を持つACSプロジェクトを作成する。
2. `Shipping`のパッケージを独立した出力先へ2回生成する。
3. 2つのZIPのSHA-256が一致し、決定的な配布物になっていることを確認する。
4. 1つ目のZIPへ`verify --report`を実行し、終了コード0と`verified: true`を確認する。
5. `inspect --json`を実行し、4項目の製品情報が完全に保持されていることを確認する。
6. 2つのZIPへ`diff --json`を実行し、終了コード0と`identical: true`を確認する。
7. ZIP内の実行ファイルを[実行ファイルの事前検証](executable-preflight.md)で確認する。
8. 複製したZIP内の`Config/ProjectSettings.ini`だけを変更し、マニフェストを更新しない改変版を作る。
9. 改変版の`verify`と`inspect`が終了コード1と`verified: false`を返すことを確認する。
10. 正常版と改変版の`diff`が終了コード3と`compared: false`を返すことを確認する。

検証結果は`distribution-e2e-summary.json`へ保存します。概要には各コマンドの終了コード、正常版と改変版のSHA-256、製品情報、実行ファイルのPE情報を含めます。途中で失敗した場合も、失敗概要を同じ保存先へ書きます。

[パッケージ作成の概要](overview.md)へ戻る。
