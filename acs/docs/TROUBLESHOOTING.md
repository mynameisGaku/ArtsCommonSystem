# ACS トラブルシューティング

ビルドや実行でつまずいたときの、よくある症状と対処をまとめています。
初学者が最初に当たりやすい順に並んでいます。

---

## ビルドの前に

ACS のビルドには次が必要です（リポジトリ直下の `README.md` 参照）：

- **Visual Studio 2026** ＋ ワークロード **「C++ によるデスクトップ開発」**
  — これで MSVC コンパイラ・Windows SDK・**Ninja**・CMake が一括で入ります。
- build processはMSVC、Windows SDK、CMake、Ninjaを解決できるtoolchain環境を要求します。

---

## ビルド時のエラー

configureとbuildの診断はsource root、toolchain、依存取得、選択backendの順に確認します。

### `CMake Error: No such preset ... 'dx12-debug'`
preset解決のsource rootが`CMakePresets.json`を含んでいません。構成処理へpreset fileを
所有するACS source rootを渡し、要求したpreset名が定義済みか確認してください。

### `CMAKE_MAKE_PROGRAM is not set` / `Ninja` が見つからない
ビルドツール **Ninja** が PATH にありません。Visual Studio 2022 の
「C++ によるデスクトップ開発」ワークロードを入れると Ninja も同梱されます。
「x64 Native Tools Command Prompt for VS 2022」から実行すれば PATH が通ります。

### `Failed to clone` / 依存ライブラリの取得で構成が止まる
ACS は初回 configure 時に stb・cgltf・ufbx・dr_libs・Dear ImGui を GitHub から
自動取得します（`dx12-*` プリセットでも必要）。`diligent-*` ではさらに
DiligentCore 等も取得します。取得元へ接続できない、Git executableをPATHから解決できない、
またはproxy設定が取得処理へ渡らない場合、configureは依存取得の診断を返して停止します。

### `std::vector` などを書いたら大量のエラーが出る
**ACS は STL（標準テンプレートライブラリ）を使いません。** `std::vector` /
`std::string` / `std::unique_ptr` などは使えません。代わりに ACS の型を使います：

| STL | ACS の代替 | ヘッダ |
|---|---|---|
| `std::vector<T>` | `acs::TArray<T>` | `container/Array.h` |
| `std::string` | `acs::FString` | `container/String.h` |
| `std::unordered_map` | `acs::THashMap<K,V>` | `container/HashMap.h` |
| `std::unique_ptr<T>` | `acs::TUniquePtr<T>` | `memory/UniquePtr.h` |
| `std::shared_ptr<T>` | `acs::TRc<T>` | `memory/Rc.h` |

ACS は例外を無効化（`/D_HAS_EXCEPTIONS=0`）してビルドするため、`<vector>` など
STL ヘッダを include すると STL 内部の `throw` が原因で大量のテンプレート
エラーになります。エラーの中に `vector` や `xthrow` が見えたら STL の混入を疑って
ください。

### `looser exception specification` / フックのオーバーライドが通らない
`CApplication` のフック（`OnStart` / `OnUpdate` / `OnRender` / `OnShutdown` /
`OnEvent`）はすべて `noexcept` です。オーバーライド側も **`noexcept override`**
と書いてください。`noexcept` を付け忘れると基底と署名が合わず、
`looser exception specification` 等のコンパイルエラーになります。

### `use of deleted function`（コンテナを `=` でコピーした）
`TArray<T>` / `THashMap<K,V>` は意図しない重いコピーを防ぐため**コピー禁止**です。
copy constructionとcopy assignmentは削除され、明示的な複製は`Clone()`が所有値として返します。

### `unresolved external symbol`（リンクエラー）
targetのlink interfaceに、未解決symbolを所有するACS moduleが含まれていません。
`ACS::Game`はgame runtimeの10 moduleを集約し、`ACS::Audio`と`ACS::Imgui`は任意機能を
個別に提供します。

### `ImGui_ImplDX12_Init` などが未解決
ImGui実装はDX12 raw backendを必要とし、`ACS_RENDER_DX12_RAW=OFF`の構成では
backend symbolをlinkできません。

---

## 実行時の問題

runtime診断は生成構成、working directory、必須backend、asset rootを入力として切り分けます。

### ビルドは成功したのに `.exe` が見つからない
Visual Studio generator の実行ファイルと配布 DLL は
**`acs/Binaries/<構成>/<target>.exe`** のschemaで出力されます。対象構成またはtargetが異なると
期待した実行物は生成されないため、build結果に記録された構成名とtarget名を確認してください。

Ninja preset は単一構成のため、`acs/Binaries/` 直下へ出力します。

### アセット（画像・モデル・音声）が「見つからない」と言われる
ファイルパスは **実行時の作業ディレクトリ（カレントディレクトリ）からの相対**で
解決されます。IDE（Rider など）から実行すると作業ディレクトリがビルド出力
フォルダや別の場所になり、asset rootと一致しない場合があります。`Asset`カテゴリの
`TResult`は解決後の探索pathを返すため、working directoryと登録済みasset rootの不一致を
診断できます。

### `diligent-*` プリセットの初回 configure が固まったように見える
`diligent-*` プリセットは初回 configure 時に外部ライブラリ（DiligentCore など）を
取得します。取得中はconfigure logが進捗を所有し、完了後は同じdependency cacheを再利用します。
取得を完了できない場合は生成を確定せず、dependency errorを返します。

### IBL を使うと backend capability error になる
`CImageBasedLighting` の生成・描画経路は Diligent backend の機能です。
`ACS_RENDER_DILIGENT`が無効な構成ではcapability errorを返します。

---

## エラーメッセージの読み方

ACSのerror blockは失敗条件、診断message、source位置、call stackを分離して記録します。

### `ACS PANIC` ブロックが表示されてプログラムが落ちた
アサート失敗・致命的エラーです。出力は枠線で囲まれており、次の順で読みます：

1. **`expr:`** 行 — 失敗した条件式。まずここを見る。
2. **`message:`** 行 — 何が起きたかの説明。
3. **`location:`** 行 — 発生したファイルと行番号。
4. **stack trace** — そこに至るまでの呼び出し履歴（上が新しい）。

`expr` と `location` を見れば、どのコードのどの前提が崩れたかが分かります。

### `TResult` の `Value()` で停止した
`TResult<T,E>` がエラーを保持している状態で `Value()` を呼ぶと、`ACS_ASSERT`
で停止します（誤用を早期に検出するため）。`Value()`の事前条件は`IsErr()`または
`IsOk()`による成功確認です。

### `ACS_LOG_*` を呼んだのに何も出力されない
`CLogger` が初期化されていない可能性があります。`CApplication` を継承した
ゲームでは起動時に自動で初期化されるため通常は問題ありません。`CApplication`
の外で`CLogger`を直接所有する処理は、log出力より先にloggerを初期化する必要があります。

---

## それでも解決しないとき

- リポジトリ直下の `README.md`（必要なもの・ビルド手順）
- `docs/QUICKSTART.md`（主要 API の責務と失敗契約）
- `docs/ARCHITECTURE.md`（設計・モジュール構成）
- `tests/*_tests.cpp`（コア API の回帰契約）
