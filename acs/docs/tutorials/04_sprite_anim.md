# スプライトシートアニメ (ASpriteAnimComponent / FSpriteAnimator)

1 枚のテクスチャ (スプライトシート) を時間で切り替えてアニメーションさせる仕組み。`ANode` に貼った `ASprite2DComponent` の UV サブ矩形を毎フレーム書き換えて実現する。歩行・攻撃モーション、エフェクトのコマ送り、回転アイコンなどに使う。

役割分担:

- **`FSpriteAnimator`** … 「時間 → 現在 frame index」だけを計算する純ロジック (asset 非依存・テスト可能)。`Loop / PingPong / Once`、frame event を担当。
- **`ASpriteAnimComponent`** … 上記 animator を `ASprite2DComponent` に橋渡しする `AComponent`。グリッド/任意 UV の事前計算と、毎 `OnUpdate` での UV 適用を行う。

通常は `ASpriteAnimComponent` を使えばよい。`FSpriteAnimator` は自前の Quad に UV を当てたいときの低レベル API。

---

## 最小例

`ANode` にスプライトとアニメコンポーネントを付け、`InitGrid` でグリッドを割って `Play()` するだけ。これが verified サンプル 56 の中核そのまま。

```cpp
#include "gameframework/GameFramework.h"
using namespace acs;
using namespace acs::game;

// シーンの OnReady() 内など
auto node = NewObject<ANode>();
node->SetPosition2D(FVec2{0.0f, 0.0f});

// 1) 描画先スプライト (ワールド単位サイズ 2x2) を付けてテクスチャを差す
auto& spr = node->AddComponent<ASprite2DComponent>(FVec2{2.0f, 2.0f});
spr.SetTexture(sheetTex);               // IRhiTexture*  (256x64 を 4 セルに割る想定)

// 2) アニメコンポーネントを付けてグリッド初期化 → 再生
auto& anim = node->AddComponent<ASpriteAnimComponent>();
anim.InitGrid(/*cols=*/4, /*rows=*/1, /*frame_count=*/4, /*fps=*/8.0f,
              EPlayMode::Loop);
anim.Play();

Root().AddChild(Move(node));
```

`InitGrid` はテクスチャ全体 `{0,0}〜{1,1}` を `cols×rows` に等分し、先頭から `frame_count` セルを左→右、上→下の順に並べる。あとはシーンの更新ループ (`AScene` が `OnUpdate` を回す) で自動的に UV が切り替わる。**手動で `Tick` を呼ぶ必要はない**。

---

## 主要 API

### EPlayMode (`SpriteAnimator.h`)

| 値 | 挙動 |
| --- | --- |
| `EPlayMode::Loop` | `0→N-1→0→…` と循環 (既定) |
| `EPlayMode::PingPong` | `0→N-1→0→…` と両端で折り返す。周期は `2*(N-1)` frame |
| `EPlayMode::Once` | `0→N-1` で停止。末尾で `IsFinished()==true`・`IsPlaying()==false` |

### ASpriteAnimComponent

| メソッド | 説明 |
| --- | --- |
| `InitGrid(cols, rows, frame_count, fps, mode=Loop)` | グリッドシートを等分して frame UV を生成。`frame_count==0` or `>cols*rows` なら `cols*rows` を使う |
| `BeginFrames(fps, mode=Loop)` | 任意 UV 列の構築開始 (それまでの frame をクリア) |
| `AddFrameUv(FVec4 uv)` | `{u0,v0,u1,v1}` を 1 frame 追加。`Begin〜End` の間のみ有効 |
| `EndFrames()` | 構築確定。`AddFrameUv` の数で `animator.Init` する |
| `Play()` / `Pause()` / `Stop()` | 再生 / 一時停止 (位置維持) / 停止 (先頭に戻す) |
| `SetFps(f32)` | 再生速度変更 (animator へ委譲) |
| `IsPlaying()` / `IsFinished()` | 再生中か / `Once` で終了したか |
| `CurrentFrame()` | 現在の frame index (`u32`) |
| `Animator()` | 下位 `FSpriteAnimator&` を取得 (frame event 登録などに) |

### FSpriteAnimator (低レベル)

| メソッド | 説明 |
| --- | --- |
| `Init(frame_count, fps, mode=Loop)` | 初期化。`frame_count==0` → 1、`fps<=0` → 1 にフォールバック |
| `Tick(f32 dt)` | 経過時間を流す。`dt<=0` は no-op (巻き戻し不可) |
| `Play/Pause/Stop` | 再生制御。`Once` 終了後の `Play()` は先頭から再生し直す |
| `CurrentFrame()` | `floor(elapsed*fps)` をモードで補正した index |
| `NormalizedTime()` | 周期内の進行率 `[0,1]` |
| `SetCurrentFrame(u32 i)` | 強制シーク (範囲外は末尾にクランプ) |
| `SetFps(f32)` | fps 差し替え (負値は 0 = フリーズにクランプ) |
| `AddFrameEvent(frame, cb, user)` | 指定 frame 進入時に 1 度だけ `void(*)(void*) noexcept` を呼ぶ |

---

## よく使うパターン

### 1. グリッドシートをループ再生 (サンプル 56)

横並び 4 セル (256x64) を 8fps でループ。これが最も基本。

```cpp
auto& anim = node->AddComponent<ASpriteAnimComponent>();
anim.InitGrid(4, 1, 4, 8.0f, EPlayMode::Loop);
anim.Play();
```

### 2. 複数行シート + 一部だけ使う

`8x4` の 32 セルのうち先頭 6 枚だけを歩行モーションとして再生する例。`frame_count` で枚数を絞れる (左上から行優先で並ぶ)。

```cpp
anim.InitGrid(/*cols=*/8, /*rows=*/4, /*frame_count=*/6, /*fps=*/12.0f);
anim.Play();
```

### 3. 任意 UV 列 (シートが等分でない / 名前付き frame)

セルが不均等な packed atlas などはグリッドで割れないので、`BeginFrames → AddFrameUv* → EndFrames` で 1 枚ずつ UV を積む。`AddFrameUv` は `FVec4{u0,v0,u1,v1}` (0〜1 正規化 UV)。

```cpp
auto& anim = node->AddComponent<ASpriteAnimComponent>();
anim.BeginFrames(/*fps=*/12.0f, EPlayMode::Loop);
anim.AddFrameUv(FVec4{0.00f, 0.0f, 0.25f, 1.0f});  // frame 0
anim.AddFrameUv(FVec4{0.25f, 0.0f, 0.55f, 1.0f});  // frame 1 (幅違いも可)
anim.AddFrameUv(FVec4{0.55f, 0.0f, 1.00f, 1.0f});  // frame 2
anim.EndFrames();
anim.Play();
```

> アトラスのフレーム名→UV を **`FSpritePack` から取る経路も検証済み**: `pack.LoadAtlasJson(text, len)`（Aseprite hash / TexturePacker array）または `pack.AddFrame(...)` でフレームを登録し、`pack.FindFrame("name")` → `pack.ComputeUv(*frame)` で `FVec4{u0,v0,u1,v1}` を得て `AddFrameUv` / `ASprite2DComponent::SetUvRect` へ渡せる。検証 = `62`（`TestSpriteAtlasLoad`）/ `63`（プレイヤースプライトを atlas フレームで描画）。手書きで `FVec4` を直接積んでもよい。

### 4. frame event (足音・攻撃判定タイミング)

特定 frame に入った瞬間にコールバックを 1 度だけ呼ぶ。`std::function` は使えず **capture なしの関数ポインタ + `void* user`** のみ。`Animator()` 経由で登録する。

```cpp
struct FPlayer { void OnFootstep() noexcept { /* ... */ } };

// InitGrid / EndFrames の後 (frame_count 確定後) に登録すること
anim.InitGrid(8, 1, 8, 12.0f, EPlayMode::Loop);
anim.Animator().AddFrameEvent(4, [](void* ud) noexcept {
    static_cast<FPlayer*>(ud)->OnFootstep();
}, playerPtr);
anim.Play();
```

`Loop` では周回ごとに再発火する。`frame >= frame_count` の登録は黙って無視される。

---

## 注意点 (gotcha)

- **`ASprite2DComponent` が描画先**: `ASpriteAnimComponent` 自身は描かない。同じ `ANode` に貼った sprite の `SetUvRect` を書き換えるだけ。`OnRequire` で sprite が無ければ `GetOrAddComponent<ASprite2DComponent>()` で**自動追加される**が、それだと**テクスチャ未設定の素の sprite**になる。アニメさせたいなら自分で `AddComponent<ASprite2DComponent>` してから `SetTexture` を呼ぶこと (サンプル 56 もそうしている)。

- **sibling は初回 `OnUpdate` で遅延 lookup**: コンポーネントの追加順 (sprite が先か anim が先か) に依存しないよう、sprite ポインタは最初の `OnUpdate` で解決される。つまり **`OnReady`/構築直後の時点ではまだ UV は適用されていない**。1 フレーム目以降で反映される点に注意 (静止 1 フレーム目を即見せたい用途では誤解しやすい)。

- **fps と dt の関係**: frame index は `floor(elapsed * fps)`。fps が高すぎて 1 frame の表示時間 (`1/fps` 秒) が `dt` より短いと、コマが飛ぶ (低 FPS 環境で顕著)。逆に `fps=0` は完全フリーズ。`InitGrid`/`Init` は `fps<=0` を `1.0f` にフォールバックするが、`SetFps` 後は `0` も許す (= フリーズ)。

- **`Stop` と `Pause` の違い**: `Stop()` は先頭 frame に戻して停止、`Pause()` は現在位置を維持して停止。`Play()` は現在位置から再開する (`Once` が終わっている場合のみ先頭に巻き戻して再生)。

- **`PingPong` の枚数**: 周期は `2*(N-1)` frame で、両端 (frame 0 と N-1) は 1 周期に 1 度ずつしか出ない。3 枚以上ないと折り返しが意味を持たない (`N<=1` は常に frame 0)。

- **frame event は `Init` の後に登録**: `AddFrameEvent` は `frame >= m_FrameCount` を無視する。`InitGrid`/`EndFrames` で `frame_count` が確定する**前**に登録すると (`frame_count` の既定は 1)、index 1 以上のイベントが全部捨てられる。必ず初期化後に登録すること。

- **UV は 0〜1 正規化座標**: `AddFrameUv` / `SetUvRect` はピクセルではなくテクスチャ正規化 UV。`{u0,v0}` が左上、`{u1,v1}` が右下。V は下方向に増える (テクスチャ座標系)。

---

## 動くサンプル

- `acs/samples/56_HelloSpriteAnim/SpriteAnimDemo.cpp` — 手続き生成した 4 セル (256x64) シートを `InitGrid(4,1,4,8.0f,Loop)` で 8fps ループ再生。ノードを左右に揺らしつつ HUD に `CurrentFrame()` を表示する (verified / スクショ確認済)。

実装本体:

- `acs/src/gameframework/SpriteAnimComponent.h` / `.cpp` — グリッド/任意 UV の事前計算と sprite への適用
- `acs/src/gameframework/SpriteAnimator.h` / `.cpp` — 時間 → frame index、再生モード、frame event
- `acs/src/gameframework/Sprite2DComponent.h` — `SetUvRect` / `SetTexture` など描画先
