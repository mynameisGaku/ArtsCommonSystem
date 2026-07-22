# はじめに — FGame + FScene2D で最小の 2D アプリ

`FGame`（アプリ本体）と `FScene2D`（実用的な 2D シーン基底）の組み合わせは、ACS で 2D ゲームを始めるときの一番素直な入口です。
ウィンドウを開き、毎フレーム更新・描画し、入力・カメラ・物理（コリジョン）の配線まで面倒を見てくれます。
「まず動くウィンドウを出して 1 フレーム描く」ところから、ライフサイクルと座標系の感覚を掴むのがこのチュートリアルの目的です。

- `FGame` = `acs::FApplication` を継承し、`FSceneManager` を駆動する**アプリの器**。やることは「最初の `FScene` を返す」だけ。
- `FScene2D` = root ノードツリー / 共有 `FSpriteBatch` / `Default2D | Camera2D | Physics2D` サービスを束ねた**シーン基底**。
- 入口マクロは `ACS_GAME_MAIN(YourGame)`。これだけで `main` / `WinMain` が生成されます。

---

## 最小例

これだけでウィンドウが開き、暗い背景に 1 個の四角（スプライト）が描かれます。コピペでビルド可能です。

```cpp
// MyFirst.cpp
#include "gameframework/GameFramework.h"   // FGame / FScene2D / ANode など一式

using namespace acs;
using namespace acs::game;

namespace {

class FHelloScene final : public FScene2D
{
public:
    // シーンが top に来てサービス attach 済みの最初の 1 回。初期化はここで。
    void OnReady() noexcept override
    {
        SetPixelsPerUnit(64.0f);                 // 1 ワールド単位 = 64px（zoom=1 時）
        GetGame().SetClearColor(0.04f, 0.045f, 0.055f);

        // 0.9 x 0.9 ワールド単位の水色スプライトを 1 個、原点に置く
        auto node = NewObject<ANode>();
        node->SetPosition2D(FVec2{0.0f, 0.0f});
        node->AddComponent<ASprite2DComponent>(FVec2{0.9f, 0.9f},
                                               FVec4{0.15f, 0.85f, 1.0f, 1.0f});
        Root().AddChild(Move(node));

        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
        Services().Camera().SetZoom(1.0f);
    }

    // 毎フレーム（dt は秒、time scale 反映済み）
    void OnTick(f32 /*dt*/) noexcept override {}
};

class FHelloGame final : public FGame
{
protected:
    // FGame が唯一要求するのはこれ。最初に push する FScene を返す。
    TUniquePtr<FScene> InitialScene() noexcept override
    {
        return MakeUnique<FHelloScene>();
    }
};

} // namespace

ACS_GAME_MAIN(FHelloGame)   // main / WinMain を自動生成
```

CMake 側（`samples/55_HelloScene2D/CMakeLists.txt` と同型）:

```cmake
add_executable(my_first WIN32 MyFirst.cpp)
acs_apply_compiler_options(my_first)
target_link_libraries(my_first PRIVATE ACS::GameFramework)
```

> 注意: ライフサイクルのフックは**すべて `noexcept override`** で宣言してください。基底が `noexcept` なので、付け忘れるとコンパイルエラーになります。

---

## 主要 API

### FGame（`gameframework/Game.h`）

| メンバ | 説明 |
| --- | --- |
| `TUniquePtr<FScene> InitialScene() noexcept` | **override 必須**。最初に push する `FScene` を返す。 |
| `FSceneManager& Scenes()` | シーンスタック。`Scenes().ChangeScene(...)` で遷移（次フレーム頭で適用）。 |
| `void SetTimeScale(f32 s)` | `OnTick`/`OnFixedTick` に渡る `dt` への乗数。0 でポーズ相当。 |
| `void SetFixedTimestep(f32 fixed_dt, u32 max_steps = 8)` | 固定刻みの長さと 1 フレーム最大ステップ数。既定 `1/60`, `8`。 |
| `void TransitionTo(TUniquePtr<FScene>, f32 out=0.3f, f32 in=0.3f)` | フェード付きシーン遷移（実時間進行、ポーズ中でも進む）。 |
| `void SetClearColor(f32 r,g,b,a=1)` | 背景クリア色（`FApplication` 由来）。次フレームから反映。 |
| `void Quit()` | メインループを抜ける（`FApplication` 由来）。 |

`SetClearColor` / `Quit` は基底 `acs::FApplication` のメソッドです（`app/Application.h`）。

### FScene2D（`gameframework/Scene2D.h`）— override するフック

| フック | いつ呼ばれる | 用途 |
| --- | --- | --- |
| `OnReady() noexcept` | シーン開始時 1 回（`OnEnter` の中） | 初期化・ノード生成・入力/物理セットアップ |
| `OnTick(f32 dt) noexcept` | 毎フレーム | 通常ロジック。`dt` は秒・time scale 反映済み |
| `OnFixedTick(f32 fixed_dt) noexcept` | 固定刻み（0〜複数回/フレーム） | 物理など決定的に進めたい更新 |
| `OnDrawWorld(FRenderContext&, FSpriteBatch&) noexcept` | 毎フレーム描画（カメラ適用後） | ワールド座標での追加描画 |
| `OnDrawHud(FRenderContext&, FSpriteBatch&) noexcept` | 毎フレーム描画（スクリーン座標） | UI / HUD。左上原点・ピクセル単位 |

> `FScene2D` は `FScene` の `OnEnter/OnUpdate/OnFixedUpdate/OnRender` を**既に実装済み**で、その中から上記 `OnReady/OnTick/...` を呼びます。普通はこれら 5 つだけ override すれば十分です。

### FScene2D のアクセサ

- `ANode& Root()` — シーンの root ノード。`Root().AddChild(Move(node))` でツリーに足す。
- `FSpriteBatch& SpriteBatch()` — world/HUD 共有のスプライトバッチ。
- `void SetPixelsPerUnit(f32)` / `f32 PixelsPerUnit()` — 1 ワールド単位のピクセル数（既定 64）。
- `FVec2 ScreenToWorld(FVec2 screen_px)` — マウス座標（左上原点）→ ワールド。**ピッキングはこれを使う**（`FCamera2D::ScreenToWorld` は ppu 非考慮なので不可）。
- `u32 ScreenWidth()` / `ScreenHeight()` — 直近 `OnRender` でキャッシュした画面サイズ。

### FScene 共通（`gameframework/Scene.h`）

- `FGame& GetGame()` — アプリ本体へ。`GetGame().Quit()` / `SetClearColor()` / `SetTimeScale()` など。
- `FSceneManager& Scenes()` — 遷移用。
- `FSceneServices& Services()` — `WantedServices()` で要求したサービス群。`FScene2D` は `Default2D | Camera2D | Physics2D` を要求済み。

### Services()（`gameframework/SceneServices.h`）`FScene2D` で使えるもの

- `Services().Input()` → `FInputMap`（キー/軸バインド）
- `Services().Camera()` → `FCamera2D`（`SetPosition` / `SetZoom` / `SetTargetPos`）
- `Services().Physics()` → `FCollisionWorld2D`（AABB 等のコリジョン）
- `Services().Clock()` / `Tweens()` / `Sequences()` — `Default2D` に含まれる（補間・演出）
- `Services().Triggers()` は `FScene2D` 既定では**要求していない**（`Triggers` ビットを足したシーンでのみ可）

---

## よく使うパターン

実例は `samples/55_HelloScene2D/Scene2DStarter.cpp`（スクショ確認済み）から抜粋・整理したものです。

### 1. 入力バインドと終了（`OnReady` + `OnTick`）

```cpp
constexpr FActionId kMoveX("MoveX");
constexpr FActionId kQuit("Quit");

void OnReady() noexcept override
{
    FInputMap& input = Services().Input();
    input.ClearAll();
    input.BindAxisKeys(kMoveX, EKey::A, EKey::D);  // A=-1, D=+1
    input.BindKey(kQuit, EKey::Escape);
}

void OnTick(f32 /*dt*/) noexcept override
{
    if (Services().Input().IsPressed(kQuit)) { GetGame().Quit(); return; }
    const f32 mx = Services().Input().Axis(kMoveX);  // -1..+1
    // mx を使って移動…
}
```

### 2. ノード生成のヘルパ（タイルや壁を量産）

```cpp
void AddBox(FVec2 pos, FVec2 size, FVec4 color) noexcept
{
    auto n = NewObject<ANode>();
    n->SetPosition2D(pos);                       // ワールド座標
    n->AddComponent<ASprite2DComponent>(size, color);
    Root().AddChild(Move(n));                          // 所有権はツリーへ
}
```

`ANode::Local()` は `FTransform3D`。2D では `Position2D()` / `Rotation2D()` /
`Scale2D()` の射影ヘルパを使います。`Move(...)` で `TObjectPtr<ANode>` の強参照を
root に移します。ツリー外から長期間参照する場合は `TWeakObjectPtr<ANode>` を使い、
利用前に `Get()` または `IsValid()` で生存確認します。

### 3. カメラ追従（`Services().Camera()`）

```cpp
// OnReady で初期化
Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
Services().Camera().SetZoom(1.0f);

// OnTick で毎フレーム、プレイヤーへスムーズ追従（第2引数=追従の速さ）
if (ANode* player = m_Player.Get()) {
    Services().Camera().SetTargetPos(player->Position2D(), 8.0f);
}
```

この例の `m_Player` は `TWeakObjectPtr<ANode>` です。親ノードが子を
`TObjectPtr<ANode>` で所有するため、追跡側が強参照を重ねる必要はありません。

### 4. world / HUD を別レイヤーで描く

```cpp
void OnDrawWorld(FRenderContext& /*rc*/, FSpriteBatch& sb) noexcept override
{
    // カメラ適用後のワールド座標（単位=ワールド単位）。グリッド線など。
    sb.DrawRect(-8.0f, -0.01f, 16.0f, 0.02f, FVec4{0.25f,0.35f,0.45f,0.65f});
}

void OnDrawHud(FRenderContext& rc, FSpriteBatch& sb) noexcept override
{
    // 左上原点・ピクセル単位。半透明パネルなど。
    sb.DrawRect(12.0f, 12.0f, 360.0f, 54.0f, FVec4{0.0f,0.0f,0.0f,0.45f});
    (void)rc;
}
```

---

## 注意点 (gotcha)

- **座標系（ワールド）**: 中心が原点、`+Y = 画面下`（スクリーン系・DirectX/Tiled と同じ。HUD も同じ向き）。スクリーン投影は
  `screen_px = (world - ViewCenter) * (ppu * zoom) + (Width/2, Height/2)`。
  `OnReady` でプレイヤーに `gravity = FVec2{0, 14}` のように**下向きは正の Y**を使います（サンプル 55 もそう）。
- **HUD は別座標系**: `OnDrawHud` のスプライトバッチは**左上原点・ピクセル単位**。world と混同しない。
- **ピッキングは `FScene2D::ScreenToWorld` を使う**: `FCamera2D::ScreenToWorld` は ppu を考慮しないため、`FScene2D` のレンダリングと逆対応しません。マウス→ワールド変換はシーン側の `ScreenToWorld(FInput::MousePos())` を使うこと。
- **フックは全部 `noexcept`**: `override` の付け忘れ・`noexcept` の付け忘れはコンパイルエラー。
- **`Services()` は要求したものだけ**: `WantedServices()` が `None` のサービスを `Services().Xxx()` で触ると `ACS_CHECKF` で停止します。`FScene2D` 既定は `Default2D | Camera2D | Physics2D`。`Triggers` は含まれないので必要なら派生で `WantedServices()` を拡張してください。
- **初期化は `OnReady` で**: `OnReady` は `OnEnter` の中でサービス attach 後に 1 回呼ばれる、`FScene2D` 専用の初期化フック。コンストラクタでは `Services()` はまだ使えません。
- **固定タイムステップ**: `OnFixedTick(fixed_dt)` は 1 フレームで**0 回〜複数回**呼ばれ得ます（既定 `fixed_dt=1/60`、最大 8 step/フレームでキャッチアップ後はクランプ）。`dt` 依存の通常処理は `OnTick`、決定的な物理は `OnFixedTick` に分けるのが定石。
- **ウィンドウサイズ等の細かい設定**: `ACS_GAME_MAIN` は既定 `FAppConfig`（1280x720, vsync, リサイズ可）を使います。タイトルや解像度を変えたい場合は `ACS_GAME_MAIN` を使わず、`ACS_DEFINE_MAIN_WITH_CONFIG(YourGame, factory)`（`app/EntryPoint.h`）か自前 `main` で `FAppConfig` を作って `app.Run(cfg)` を呼びます。
- **遷移は次フレーム頭で適用**: `Scenes().ChangeScene(...)` / `GetGame().TransitionTo(...)` は走査中の構造変更を避けるため即時には切り替わりません（1 フレーム 1 遷移）。

---

## 動くサンプル

- `acs/samples/55_HelloScene2D/Scene2DStarter.cpp`
  本チュートリアルの全要素（`FScene2D` 派生 / `OnReady`-`OnTick`-`OnDrawWorld`-`OnDrawHud` / 入力バインド / `ASprite2DComponent` / `APhysicsBody2D` の collide-and-slide / カメラ追従 / `SetPixelsPerUnit` / `SetClearColor`）を 1 ファイルで実演。ビルドターゲットは `hello_scene2d`。スクリーンショット確認済みの実動サンプルです。
- ビルド: `acs/samples/55_HelloScene2D/CMakeLists.txt`（`ACS::GameFramework` にリンクするだけ）。

参照したヘッダ: `acs/src/gameframework/Game.h` / `Scene2D.h` / `Scene.h` / `EntryPoint.h` / `RenderContext.h` / `SceneServices.h`、および `acs/src/app/Application.h` / `AppConfig.h` / `EntryPoint.h`。
