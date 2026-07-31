# 学習用サンプルへの全面移行計画

## 目的

`samples` にある既存68サンプルは、学習順、前提知識、検証方法が一貫した新しい
学習用サンプルへ全面的に置き換えます。移行完了時には既存サンプルをすべて削除し、
旧ディレクトリや旧番号への互換リンクは残しません。Git履歴は過去の参照先として保持します。

対象はACS本体の `acs/samples` だけです。CardGameとゲーム固有コードは変更しません。
日常の生成、build、testは[Windowsプロジェクト操作](ProjectOperations.md)の統一入口を使用します。

## 移行原則

- 一つのサンプルでは一つの中心概念を扱い、前段のサンプルだけを前提にする。
- 新機能はACSの既存module、memory、event、subsystem、build機構を使う。
- サンプル専用の代替実装を作らず、公開APIだけで実現する。
- 実装を持つ主要classは同名headerとcppへ分ける。
- 型、関数、変数には短く具体的な日本語コメントを付ける。
- 関数呼び出し、制御文、cast、initializerの丸括弧内は一行にする。
- 各サンプルへ学習目標、前提、実行方法、期待結果、失敗例、練習課題を記載する。
- サンプルは説明資料であり、回帰試験の代替にはしない。重要契約はunit testにも固定する。

## 新しい構成

新サンプルは目的別のtrackへ分けます。通し番号だけに依存せず、フォルダー名から学ぶ内容が
分かる構成にします。

| Track | 内容 |
|---|---|
| `01_foundation` | 型、結果、memory、container、delegate、event、timer、threading |
| `02_application` | application lifecycle、window、input、file、storage、asset |
| `03_2d` | sprite、text、audio、scene、node、component、collision、physics、tilemap、UI |
| `04_3d` | mesh、camera、material、light、shadow、animation、asset loading |
| `05_rendering` | RHI、shader、buffer、texture、render target、post process、水面、空、雲、fog |
| `06_services` | network、scripting、telemetry、crash report、package、optional backend |
| `07_tools` | ImGui、MVVM、editor extension、asset tool、profiling |
| `08_integration_labs` | 複数moduleを組み合わせる小規模な完成例 |

各trackには、画面を必要としない最小例を先に置き、その後に対話的な例を置きます。
任意依存を使う例は、無効構成でもstubまたは明確なskip結果を返します。

## 移行手順

### 1. 既存coverageを固定する

既存68サンプルについて、利用module、公開API、必要backend、入力asset、期待出力、
現在のbuild・実行状況を機械可読な台帳へ記録します。未検証の説明は完了扱いにしません。

### 2. 共通templateと検証器を作る

新サンプル用の最小CMake、`main.cpp`、必要なApp class、READMEをtemplate化します。
監査器では次を確認します。

- READMEの必須項目と前提サンプルが存在する。
- CMake登録とsource fileが一致する。
- 公開API、型名、コメント、丸括弧内一行の規約に違反しない。
- optional featureの有効・無効条件がCMakeとREADMEで一致する。
- 旧サンプル名を新しい文書やsourceが参照しない。

### 3. 基盤サンプルを先に置き換える

Foundation、Container、Memory、Event、Timer、Threading、Platform、Asset、Scriptingを
画面不要のサンプルから移します。DebugとReleaseでbuild・runし、終了値と標準出力を固定します。

### 4. 描画とapplicationサンプルを置き換える

window、2D、3D、RHI、UI、editorの順に移します。画面を持つサンプルは、起動確認だけでなく
代表フレームの画像、描画領域、入力後の状態を検証します。

### 5. optional backendと統合例を置き換える

Lua、Steamworks、ONNX、OpenXR、telemetry、crash reportなどを個別サンプルへ分けます。
最後に、学習済みの機能だけを組み合わせるintegration labを追加します。

### 6. 旧サンプルを一括削除する

全既存サンプルに置換先があり、下記の完了条件を満たしたcommitでのみ、旧68ディレクトリ、
旧CMake登録、旧文書参照、旧生成solution登録をまとめて削除します。途中段階では新旧を
併存させ、常にbuild可能な状態を保ちます。

## 完了条件

- 旧68サンプルのディレクトリと参照が0件である。
- 旧サンプルが担っていた公開APIとbackendのcoverageに、すべて新しい置換先がある。
- 新サンプルはDebugとReleaseでbuildし、画面不要のものは実行結果まで一致する。
- DX12、Diligent、optional feature ON/OFFの対象matrixがすべて通る。
- 対話的サンプルは代表画像と主要操作後の状態を確認する。
- tutorial、quickstart、reference、README、solution生成、配布物の参照が新構成と一致する。
- 単一headerと通常headerの外部consumerが、対応する学習サンプルをbuild・linkできる。
- 旧サンプル名の再導入を監査で検出する。
- 独立source reviewと配布物検証が完了している。

## 削除を始めない条件

Scriptingなど現在の統合検証に使っているサンプルは、対応する新サンプルと同等以上の
ON/OFF、Debug/Release、実backend、配布consumer検証が揃うまで削除しません。
ファイル数を減らすことより、学習経路と検証coverageの連続性を優先します。

## 最終リポジトリ整理wave

学習用サンプルへの移行後、ACSの全ファイルを次のいずれかへ分類します。

### 基準inventory

整理開始時の比較基準は `origin/main` のtracked fileとし、現在固定したcommit
`02f3f10305f1f879115a7faa67039436cd8bc1f2` の基準値は1,848件です。
内訳はACS配下1,801件、root直下12件、screenshots 26件、scripts 5件、dist 4件です。
主な拡張子はC++ source 609件、header 578件、C# 220件、PNG 83件、Markdown 82件、
text 71件、JavaScript 39件、CMake 35件、PowerShell 35件、HTML 34件、Python 21件です。
`acs/samples` は387ファイル、`acs/tools` は16ファイル、`acs/scripts` は47ファイルで、
trackedされたEXEとDLLは0件です。この値は削除数の目標ではなく、分類漏れと意図しない増減を
検出する比較基準です。実施時には対象commitを固定し、同じ集計を再生成して台帳へ保存します。
rootの `screenshots` にある26枚のPNGは、現在のrepo内からbasenameで参照されず、旧Hello
sampleに由来します。新trackの代表画像と操作後画像を再生成してcoverageを移した後、旧68
sampleの一括削除waveで全削除する候補とします。参照0だけを削除根拠にはせず、新画像の
生成手順、owner、visual regression gateまで台帳へ記録します。

| 分類 | 保持方針 |
|---|---|
| 実行必須 | 製品runtime、公開API、module、assetなど、実行に直接必要なものを保持する。 |
| 再現必須 | clean build、code generation、test、release、配布物再現に必要なsourceと設定を保持する。 |
| 便利tool | 製品と再現に不要な配布用toolだけを `tools/bin` へ整理する。 |
| 文書 | 新しい責務構成と一致するMarkdown、tutorial、referenceだけを保持して全面改稿する。 |
| 不要 | 参照、生成、検証、配布のどれにも必要ないと実測できたものを全削除する。 |

`tools/bin` に置く便利toolは、実行ファイル、version、SHA-256、licenseだけで再現元を追跡できる
形にします。一方、build、code generation、test、releaseを担うtool sourceは再現必須なので
削除または実行物だけへの置換を行いません。
現在の `acs/tools` にある `acs_assetpack`、`acsassetdb`、`acsbuild`、`acspackage` と
`capture_window.ps1` は、module生成検査、package E2E、asset pipeline、画面検証のどれを
担っているか個別に確認します。これらを一括して便利toolへ分類しません。責務を置き換える
正規sourceと同等以上の再現gateがない限り、再現必須としてsourceを保持します。
rootの `fix_render_backend_rename.py`、`rename_enums_to_e_prefix.py` と、`acs/scripts` の
rename・revert系8本は一時migration tool候補です。参照0、現在のsourceへ再適用不要、
履歴以外の文書依存なしを確認した項目だけ削除候補にします。editorの `test_*` と `shot_*`
は参照0でも手動回帰runnerの可能性があるため、test caseのownerとCIまたは新しいvisual
runnerへの置換を確認するまで保持します。

reference、dist、screenshotsなど生成物候補161件も一括削除しません。台帳の
`source-of-truth` と `generated-from` が有効で、生成後のbyteまたは意味的な一致を検証できる
項目だけを生成物として扱います。
`bt_serialize_test.btg` とmemory dump画像はtestの実行時成果物として生成されるため、
trackedする正本ではありません。clean-up対象としてignore、cleanup、安全な再生成と
非配布確認を同じgateへ固定します。

全削除と再配置は、責務別folderへの移動先と参照更新を含む機械可読manifestとして作成します。
その台帳は各tracked fileについて、現在path、分類、責務owner、正規sourceか生成物か、
`source-of-truth`、`generated-from`、参照元、移動先または削除理由、検証gateを持ちます。正規sourceを持たない
生成物や、生成元を削除する移動は完了扱いにしません。
正規treeへ適用する前に隔離cloneで次を順番に通します。

1. source、build、docs、reference、generated fileの参照scan
2. DebugとReleaseの全build・test
3. optional featureと描画backendの対象matrix
4. 単一ヘッダー生成と検証
5. 一時配布先へのpackage再現
6. 外部consumerのbuild・link・run
7. 保持した便利toolのsmoke test、version、SHA-256、license照合
8. 旧path、旧sample名、削除対象参照が0件であることの再監査

この隔離検証を完了したdelete manifestだけを正規treeへ適用し、Markdownとreferenceを
新しいfolder構成へ同じcommitで更新します。
