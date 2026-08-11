# ACS クイックスタート（C++ 初学者向け）

この文書はapplication寿命、build入力、主要API、配布物のACS契約をまとめます。

## Application lifecycle

`CApplication` は window、入力、描画、ECS のapplication寿命を所有します。
`OnStart`、`OnUpdate`、`OnRender`、`OnShutdown` が起動、frame更新、描画、終了の
各phaseを受け持ち、`ACS_DEFINE_MAIN` が登録した派生型のentry pointを生成します。

`ACS_DEFINE_MAIN` がエントリポイント (`int main()`) を自動生成します。

## ビルド方法

生成処理はACS source root、有効化するtests／tools、build構成を入力として受け取ります。
必要なツール（Visual Studio・CMakeなど）が不足する場合、または構成名やtargetが無効な場合は
処理を停止し、configure logまたはbuild／test出力へ理由を記録します。

実行ファイルと配布 DLL は `acs/Binaries/<構成>/`、中間生成物は
`acs/Intermediate/vs/` に分離されます。生成された `ACSEngine.slnx` を開けば
Visual Studio からも同じ構成をビルドできます。

## モジュール一覧

下表は初学者がよく使う主要モジュールです。全モジュールの一覧は
リポジトリ直下の `README.md` を参照してください。

| モジュール | 役割 | 主なクラス |
|---|---|---|
| Foundation | 基本型・エラー処理・ログ | `TResult<T,E>`, `ACS_LOG_*`, `ACS_ASSERT` |
| Threading | 並列処理 | `TAtomic<T>`, `FMutex`, `FThreadPool` |
| Memory | メモリ管理 | `FAllocator`, `FMemorySystem`, `TUniquePtr<T>`, `TRc<T>` |
| Container | コンテナ | `TArray<T>`, `FString`, `THashMap<K,V>` |
| Math | 数学 | `FVec2/3/4`, `FMat4`, `FQuat` |
| Platform | OS 層 | `FWindow`, `CInput`, Time API, `CFileSystem` |
| Ecs | エンティティ・コンポーネント | `CWorld`, `FEntityId`, `TQueryView<...>` |
| Asset | アセット管理 | `CAssetRegistry`, 画像/メッシュ/音声ローダ, 非同期ロード |
| Render | 描画 | `CRenderer` (DX12 / Diligent) |
| App | アプリ枠組み | `CApplication`, `FAppConfig` |

## 主要 API の責務

主要moduleの入口は、保持状態、所有権、失敗結果を明示して呼び出し側へ返します。

### 1. キー入力

`CInput::IsKeyDown` は保持状態、`IsKeyPressed` と `IsKeyReleased` はframe間のedgeを返します。

### 2. マウス

`MousePos` は現在位置、`MouseDelta` は前frameからの移動量を返し、mouse button APIは
保持状態とpress/release edgeを区別します。

### 3. ECS

`CWorld` がentityとcomponent storageを所有し、`Create`が安定handleを発行します。
`Add<T>` はcomponent値を登録し、`Query<T...>` は必要なcomponentを持つentityだけを走査します。

### 4. ファイル I/O

`CFileSystem::ReadAllBytes` は成功時に所有byte列、失敗時に診断可能なerrorを返します。
`TResult::Value` は成功確認後だけ参照できます。

### 5. ログ出力

`ACS_LOG_INFO`、`ACS_LOG_WARN`、`ACS_LOG_ERROR` はseverityを付けて現在のlog sinkへ記録します。

### 6. メモリスナップショット出力

`FMemorySnapshot` は現在のmemory診断をSVG、BMP、標準出力へ固定形式で出力します。

## エラー処理の流儀

ACS は例外を使いません。失敗する関数は `TResult<T, FErrorCode>` を返します。
**`Value()` は成功時のみ呼べます** — `IsErr()` で確認せずに呼ぶと `ACS_ASSERT`
で停止します（アサート無効のリリースビルドでは未定義動作）。`IsErr()` を確認してから
`Value()` を呼んでください。

`ACS_TRY` は失敗した `TResult` のerrorを呼び出し元へ早期returnし、成功時だけ後続処理を続けます。

## 次のステップ

コア API の実行契約は `acs/tests/*_tests.cpp` で確認できます。

学習用実行例は現在同梱していません。再導入候補と検証条件は
[`LearningSamplesMigrationPlan.md`](LearningSamplesMigrationPlan.md) に集約します。
ゲーム側では `CApplication` または `CGame` の派生型を作り、CMake target を
`ACS::App` または `ACS::GameFramework` へリンクします。

## ゲーム配布アーカイブ

`acs_package_game`は登録済みゲームtargetとasset rootを入力として受け取り、選択したbuild構成の
実行物、runtime依存、asset、licenseを配布アーカイブへまとめます。targetが未登録、asset rootが
不正、または必須runtimeが不足する場合はpackage処理を失敗させます。

ZIP は `acs_package_game()` で宣言したゲーム実行物だけを収録し、private な
FetchContent 依存の SDK や CMake metadata は混入させません。実際に必要な依存 DLL と
アセットに加え、ACS と静的リンクされた第三者実装の license を同梱します。

ZIP の中身：

```
<game>/
    <game>.exe
    <runtime dependencies>
    <asset root>/
Licenses/
    ACS-License.txt
    ThirdParty/
        <required licenses and notices>
```

アーカイブのentrypointは登録したゲーム実行物です。package処理は必要なruntime、asset、licenseを
同じ契約で検証してから出力を確定します。
