# ACS クイックスタート（C++ 初学者向け）

## 5 行でゲームを作る

`Application` を継承して 4 つの関数を実装すれば、ウィンドウ + 入力 + 描画 + ECS が
すぐ使えます。

```cpp
#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"

using namespace acs;

class MyGame : public Application {
public:
    void OnStart() noexcept override {
        // 起動時に 1 度だけ呼ばれる（リソース読み込みなど）
    }

    void OnUpdate(f32 dt) noexcept override {
        // 毎フレーム呼ばれる（ゲームロジック）
        if (Input::IsKeyPressed(Key::Escape)) Quit();
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

CMake で構成 → ビルド → 実行：

```bash
cmake -B build -S acs
cmake --build build --config Release
./build/samples/HelloWindow/Release/hello_window.exe
```

## モジュール一覧

| モジュール | 役割 | 主なクラス |
|---|---|---|
| Foundation | 基本型・エラー処理・ログ | `Result<T,E>`, `Logger`, `ACS_ASSERT` |
| Threading | 並列処理 | `Atomic<T>`, `Mutex`, `ThreadPool` |
| Memory | メモリ管理 | `Allocator`, `MemorySystem`, `UniquePtr<T>`, `Rc<T>` |
| Container | コンテナ | `Array<T>`, `String`, `HashMap<K,V>` |
| Math | 数学 | `Vec2/3/4`, `Mat4`, `Quat` |
| Platform | OS 層 | `Window`, `Input`, `Time`, `FileSystem` |
| Ecs | エンティティ・コンポーネント | `World`, `EntityId`, `Query<...>` |
| Asset | アセット管理 | `AssetRegistry`, `BinaryAsset` |
| Render | 描画 | `Renderer` (DX12) |
| App | アプリ枠組み | `Application`, `AppConfig` |

## よく使うコード例

### 1. キー入力
```cpp
if (Input::IsKeyDown(Key::W)) move_forward();   // 押されている間
if (Input::IsKeyPressed(Key::Space)) jump();    // 押した瞬間
if (Input::IsKeyReleased(Key::F)) release();    // 離した瞬間
```

### 2. マウス
```cpp
Vec2 mouse = Input::MousePos();
Vec2 delta = Input::MouseDelta();
if (Input::IsMouseButtonPressed(MouseButton::Left)) shoot();
```

### 3. ECS
```cpp
struct Position { f32 x, y, z; };
struct Velocity { f32 dx, dy, dz; };

World& w = GetWorld();
EntityId player = w.Create();
w.Add<Position>(player, {0, 0, 0});
w.Add<Velocity>(player, {1, 0, 0});

w.Query<Position, Velocity>().Each([dt](EntityId, Position& p, Velocity& v) {
    p.x += v.dx * dt;
    p.y += v.dy * dt;
    p.z += v.dz * dt;
});
```

### 4. ファイル I/O
```cpp
auto data = FileSystem::ReadAllBytes(L"data/save.bin");
if (data.IsErr()) {
    ACS_LOG_ERROR("save not found: %s", data.Error().message);
    return;
}
Array<byte>& bytes = data.Value();
```

### 5. ログ出力
```cpp
ACS_LOG_INFO("プレイヤーが %s を装備しました", weapon_name);
ACS_LOG_WARN("HP が低い: %d", hp);
ACS_LOG_ERROR("セーブ失敗: %s", err_message);
```

### 6. メモリスナップショット出力
```cpp
MemorySnapshot::WriteSvg(L"memdump.svg");  // ブラウザで開ける
MemorySnapshot::WriteBmp(L"memdump.bmp");  // 画像ビューアで開ける
MemorySnapshot::DumpToStdOut();             // コンソールへテキスト
```

## エラー処理の流儀

ACS は例外を使いません。失敗する関数は `Result<T, ErrorCode>` を返します。

```cpp
auto wr = Window::Create(cfg);
if (wr.IsErr()) {
    ACS_LOG_ERROR("Window 作成失敗: %s", wr.Error().message);
    return -1;
}
Window& w = wr.Value();
```

`ACS_TRY` マクロで早期 return も書けます：
```cpp
Result<void> Setup() noexcept {
    ACS_TRY(MemorySystem::Init(MemorySystem::DefaultConfig()));
    ACS_TRY(ThreadPool::Init());
    return Ok();
}
```

## 次のステップ

- `samples/HelloWindow` を実行してウィンドウが開くか確認
- `tests/*_tests.cpp` を読んで API の使い方を学ぶ
- 自分のゲームのフォルダを作って `Application` を継承
