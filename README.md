# Arts Common System (ACS)

> 日本のインディー / 学生開発者向けの、軽量モジュール式 C++20 ゲームフレームワーク。
> Originally developed by students at [Arts College Yokohama](https://www.kccollege.ac.jp/).

ACS はウィンドウ・入力・2D/3D 描画・ECS・アセット読み込み・音声・UI までを
そろえた、Windows / DirectX 12 向けの実用ゲームフレームワークです。
**「C++ 初学者でも触れる」と「実用的なクオリティ」の両立**を設計目標にしています。

---

## はじめに（初学者の方へ）

**まず [`acs/docs/QUICKSTART.md`](acs/docs/QUICKSTART.md) を読んでください。** `Application`
を継承して 4 つの関数を書くだけでゲームが動く、という流れを最短で説明しています。

下記の手順でビルドすると、最初のサンプル `01_HelloWindow`（ウィンドウを開く
だけの 73 行の例）が起動します。そのあとは
[`acs/samples/README.md`](acs/samples/README.md) の学習順に沿ってサンプルを
読み進めてください。ビルドや実行でつまずいたら
[`acs/docs/TROUBLESHOOTING.md`](acs/docs/TROUBLESHOOTING.md) を参照してください。

## 必要なもの

- **Windows 10 / 11（64-bit）**
- **Visual Studio 2022** — インストール時にワークロード
  **「C++ によるデスクトップ開発」** を選択してください。これで MSVC コンパイラ・
  Windows SDK・**Ninja**・CMake が一括で入ります（ACS のビルドにはこれら全部が必要です）。
- 単体の CMake を使う場合は **3.24 以上**。
- **Git** — ソースの取得に加え、初回ビルド時に依存ライブラリを GitHub から
  自動取得するため、ビルドにも必要です（VS の個別コンポーネントにも含まれます）。

## ビルドと実行

リポジトリ直下から、`acs/` ディレクトリに入って実行します（PowerShell）：

```pwsh
cd acs
cmake --preset dx12-debug          # 構成（初回のみ。CMakePresets.json を使う）
cmake --build --preset dx12-debug  # ビルド
.\cmake-build-debug\samples\01_HelloWindow\hello_window.exe   # 実行
```

- Rider / CLion / Visual Studio を使う場合は `acs/` を開けば `CMakePresets.json` の
  プリセット（`dx12-debug` など）が自動で認識されます。
- ビルド成果物は `acs/cmake-build-debug/` 以下に出力されます（Ninja は単一構成の
  ため `Debug/` `Release/` のサブフォルダは作られません）。
- 初回の構成では依存ライブラリ（stb / cgltf / ufbx / dr_libs / Dear ImGui）を
  GitHub から自動取得します（インターネット接続と Git が必要・数分かかります）。
- 単体テストの実行: `ctest --preset dx12-debug`

## モジュール一覧

ACS はモジュールの集合です。どのモジュールをビルドするかは
[`acs/modules.cmake`](acs/modules.cmake) で選択します（既定で下記すべて有効）。

| モジュール | 役割 | 主なクラス |
|---|---|---|
| `Foundation` | 基本型・エラー処理・ログ | `Result<T,E>`, `Logger`, `ACS_ASSERT`, `Panic` |
| `Threading`  | 並列処理 | `Atomic<T>`, `Mutex`, `ThreadPool`, `JobGraph` |
| `Memory`     | メモリ管理 | `Allocator`, `UniquePtr<T>`, `Rc<T>`, `MemorySnapshot` |
| `Container`  | コンテナ | `Array<T>`, `String`, `HashMap<K,V>`, `Span<T>` |
| `Math`       | 数学・衝突判定 | `Vec2/3/4`, `Mat4`, `Quat`, `Camera`, `Collision2D/3D` |
| `Test`       | テストフレームワーク | `ACS_TEST`, `EXPECT_*` |
| `Platform`   | OS 層 | `Window`, `Input`, `Time`, `FileSystem` |
| `Ecs`        | エンティティ・コンポーネント | `World`, `EntityId`, `Query<...>` |
| `Event`      | イベント駆動 | `TimerManager`, `MessageBroker`（pub/sub） |
| `Asset`      | アセット管理 | `AssetRegistry`, 画像/メッシュ/音声ローダ, 非同期ロード |
| `Render`     | 描画 | `Renderer`, `StandardShader`, `PbrShader`, `SpriteBatch`, `Font` |
| `App`        | アプリ枠組み | `Application`, `AppConfig`, `ACS_DEFINE_MAIN` |
| `Audio`      | 音声 | XAudio2 による WAV / MP3 再生 |
| `Network`    | 通信 | TCP ソケット |
| `Imgui`      | デバッグ UI | Dear ImGui 統合 |
| `Mvvm`       | データバインディング | MVVM（Observable / Binder） |
| `Ui`         | UI フレームワーク | 純正 Widget + レイアウト |

詳しい設計は [`acs/docs/ARCHITECTURE.md`](acs/docs/ARCHITECTURE.md) を参照。

## 設計方針

- **STL を使用しない** — `Array<T>`, `String`, `HashMap<K,V>`, `UniquePtr<T>`,
  `Rc<T>` などを自前実装（`std::vector` 等は使えません。詳細は ARCHITECTURE.md）
- **例外を使わない** — エラーは `Result<T, ErrorCode>` で戻り値として伝搬
- **詳細なエラー報告** — `ACS_ASSERT`、ファイル/行/関数名つき `Panic`、
  シンボル化済みスタックトレース
- **UE 風モジュールシステム** — `modules.cmake` でモジュールと機能を選択

## レンダラバックエンド

`CMakePresets.json` に 4 つのプリセットがあります：

| プリセット | バックエンド | 用途 |
|---|---|---|
| `dx12-debug` / `dx12-release` | 自前 DX12（既定） | Diligent 非依存で軽量。まずはこれ |
| `diligent-debug` / `diligent-release` | Diligent Engine 経由 | 将来の Vulkan 等クロス対応。一部の上級サンプル（`24_HelloBloom` / `25_HelloIbl` / `26_HelloLightmap`）で必要 |

> **注意:** `diligent-*` プリセットは初回 configure 時に外部ライブラリを取得する
> ため **10 分前後かかります**。固まったように見えても待ってください。

## ドキュメント

- [`acs/docs/QUICKSTART.md`](acs/docs/QUICKSTART.md) — **初学者向け**。ここから始める
- [`acs/docs/RECIPES.md`](acs/docs/RECIPES.md) — 各機能の逆引きレシピ集（3D描画・音・UI ほか）
- [`acs/samples/README.md`](acs/samples/README.md) — 26 サンプルの一覧と推奨学習順
- [`acs/docs/ARCHITECTURE.md`](acs/docs/ARCHITECTURE.md) — 設計思想・モジュール構成
- [`acs/docs/TROUBLESHOOTING.md`](acs/docs/TROUBLESHOOTING.md) — よくあるエラーと対処

## ライセンス

教育目的での使用に限定されています。商用利用・外部配布は事前許可が必要です。
