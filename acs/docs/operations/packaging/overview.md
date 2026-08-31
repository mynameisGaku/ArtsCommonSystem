# パッケージと配布物

ACS Editor の `Build > Package Project` と `tools/acspackage` は、同じパッケージ作成機能を使ってプロジェクトを配布用 ZIP へ変換します。

## 処理境界

1. プロジェクト、正規シーン、パッケージメタデータを検証する。
2. Release 実行ファイルをビルドするか、`--skip-build` で既存実行ファイルを検証する。
3. ルートシーンから配布用変換の閉包を構築する。
4. 持ち運び可能な仮想パスへ変換し、`game.acpak` を生成する。
5. ネイティブ検証器で全項目、CRC、マニフェストを確認する。
6. 非公開の準備領域へ実行ファイル、実行時 DLL、`game.acpak`、`Config` を配置する。
7. `package-manifest.json` と ZIP を決定的に生成する。
8. 公開済み ZIP の複製を非公開ディレクトリで検証し、起動確認を実行する。

元の実行ファイルや公開済み ZIP をその場で変更しません。公開直前に配布用変換の閉包を再計算し、スナップショット取得後に必須アセットが変わった場合は失敗します。

## 作成プロファイル

| プロファイル | 内容 |
|---|---|
| `Development` | 非圧縮 `game.acpak`、確認用の個別変換済みアセット、`main.acscene` |
| `Test` | LZ4 圧縮した `game.acpak` を正本とし、任意でゲーム用 PDB を含める |
| `Shipping` | 圧縮パックを正本とし、個別アセットと PDB を配布 ZIP へ含めない |

パックが存在する場合、実行時は `main.acscene` を起動項目として読み込みます。2D は `ACSCENE v1`、対応する 3D は `ACS3D v2` として処理します。マウント、CRC、解析、依存関係の失敗時に個別アセットへ暗黙の代替を行いません。

## コマンドライン

```pwsh
dotnet run --project tools/acspackage -- package `
  C:\Games\MyGame\MyGame.acsproject `
  --version 1.0.0 `
  --profile Shipping

dotnet run --project tools/acspackage -- validate `
  C:\Games\MyGame\MyGame.acsproject `
  --version 1.0.0

dotnet run --project tools/acspackage -- smoke `
  C:\Games\MyGame\Build\Packages\MyGame-1.0.0-win64.zip `
  --timeout-seconds 45
```

`--include-symbols` は `Development` / `Test` でゲーム用 PDB だけを追加します。`--skip-build` は既存の Release 実行ファイルを使用します。

## メタデータと実行ファイル

任意の `Config/PackageMetadata.json` が保持する配布メタデータは、`publisher`、`description`、`copyright`、`supportUrl` の4項目です。これらは実行ファイルの `CompanyName`、`FileDescription`、`LegalCopyright`、`SupportUrl` へそれぞれ反映します。`ProductName` は `project.Name`、`ProductVersion` は `--version` で渡したパッケージ作成オプションから設定します。パッケージ作成機能は非公開の準備領域内にある実行ファイルだけを更新し、更新後の資源を読み戻してマニフェストと一致することを確認します。

実行ファイルの形式、アーキテクチャ、エントリーポイント、マニフェスト、実行時依存関係を公開前に検証します。`Shipping` のアプリケーションマニフェストは `asInvoker`、`uiAccess=false` を必要とします。

PE形式とリソース走査の上限は[実行ファイルの事前検証](executable-preflight.md)に記載しています。

## Cookと公開

Cookは正規シーンから必要なアセットを決定し、入力内容と依存関係をハッシュで固定します。ZIPを公開する直前にプロジェクト、`Config`、正規シーン、必要アセットのグラフを再検証し、同一性を証明できない場合は `PROJECT_CHANGED_DURING_PACKAGE` で公開を拒否します。

詳細は[Cookスナップショットと公開境界](cook-snapshot.md)を参照してください。正規シーンの識別子と `sceneBootstrap` の検証規則は[正規シーンの起動情報](scene-bootstrap.md)に記載しています。

## 起動確認

起動確認は公開 ZIP を読み取り専用で複製し、非公開ディレクトリへ展開して非表示プロセスとして実行します。時間切れ、終了コード、準備完了通知、子プロセス、出力上限、後片付けをレポートへ記録します。時間切れまたは取り消し時は管理対象プロセスを終了し、未管理のプロセスを成功扱いしません。

展開先と容量上限は[起動確認用の非公開展開](private-extraction.md)、準備完了通知とプロセス終了条件は[起動確認プロトコル](launch-smoke.md)を参照してください。配布処理全体の再現性と改変検出は[配布E2E検証](distribution-e2e.md)で確認します。

パッケージ作成の前に[アセットのパッケージ準備確認](asset-package-readiness.md)を実行し、メタデータは[Project Settings のパッケージ情報](project-settings-metadata.md)で編集します。
