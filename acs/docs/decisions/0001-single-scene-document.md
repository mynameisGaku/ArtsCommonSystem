# 設計判断 0001: 単一シーンドキュメント

## 状態

採用済みです。

## 背景

ACS Editor は、2D と 3D の表示切り替えを文書の切り替えとして扱いません。選択、未保存状態、取り消し、やり直し、自動保存、復旧、再生、パッケージ作成が別々のシーンを参照すると、同じプロジェクト内で編集内容と起動内容が分岐するためです。

一方、ACS の 2D と 3D には別々の変換、描画、物理、直列化の仕組みがあります。単一の文書識別子を採用しても、`.acscene` を `.acs3d` の別名として扱ったり、2D の実行時領域を 3D へ暗黙変換したりはしません。

## 判断

1. ACS Editor は 1 つの論理シーンドキュメントと、1 つの安定した文書識別子を所有します。
2. `Perspective` と `Orthographic` は `Scene View` の表示設定です。投影、操作用カメラ、選択状態、操作器、作業領域の配置はシーン内容の識別子に含めません。
3. 新しい `3d`、`blank`、`2d` プロジェクトは、いずれも `Assets/main.acs3d` へ `ACS3D v2` を書き込みます。`blank` は `3d` の互換別名です。`2d` だけが初期表示に XY 正面の `Orthographic` 表示設定を選びます。
4. 既存の `.acscene` は `legacy-acscene-v1` 互換アダプター、既存の `.acs3d` は `legacy-acs3d-v2` 互換アダプターで扱います。表示方式の変更では互換アダプター、元形式、データ本体を変更しません。
5. `.acsproject.initialScene` は互換用のパス、`.acsproject.canonicalSceneAssetId` は `Asset Database` 上の永続的な起動シーン識別子です。両者は同じアセット記録へ解決されなければなりません。
6. `Game.DefaultScene` は互換期間中の実行時設定です。復旧後の `.acsproject.initialScene` と一致しない場合は、別のシーンへ暗黙に切り替えず、プロジェクトを開く処理を失敗させます。
7. `SceneWorldDocumentEnvelope` は 2D と 3D の互換状態を 1 つの ACS Editor 一括処理として保存・復旧する内部形式です。配布シーン形式ではありません。
8. 読み込んだ元データを公開するまでは描画面を表示せず、以前のフレームまたは既定の 2D フレームも見せません。元データが設定されていない新規 3D 文書は `ACS3D v2` と `Perspective` で開始します。
9. `Scene View` は ACS Editor の操作用カメラだけを所有します。`Game View` はシーンに設定されたカメラを決定的に選び、`Scene View` と `Game View` の切り替えでは再生状態または操作用カメラの姿勢を変更しません。

## 互換表

| 入力 | ACS Editor 文書 | 初期表示 | `sceneBootstrap.sourceFormat` |
|---|---|---|---|
| 新規 `3d` | 1 つの `ACS3D v2` 元データ | `Perspective` | `legacy-acs3d-v2` |
| 新規 `blank` | 1 つの `ACS3D v2` 元データ | `Perspective` | `legacy-acs3d-v2` |
| 新規 `2d` | 1 つの `ACS3D v2` 元データ | `Orthographic` | `legacy-acs3d-v2` |
| 既存 `.acs3d` | 1 つの 3D 互換元データ | プロジェクトと表示の状態 | `legacy-acs3d-v2` |
| 既存 `.acscene` | 1 つの 2D 互換元データ | `Orthographic` | `legacy-acscene-v1` |

未変換の `.acscene` では `Perspective` を選択できません。`SetPosition2D` と `SetScale2D` は `ANode` の Z 成分を保持し、`SetRotation2D` は回転全体を Z 軸回転へ置き換えます。2D の値を `FTransform3D` へ直接代入して変換済みとみなしません。

## 起動シーンの識別

`canonicalSceneAssetId` は 0 ではない GUID を区切りなしの小文字 32 桁で保持します。パスだけを根拠に起動シーンを選ばず、`Asset Database` で識別子とパスが同じ記録へ解決されることを確認します。欠落、不正形式、0、パスとの不一致は失敗です。

`Cook` 後のシーンは互換用の仮想パス `main.acscene` へ格納します。このパスから元形式を推測してはいけません。`package-manifest.json` の `sceneBootstrap` が次を明示します。

| 項目 | 値 |
|---|---|
| `sceneBootstrap.path` | `main.acscene` |
| `sceneBootstrap.contract` | `acs.scene.bootstrap.v1` |
| 2Dの`sceneBootstrap.sourceFormat` | `legacy-acscene-v1` |
| 3Dの`sceneBootstrap.sourceFormat` | `legacy-acs3d-v2` |
| 2Dの`sceneBootstrap.adapterProjectionHint` | `orthographic` |
| 3Dの`sceneBootstrap.adapterProjectionHint` | `perspective` |

`adapterProjectionHint` は互換アダプターの起動補助であり、シーンに設定されたカメラまたは ACS Editor の現在の表示を上書きするシーン付加情報ではありません。詳細な `Cook` 境界は[正規シーンの起動情報](../operations/packaging/scene-bootstrap.md)で定義します。

## 自動変換を行わない条件

`.acscene` を開く操作、表示方式の切り替え、保存、再生、パッケージ作成では、`.acs3d` への自動変換を行いません。明示的な変換機能を追加する場合は、次の条件をすべて満たす必要があります。

- 未対応の命令と参照を変換前に列挙する。
- 安定したアセット識別子とノード識別子を保持する。
- 変換元を上書きせず、新しい出力先へ書く。
- 情報を失う対応付けをすべて診断する。
- 変換済み文書全体を公開するか、以前の文書を完全に維持する。
- 保存、意味を保つ往復保存、復旧、再生、単独実行、パッケージの結果を検証する。

不明な版、未対応の命令、正規識別子の欠落、パスとの不一致、互換データ本体の部分復旧は安全側で失敗させます。失敗時は直前に公開済みの文書を維持するか、利用不能な中立表示領域を表示します。

## カメラ表示

`Game View` はシーンに設定されたカメラを、有効指定、優先度、安定したカメラ識別子、ノード識別子の順で選びます。`Camera View` はこの選択を永続化せず、共有表示面へ一時的なプレビュー要求を出します。

`CameraViewRequestsV1` は最大 8 件の論理要求を保持しますが、物理的な表示担当は同時に 1 件だけです。別の要求を表示する前に現在の表示担当を解除し、主 `Game View` へ表示面を返す前にも一時指定を解除します。カメラまたは表示寸法が変わると描画先の世代と履歴の世代を更新し、以前のフレーム履歴を再利用しません。

この契約は、8 画面の同時ライブ出力、要求ごとの画面外描画先、非同期読み戻しを提供しません。これらには独立した描画先管理が必要です。

## 互換アダプターを廃止できる条件

`legacy-acscene-v1` または `legacy-acs3d-v2` は、次の条件が揃うまで削除しません。

- 2D と 3D の変換、描画、物理を 1 つの文書ルートで保持できる正規ワールド形式がある。
- `.acscene` と `.acs3d` の意味を保存する基準データの往復確認が成功する。
- 取り消し、やり直し、自動保存、復旧、再生、単独実行、パッケージが正規形式で同じ結果になる。
- ACS Editor 専用の命令について、実行時アダプターとパッケージ起動確認の対応範囲が明示されている。
- 旧形式の読み込み失敗が診断され、部分変換または暗黙の代替処理を行わない。
- `Camera View` を含む最初の画面提示を実フレームで確認できる。

## 検証

```pwsh
dotnet run --project .\editor\AcsEditor\AcsEditor.csproj -c Release -- `
  --scene-editor-migration-selftest
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\verify_editor.ps1 -Mode full
```

`SceneContractFixtureSelfTest` は、新規プロジェクトひな型、表示設定と元データの分離、`canonicalSceneAssetId`、`sceneBootstrap`、互換データ本体の一括往復、複数カメラの選択順、未知形式と不正な `SceneWorldDocumentEnvelope` の拒否を固定します。状態と順序の自己検査に加え、配布判定では実フレームの最初の画面提示も確認します。
