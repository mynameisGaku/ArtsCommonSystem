# ACS クイックスタート（C++ 初学者向け）

## 5 行でゲームを作る

`FApplication` を継承して 4 つの関数を実装すれば、ウィンドウ + 入力 + 描画 + ECS が
すぐ使えます。

```cpp
#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"

using namespace acs;

class MyGame : public FApplication {
public:
    void OnStart() noexcept override {
        // 起動時に 1 度だけ呼ばれる（リソース読み込みなど）
    }

    void OnUpdate(f32 dt) noexcept override {
        // 毎フレーム呼ばれる（ゲームロジック）
        if (FInput::IsKeyPressed(EKey::Escape)) Quit();
    }

    void OnRender() noexcept override {
        // 毎フレーム呼ばれる（描画コマンド）
    }

    void OnShutdown() noexcept override {
        // 終了時に 1 度だけ呼ばれる（後片付け）
    }
};

ACS_DEFINE_MAIN(MyGame)
```

`ACS_DEFINE_MAIN` がエントリポイント (`int main()`) を自動生成します。

## ビルド方法

`acs/` ディレクトリの中で、CMake プリセットを使って構成 → ビルド → 実行します
（PowerShell）。必要なツール（Visual Studio 2022・Ninja など）は README.md の
「必要なもの」を参照してください。

```pwsh
cd acs
cmake --preset dx12-debug          # 構成（初回のみ）
cmake --build --preset dx12-debug  # ビルド
.\cmake-build-debug\samples\01_HelloWindow\hello_window.exe   # 実行
```

ビルド成果物は `acs/cmake-build-debug/` 以下に出力されます（Ninja は単一構成
ビルドのため `Release/` `Debug/` のサブフォルダは作られません）。Rider など
IDE を使う場合は `acs/` を開けばプリセットが自動認識されます。

## モジュール一覧

下表は初学者がよく使う主要モジュールです。全 17 モジュールの一覧は
リポジトリ直下の `README.md` を参照してください。

| モジュール | 役割 | 主なクラス |
|---|---|---|
| Foundation | 基本型・エラー処理・ログ | `TResult<T,E>`, `FLogger`, `ACS_ASSERT` |
| Threading | 並列処理 | `TAtomic<T>`, `FMutex`, `FThreadPool` |
| Memory | メモリ管理 | `FAllocator`, `FMemorySystem`, `TUniquePtr<T>`, `TRc<T>` |
| FContainer | コンテナ | `TArray<T>`, `FString`, `THashMap<K,V>` |
| Math | 数学 | `FVec2/3/4`, `FMat4`, `FQuat` |
| Platform | OS 層 | `FWindow`, `FInput`, `Time`, `FFileSystem` |
| Ecs | エンティティ・コンポーネント | `FWorld`, `FEntityId`, `Query<...>` |
| FAsset | アセット管理 | `FAssetRegistry`, 画像/メッシュ/音声ローダ, 非同期ロード |
| Render | 描画 | `FRenderer` (DX12) |
| App | アプリ枠組み | `FApplication`, `FAppConfig` |

## よく使うコード例

### 1. キー入力
```cpp
if (FInput::IsKeyDown(EKey::W)) move_forward();   // 押されている間
if (FInput::IsKeyPressed(EKey::Space)) jump();    // 押した瞬間
if (FInput::IsKeyReleased(EKey::F)) release();    // 離した瞬間
```

### 2. マウス
```cpp
FVec2 mouse = FInput::MousePos();
FVec2 delta = FInput::MouseDelta();
if (FInput::IsMouseButtonPressed(EMouseButton::Left)) shoot();
```

### 3. ECS
```cpp
// コンポーネントは FVec3 などを包んだ POD（samples/04_HelloECS と同じ流儀）
struct Position { FVec3 v; };
struct Velocity { FVec3 v; };

FWorld& w = GetWorld();
FEntityId player = w.Create();
w.Add<Position>(player, { FVec3{0, 0, 0} });
w.Add<Velocity>(player, { FVec3{1, 0, 0} });

w.Query<Position, Velocity>().Each([dt](FEntityId, Position& p, Velocity& v) {
    p.v.x += v.v.x * dt;
    p.v.y += v.v.y * dt;
    p.v.z += v.v.z * dt;
});
```

### 4. ファイル I/O
```cpp
auto data = FFileSystem::ReadAllBytes(L"data/save.bin");
if (data.IsErr()) {
    ACS_LOG_ERROR("save not found: %s", data.Error().message);
    return;
}
TArray<byte>& bytes = data.Value();
```

### 5. ログ出力
```cpp
ACS_LOG_INFO("プレイヤーが %s を装備しました", weapon_name);
ACS_LOG_WARN("HP が低い: %d", hp);
ACS_LOG_ERROR("セーブ失敗: %s", err_message);
```

### 6. メモリスナップショット出力
```cpp
FMemorySnapshot::WriteSvg(L"memdump.svg");  // ブラウザで開ける
FMemorySnapshot::WriteBmp(L"memdump.bmp");  // 画像ビューアで開ける
FMemorySnapshot::DumpToStdOut();             // コンソールへテキスト
```

## エラー処理の流儀

ACS は例外を使いません。失敗する関数は `TResult<T, FErrorCode>` を返します。
**`Value()` は成功時のみ呼べます** — `IsErr()` で確認せずに呼ぶと `ACS_ASSERT`
で停止します（アサート無効のリリースビルドでは未定義動作）。必ず下記のように
`IsErr()` を確認してから `Value()` を呼んでください。

```cpp
auto wr = FWindow::Create(cfg);
if (wr.IsErr()) {
    ACS_LOG_ERROR("FWindow 作成失敗: %s", wr.Error().message);
    return -1;
}
FWindow& w = wr.Value();
```

`ACS_TRY` マクロで早期 return も書けます：
```cpp
TResult<void> Setup() noexcept {
    ACS_TRY(FMemorySystem::Init(FMemorySystem::DefaultConfig()));
    ACS_TRY(FThreadPool::Init());
    return Ok();
}
```

## 発展的な使い方（レシピ集）

3D 描画・テクスチャ・メッシュ読み込み・アニメーション・シャドウ・パーティクル・
多言語対応・セーブデータなど、各機能のコピペ可能なコード例は
**[`RECIPES.md`](RECIPES.md)（逆引きレシピ集）** にまとめてあります。

## 次のステップ

サンプルは `acs/samples/` に 26 本あります。**学習順とそれぞれの説明は
[`samples/README.md`](../samples/README.md) にまとまっています。** 番号順に
読み進めるのがおすすめです：

- `01_HelloWindow` — ウィンドウ + 入力（最初の 1 本、73 行）
- `02_HelloSprite` 〜 `09_HelloParticles` — 2D 描画とゲームの基本（シェーダ記述不要）
- `10_HelloModel` 〜 `15_HelloAnimation` — `FStandardShader` での 3D（シェーダ記述不要）
- `16_HelloTriangle` 〜 `18_HelloTextured` — 低レベル RHI（HLSL を直接書く発展編）
- `19_HelloUI` 〜 `22_HelloNet` — UI・MVVM・ImGui・ネットワーク
- `23_HelloPbr` 〜 `26_HelloLightmap` — 上級グラフィックス（PBR / IBL など）

`24`〜`26` は Diligent バックエンド（`diligent-*` プリセット）が必要です。
各サンプルのビルド要件は `samples/README.md` を参照してください。

コア API（`TArray` / `TResult` / `FWorld` など描画以外）の使用例は
`acs/tests/*_tests.cpp` も参考になります。グラフィックス API の使い方は
上記サンプルと [`RECIPES.md`](RECIPES.md) を参照してください。

準備ができたら、`samples/01_HelloWindow` を真似て自分のゲーム用フォルダを作り、
`FApplication` を継承して開発を始めましょう。CMake の書き方は
`samples/README.md` の「自分のゲームを作る」節を参照してください。

## ゲームを配布する（ZIP 化）

完成したゲームを友だちに渡すときは、exe・依存 DLL・アセットをまとめた ZIP を
1 行で作れます。

自分のゲームの `CMakeLists.txt` に 1 行追加するだけ：

```cmake
add_executable(my_game main.cpp)
target_link_libraries(my_game PRIVATE ACS::Easy)
acs_package_game(my_game ASSETS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets)
```

ビルド後、`package` ターゲットで ZIP を作ります：

```pwsh
cmake --build build --config Release
cmake --build build --target package
# → build/ACS-0.1.0-win64.zip ができる
```

ZIP の中身：

```
my_game/
    my_game.exe
    d3dcompiler_47.dll        # 依存 DLL は自動収集
    ... (Diligent DLL など)
    assets/
        ... (ASSETS_DIR の中身)
```

ZIP を相手に渡し、`my_game.exe` をダブルクリックすればすぐ動きます。
`samples/00_HelloEasy/CMakeLists.txt` も同じ仕組みで `hello_easy` を ZIP 化
できるので、まずはサンプルで試して動作確認するのがおすすめです。
