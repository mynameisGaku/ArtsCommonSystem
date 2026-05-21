# ACS トラブルシューティング

ビルドや実行でつまずいたときの、よくある症状と対処をまとめています。
初学者が最初に当たりやすい順に並んでいます。

---

## ビルドの前に

ACS のビルドには次が必要です（リポジトリ直下の `README.md` 参照）：

- **Visual Studio 2022** ＋ ワークロード **「C++ によるデスクトップ開発」**
  — これで MSVC コンパイラ・Windows SDK・**Ninja**・CMake が一括で入ります。
- 以下のコマンドは **「x64 Native Tools Command Prompt for VS 2022」** または
  Rider などの IDE から実行すると、環境変数が整っていて確実です。

---

## ビルド時のエラー

### `CMake Error: No such preset ... 'dx12-debug'`
`cmake --preset` を実行する場所が違います。**`acs/` ディレクトリの中**で
実行してください（`CMakePresets.json` がある場所）。

```pwsh
cd acs
cmake --preset dx12-debug
```

### `CMAKE_MAKE_PROGRAM is not set` / `Ninja` が見つからない
ビルドツール **Ninja** が PATH にありません。Visual Studio 2022 の
「C++ によるデスクトップ開発」ワークロードを入れると Ninja も同梱されます。
「x64 Native Tools Command Prompt for VS 2022」から実行すれば PATH が通ります。

### `Failed to clone` / 依存ライブラリの取得で構成が止まる
ACS は初回 configure 時に stb・cgltf・ufbx・dr_libs・Dear ImGui を GitHub から
自動取得します（`dx12-*` プリセットでも必要）。`diligent-*` ではさらに
DiligentCore 等も取得します。`Failed to clone` などで構成が止まるときは：

- インターネット接続を確認する。
- **Git** がインストールされ PATH に通っているか確認する（`git --version`）。
- 社内・学校のプロキシ環境では Git 側のプロキシ設定が必要なことがあります。

### `std::vector` などを書いたら大量のエラーが出る
**ACS は STL（標準テンプレートライブラリ）を使いません。** `std::vector` /
`std::string` / `std::unique_ptr` などは使えません。代わりに ACS の型を使います：

| STL | ACS の代替 | ヘッダ |
|---|---|---|
| `std::vector<T>` | `acs::Array<T>` | `container/Array.h` |
| `std::string` | `acs::String` | `container/String.h` |
| `std::unordered_map` | `acs::HashMap<K,V>` | `container/HashMap.h` |
| `std::unique_ptr<T>` | `acs::UniquePtr<T>` | `memory/UniquePtr.h` |
| `std::shared_ptr<T>` | `acs::Rc<T>` | `memory/Rc.h` |

ACS は例外を無効化（`/D_HAS_EXCEPTIONS=0`）してビルドするため、`<vector>` など
STL ヘッダを include すると STL 内部の `throw` が原因で大量のテンプレート
エラーになります。エラーの中に `vector` や `xthrow` が見えたら STL の混入を疑って
ください。

### `looser exception specification` / フックのオーバーライドが通らない
`Application` のフック（`OnStart` / `OnUpdate` / `OnRender` / `OnShutdown` /
`OnEvent`）はすべて `noexcept` です。オーバーライド側も **`noexcept override`**
と書いてください。`noexcept` を付け忘れると基底と署名が合わず、
`looser exception specification` 等のコンパイルエラーになります。

### `use of deleted function`（コンテナを `=` でコピーした）
`Array<T>` / `HashMap<K,V>` は意図しない重いコピーを防ぐため**コピー禁止**です。
`Array<int> b = a;` のように書くとこのエラーになります。複製したいときは
明示的に `b = a.Clone();` を使ってください。

### `unresolved external symbol`（リンクエラー）
必要なモジュールがリンクされていません。サンプルや自作ゲームの
`CMakeLists.txt` で `target_link_libraries(... PRIVATE ACS::Game ...)` に
不足モジュールを足してください。`ACS::Game` は標準的なゲームに必要な 10
モジュールをまとめた集約ターゲットです。音声なら `ACS::Audio`、ImGui なら
`ACS::Imgui` を追加します（`samples/README.md`「自分のゲームを作る」参照）。

### `ImGui_ImplDX12_Init` などが未解決
`20_HelloMVVM` / `21_HelloImGui` は ImGui が DX12 raw backend 専用のため、
`diligent-*` プリセット**単独**ではビルドされません。`dx12-debug` /
`dx12-release` プリセットでビルドしてください。

---

## 実行時の問題

### ビルドは成功したのに `.exe` が見つからない
成果物は **`acs/cmake-build-<preset>/samples/<番号付き名>/<exe 名>.exe`** に
出力されます。例：

```
acs\cmake-build-debug\samples\01_HelloWindow\hello_window.exe
```

ACS は Ninja の**単一構成ビルド**を使うため、`Release/` `Debug/` といった
サブフォルダは作られません。

### アセット（画像・モデル・音声）が「見つからない」と言われる
ファイルパスは **実行時の作業ディレクトリ（カレントディレクトリ）からの相対**で
解決されます。IDE（Rider など）から実行すると作業ディレクトリがビルド出力
フォルダや別の場所になり、`data/...` のような相対パスが外れることがあります。
対処：

- アセットを実行ファイルの隣に置き、IDE の実行構成で「作業ディレクトリ」を
  その `.exe` のあるフォルダに設定する。
- または絶対パスを使う。

`Result` のエラー（`Asset` カテゴリ）のメッセージに探索したパスが出るので、
まずそれを確認してください。

### `diligent-*` プリセットの初回 configure が固まったように見える
`diligent-*` プリセットは初回 configure 時に外部ライブラリ（DiligentCore など）を
git clone するため **10 分前後かかります**。固まったように見えても中断せず
待ってください。2 回目以降はキャッシュが効いて高速です。
まずは外部依存のない `dx12-debug` プリセットを使うのがおすすめです。

### `24`〜`26` のサンプルが起動できない / ビルドされていない
`24_HelloBloom` / `25_HelloIbl` / `26_HelloLightmap` は Diligent backend 専用です。
`diligent-debug` / `diligent-release` プリセットでビルドしてください。

---

## エラーメッセージの読み方

### `ACS PANIC` ブロックが表示されてプログラムが落ちた
アサート失敗・致命的エラーです。出力は枠線で囲まれており、次の順で読みます：

1. **`expr:`** 行 — 失敗した条件式。まずここを見る。
2. **`message:`** 行 — 何が起きたかの説明。
3. **`location:`** 行 — 発生したファイルと行番号。
4. **stack trace** — そこに至るまでの呼び出し履歴（上が新しい）。

`expr` と `location` を見れば、どのコードのどの前提が崩れたかが分かります。

### `Result` の `Value()` で停止した
`Result<T,E>` がエラーを保持している状態で `Value()` を呼ぶと、`ACS_ASSERT`
で停止します（誤用を早期に検出するため）。`Value()` を呼ぶ前に必ず `IsErr()`
／ `IsOk()` で成功を確認してください：

```cpp
auto r = registry.Load(L"hero.png");
if (r.IsErr()) {
    ACS_LOG_ERROR("読み込み失敗: %s", r.Error().message);
    return;
}
auto asset = r.Value();   // ここに来た時点で成功が保証される
```

### `ACS_LOG_*` を呼んだのに何も出力されない
`Logger` が初期化されていない可能性があります。`Application` を継承した
ゲームでは起動時に自動で初期化されるため通常は問題ありません。`Application`
の外（テスト用の小さなコードなど）でログを使う場合は、先に `Logger` の初期化が
必要です。

---

## それでも解決しないとき

- リポジトリ直下の `README.md`（必要なもの・ビルド手順）
- `docs/QUICKSTART.md`（API の使い方）
- `docs/ARCHITECTURE.md`（設計・モジュール構成）
- `samples/`（26 個の動くサンプル）と `tests/*_tests.cpp`（コア API の使用例）
