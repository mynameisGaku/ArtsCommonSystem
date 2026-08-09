# ACS クイックスタート（C++ 初学者向け）

## 5 行でゲームを作る

`CApplication` を継承して 4 つの関数を実装すれば、ウィンドウ + 入力 + 描画 + ECS が
すぐ使えます。

```cpp
#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"

using namespace acs;

class CMyGame : public CApplication {
public:
    void OnStart() noexcept override {
        // 起動時に 1 度だけ呼ばれる（リソース読み込みなど）
    }

    void OnUpdate(f32 dt) noexcept override {
        // 毎フレーム呼ばれる（ゲームロジック）
        if (CInput::IsKeyPressed(EKey::Escape)) Quit();
    }

    void OnRender() noexcept override {
        // 毎フレーム呼ばれる（描画コマンド）
    }

    void OnShutdown() noexcept override {
        // 終了時に 1 度だけ呼ばれる（後片付け）
    }
};

ACS_DEFINE_MAIN(CMyGame)
```

`ACS_DEFINE_MAIN` がエントリポイント (`int main()`) を自動生成します。

## ビルド方法

`acs/` ディレクトリの中で、生成スクリプトから構成 → ビルド → 実行します
（PowerShell）。必要なツール（Visual Studio・CMake など）は README.md の
「必要なもの」を参照してください。

```pwsh
cd acs
.\generate.ps1 -Tests -Tools
cmake --build Intermediate/vs --config Debug --target acs_unit_tests
ctest --test-dir Intermediate/vs -C Debug --output-on-failure
```

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

## よく使うコード例

### 1. キー入力
```cpp
if (CInput::IsKeyDown(EKey::W)) move_forward();   // 押されている間
if (CInput::IsKeyPressed(EKey::Space)) jump();    // 押した瞬間
if (CInput::IsKeyReleased(EKey::F)) release();    // 離した瞬間
```

### 2. マウス
```cpp
FVec2 mouse = CInput::MousePos();
FVec2 delta = CInput::MouseDelta();
if (CInput::IsMouseButtonPressed(EMouseButton::Left)) shoot();
```

### 3. ECS
```cpp
// コンポーネントは FVec3 などを包む値型として定義する
struct FPosition { FVec3 v; };
struct FVelocity { FVec3 v; };

CWorld& w = GetWorld();
FEntityId player = w.Create();
w.Add<FPosition>(player, { FVec3{0, 0, 0} });
w.Add<FVelocity>(player, { FVec3{1, 0, 0} });

w.Query<FPosition, FVelocity>().Each([dt](FEntityId, FPosition& p, FVelocity& v) {
    p.v.x += v.v.x * dt;
    p.v.y += v.v.y * dt;
    p.v.z += v.v.z * dt;
});
```

### 4. ファイル I/O
```cpp
auto data = CFileSystem::ReadAllBytes(L"data/save.bin");
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
    ACS_LOG_ERROR("Window 作成失敗: %s", wr.Error().message);
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

scene、node、component を使ったゲーム構成は [`tutorials/README.md`](tutorials/README.md)、
機能別のコードは [`RECIPES.md`](RECIPES.md) へ進みます。コア API の実行契約は
`acs/tests/*_tests.cpp` で確認できます。

学習用実行例は現在同梱していません。再導入候補と検証条件は
[`LearningSamplesMigrationPlan.md`](LearningSamplesMigrationPlan.md) に集約します。
ゲーム側では `CApplication` または `CGame` の派生型を作り、CMake target を
`ACS::App` または `ACS::GameFramework` へリンクします。

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
cmake --build build --config Release --target package
# → build/ACS-0.1.0-win64.zip ができる
```

ZIP は `acs_package_game()` で宣言したゲーム実行物だけを収録し、private な
FetchContent 依存の SDK や CMake metadata は混入させません。実際に必要な依存 DLL と
アセットに加え、ACS と静的リンクされた第三者実装の license を同梱します。

ZIP の中身：

```
my_game/
    my_game.exe
    d3dcompiler_47.dll        # 依存 DLL は自動収集
    ... (Diligent DLL など)
    assets/
        ... (ASSETS_DIR の中身)
Licenses/
    ACS-License.txt
    ThirdParty/
        ... (利用中の第三者licenseとnotice)
```

ZIP を相手に渡し、`my_game.exe` をダブルクリックすればすぐ動きます。
`acs_package_game()` は登録したゲーム target と必要な runtime、asset、license を
同じ契約で検証して ZIP 化します。
