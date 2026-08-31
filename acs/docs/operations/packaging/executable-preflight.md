# 実行ファイルの事前検証

`PackageExecutableContract` は、パッケージへ格納する実行ファイルを起動せずに検証します。パッケージ作成前と完成したZIPの検証時に同じ契約を適用します。

## PE形式

実行ファイルは次の条件をすべて満たす必要があります。

- PE32+のAMD64実行イメージであり、DLLではない。
- エントリーポイントが0ではなく、実行可能なコード節のファイル範囲または仮想範囲に含まれる。
- サブシステムがWindows GUIまたはWindowsコンソールである。
- DOSヘッダー、PEヘッダー、節テーブル、各節の範囲がファイル内で整合している。
- PE32+の`NumberOfRvaAndSizes`が省略可能ヘッダー内のデータディレクトリ数を超えない。

形式、節、アーキテクチャ、エントリーポイントを検証できない場合は `EXECUTABLE_INVALID` になります。

## 決定的なPE正規化

製品情報とアプリケーションマニフェストは、Release実行ファイルの非公開な複製へだけ反映します。リソース更新後は、次の変動フィールドを0へ正規化してディスクへフラッシュします。

- COFFヘッダーの`TimeDateStamp`
- PE Optional Headerの`CheckSum`
- 訪問した各`IMAGE_RESOURCE_DIRECTORY`の`TimeDateStamp`
- 各`IMAGE_DEBUG_DIRECTORY`の`TimeDateStamp`

正規化後は実行ファイルを再度解析し、PE構造、`VERSIONINFO`の言語と完全なバイト列、アプリケーションマニフェストを期待値へ照合します。変動フィールドだけが異なる同じ実行ファイルは、製品情報の反映後に同じバイト列になります。

## リソース走査の上限

PEリソースは次の上限内で走査します。

| 対象 | 上限 |
|---|---:|
| リソースディレクトリ全体 | 64 MiB |
| 1件の`VERSIONINFO`またはアプリケーションマニフェスト | 4 MiB |
| 1つのリソースツリーで訪問できる異なるディレクトリ | 4,096 |
| 1つのリソースツリーで処理できるエントリの合計 | 16,384 |

同じディレクトリオフセットの再利用、循環、範囲外参照、重複するリソース識別子、上限超過はすべて拒否します。1つのディレクトリが4,096件を超えるエントリを宣言した場合も拒否します。

## アプリケーションマニフェスト

組み込みアプリケーションマニフェストはDTDを無効にして解析します。Windowsの`assembly`として有効で、AMD64互換またはアーキテクチャ非依存であり、実効設定が`asInvoker`かつ`uiAccess=false`でなければなりません。

複数言語のマニフェスト、壊れたXML、`highestAvailable`、`requireAdministrator`は `EXECUTABLE_MANIFEST_INVALID` になります。互換マニフェストが存在しない場合は、非公開の準備領域にある複製へ決定的なAMD64用マニフェストを追加します。元のRelease実行ファイルは変更しません。

## 製品情報

`VERSIONINFO` はリソースID 1、言語`0x0409`の1件へ正規化されます。`ProductName`、`ProductVersion`、`CompanyName`、`FileDescription`、`LegalCopyright`、`SupportUrl`に加え、`FileVersion`と`OriginalFilename`を格納します。`OriginalFilename`はパッケージへ格納する`.exe`のファイル名だけを受理します。

製品バージョンは3つの数値要素を持つSemVerとし、プレリリース識別子とビルド識別子を付けられます。Windowsの固定バージョンには数値部を`major.minor.patch.0`として格納し、`major`、`minor`、`patch`はそれぞれ0〜65,535に制限します。元のSemVer全体は`ProductVersion`へ保持します。

複数言語の内容が一致しない場合、必須フィールドが欠ける場合、正規リソースのバイト列が一致しない場合、またはパッケージマニフェストと読み戻した値が一致しない場合は `EXECUTABLE_METADATA_INVALID` になります。

[パッケージ作成の概要](overview.md)へ戻る。
