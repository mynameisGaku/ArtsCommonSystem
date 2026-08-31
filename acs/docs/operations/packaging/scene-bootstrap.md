# 正規シーンの起動情報

パッケージ作成は、プロジェクトの起動シーンを`canonicalSceneAssetId`で識別します。パスだけを根拠に別のシーンを選ぶことはありません。

## `canonicalSceneAssetId`

`canonicalSceneAssetId`は、0ではないGUIDを区切り記号なしの小文字32桁で保持します。値が空の場合は `CANONICAL_SCENE_ASSET_ID_REQUIRED`、形式が不正または0の場合は `CANONICAL_SCENE_ASSET_ID_INVALID` で拒否します。

互換用の`initialScene`は引き続きプロジェクトに残りますが、`canonicalSceneAssetId`が示すAsset DBのシーンと同じファイルへ解決されなければなりません。一致しない場合は `CANONICAL_SCENE_PATH_MISMATCH` です。`canonicalSceneAssetId`を検証できない状態で`initialScene`へ暗黙に切り替えません。

## `sceneBootstrap`

Cookは正規シーンをパッケージルートの`main.acscene`として格納し、`package-manifest.json`へ次の起動情報を記録します。

| フィールド | 必須値 |
|---|---|
| `sceneBootstrap.path` | `main.acscene` |
| `sceneBootstrap.contract` | `acs.scene.bootstrap.v1` |
| 2Dの`sceneBootstrap.sourceFormat` | `legacy-acscene-v1` |
| 3Dの`sceneBootstrap.sourceFormat` | `legacy-acs3d-v2` |
| 2Dの`sceneBootstrap.adapterProjectionHint` | `orthographic` |
| 3Dの`sceneBootstrap.adapterProjectionHint` | `perspective` |

`adapterProjectionHint`は互換読込用の補助値であり、シーンやカメラの正本ではありません。

## 失敗時閉鎖

ZIP検証では、次の状態をすべて不正として拒否します。

- `canonicalSceneAssetId`、正規シーン種別、インポーター情報、依存関係グラフハッシュの欠落または不正
- `sceneBootstrap`の欠落
- `path`または`contract`の不一致
- `sourceFormat`が2つの対応形式以外
- `sourceFormat`と`adapterProjectionHint`の組み合わせが不一致
- 正規シーンの内容、参照、対応範囲を検証できない状態

実行時に`game.acpak`が存在する場合、マウント、CRC、解析、依存関係、起動情報の失敗から個別アセットへ暗黙に切り替えません。2Dと3Dを相互に自動変換せず、各形式を対応する互換読込経路で処理します。

[Cookスナップショットと公開境界](cook-snapshot.md)と[パッケージ作成の概要](overview.md)も参照してください。
