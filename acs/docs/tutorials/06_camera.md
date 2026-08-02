# カメラ (CCamera2D)

2D シーンの「見ている位置・拡大率・回転」を持ち、プレイヤー追従・画面振動 (shake)・ワールド境界クランプ・座標変換をまとめて面倒見るのがこのカメラです。`AScene2D` を継承していれば既に有効化されており、`Services().Camera()` でいつでも触れます。「プレイヤーを真ん中に映したい」「攻撃ヒットで画面を揺らしたい」「クリックした場所のワールド座標が欲しい」ときに使います。

> カメラは `CSceneServices` のサービスとして `ESvc::Camera2D` ビットで有効化されます。`AScene2D` の `WantedServices()` は `Default2D | Camera2D | Physics2D` を返すので、`AScene2D` を継承する限り自分で何も足さなくて OK です。

---

## 最小例

`AScene2D` 派生シーンで、毎フレームプレイヤー位置にカメラを追従させる最小コードです (sample 55 と同じ構造)。

```cpp
#include "gameframework/GameFramework.h"

using namespace acs;
using namespace acs::game;

class AMyScene final : public AScene2D
{
public:
    void OnReady() noexcept override
    {
        SetPixelsPerUnit(64.0f);                  // 1 ワールド単位 = 64px
        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
        Services().Camera().SetZoom(1.0f);

        auto player = NewObject<ANode>();
        player->AddComponent<ASprite2DComponent>(FVec2{0.9f, 0.9f},
                                                 FVec4{0.2f, 0.8f, 1.0f, 1.0f});
        m_Player = player;                       // 生存を延長しない弱参照
        Root().AddChild(Move(player));            // 強参照はツリーへ
    }

    void OnTick(f32 /*dt*/) noexcept override
    {
        // 毎フレーム「追いたい位置」を渡すだけ。指数 smoothing で滑らかに寄る。
        if (ANode* player = m_Player.Get()) {
            Services().Camera().SetTargetPos(player->Position2D(), 8.0f);
        } else {
            Services().Camera().ClearTarget();
        }
    }

private:
    TWeakObjectPtr<ANode> m_Player;
};
```

`SetTargetPos` は **毎フレーム値で渡す方式** です。ポインタを保持しないので、対象が消えても dangling しません。追従の実体は `CSceneServices` が `OnUpdate` の後 (PostUpdate) で `Camera().Tick(dt)` を自動で呼んで進めます。自分で `Tick` を呼ぶ必要はありません。

---

## 主要 API

すべて `Services().Camera()` (= `acs::game::CCamera2D&`) のメソッドです。

| メソッド | 説明 | 例 |
|---|---|---|
| `SetPosition(FVec2 p)` / `Position()` | カメラ中心のワールド座標を直接設定/取得 | `cam.SetPosition({3.0f, 1.0f});` |
| `SetZoom(f32 z)` / `Zoom()` | 拡大率。`z>1` で拡大、内部で `0.001` 下限にクランプ | `cam.SetZoom(1.5f);` |
| `SetRotation(f32 r)` / `Rotation()` | 回転 (ラジアン、+ で CCW) | `cam.SetRotation(0.1f);` |
| `SetTargetPos(FVec2 t, f32 smoothing=5.0f)` | 追従目標を設定。`smoothing` が大きいほど機敏 (典型 3〜10)、`<=0` で即スナップ | `cam.SetTargetPos(p, 8.0f);` |
| `ClearTarget()` / `HasTarget()` | 追従を解除/状態取得 | `cam.ClearTarget();` |
| `AddShake(f32 amount)` | shake の trauma を加算 (clamp [0,1])。**累積方式** | `cam.AddShake(0.5f);` |
| `TraumaLevel()` | 現在の trauma 値 | `if (cam.TraumaLevel() > 0) ...` |
| `SetShakeAmplitude(f32 a)` | shake 最大振幅 (ワールド単位、trauma=1 時)。既定 0.5 | `cam.SetShakeAmplitude(0.3f);` |
| `SetShakeDecayRate(f32 r)` | trauma 減衰率/秒。既定 1.0 (= 1秒で 1→0) | `cam.SetShakeDecayRate(1.5f);` |
| `SetBounds(FVec2 min, FVec2 max)` | カメラ中心の可動範囲を矩形でクランプ | `cam.SetBounds({-10,-5},{10,5});` |
| `ClearBounds()` / `HasBounds()` | 境界クランプ解除/状態取得 | `cam.ClearBounds();` |
| `SetDeadzone(FVec2 half_extents)` | 追従対象が箱の内側にいる間はカメラを動かさない | `cam.SetDeadzone({2.0f, 1.0f});` |
| `ClearDeadzone()` / `HasDeadzone()` | デッドゾーン解除/状態取得 | `cam.ClearDeadzone();` |
| `EffectiveViewCenter()` | `Position + shakeオフセット` = レンダラが実際に使う view 中心 | `auto c = cam.EffectiveViewCenter();` |

座標変換は **2 系統あり、使うべきものが違います** (後述の gotcha 参照):

| メソッド | どこ | ppu 考慮 | 使う? |
|---|---|---|---|
| `AScene::ScreenToWorld(FVec2 screen_px)` | シーン側 | **する** (ppu × zoom) | **これを使う** |
| `CCamera2D::ScreenToWorld(screen, w, h)` | カメラ単体 | しない (zoom のみ) | ppu=1 の特殊用途のみ |

```cpp
// シーン内 (this が AScene 派生) ならこれ一発でマウス→ワールド
FVec2 world = ScreenToWorld(FInput::MousePos());
```

---

## よく使うパターン

### 1. プレイヤー追従 (sample 55 そのまま)

```cpp
void OnTick(f32 /*dt*/) noexcept override
{
    // 第2引数 smoothing=8.0 でやや機敏に。dt 不変な指数追従なので
    // フレームレートが変わっても寄り具合は同じ。
    if (ANode* player = m_Player.Get()) {
        Services().Camera().SetTargetPos(player->Position2D(), 8.0f);
    }
}
```

### 2. ヒットで画面振動 (trauma を積む)

```cpp
void OnDamage() noexcept
{
    // 弱いヒットは小さく、強いヒットは大きく振らせたいので amount を変える。
    Services().Camera().AddShake(0.35f);   // 累積するので連打で強くなる
}
```
shake の実振幅は `trauma² × amplitude` で計算されるため、`AddShake` の値を二乗的に強く感じます。連続ヒットで `AddShake` を重ねると trauma が積み上がり (上限 1.0)、毎フレーム `decayRate × dt` ずつ自動で抜けていきます。

### 3. レベル端でカメラを止める (境界クランプ)

```cpp
void OnReady() noexcept override
{
    // ステージが x:[-10,10], y:[-5,5] のとき、外が見えないよう中心を制限。
    Services().Camera().SetBounds(FVec2{-10.0f, -5.0f}, FVec2{10.0f, 5.0f});
}
```
クランプは `Tick` 内で追従の後に適用されるので、追従とクランプは併用できます。

### 4. マウスピッキング (クリック地点のワールド座標)

```cpp
void OnTick(f32 /*dt*/) noexcept override
{
    if (Services().Input().IsPressed(kClick)) {        // BindMouseButton で割当
        FVec2 sp = FInput::MousePos();                  // 画面px (左上原点)
        FVec2 wp = ScreenToWorld(sp);                  // ← AScene 側 (ppu対応)
        SpawnAt(wp);
    }
}
```
`kClick` は `OnReady` で `Services().Input().BindMouseButton(kClick, EMouseButton::Left);` のように割り当てておきます。

---

## 注意点 (gotcha)

- **座標変換は必ず `AScene::ScreenToWorld` を使う。** `CCamera2D::ScreenToWorld(screen, w, h)` は ppu (pixels-per-unit) を考慮せず zoom しか割らないため、`SetPixelsPerUnit(64)` のような通常のシーンでは結果が 64 倍ずれます。`AScene` のレンダリングは `ppu × zoom` でスケールしているので、それと厳密に逆対応する `AScene::ScreenToWorld` が正解です。`CCamera2D::ScreenToWorld` は ppu=1 を前提にした単体テスト的用途以外では使わないでください。
- **画面座標は左上原点のクライアント px。** `FInput::MousePos()` が返すのはウィンドウローカルのピクセル (左上 = (0,0))。これをそのまま `ScreenToWorld` に渡す前提です。自前で y を反転させたりしないこと。
- **`ScreenToWorld` が使う画面サイズは「直近 `OnRender` でキャッシュした値」。** `AScene` は `OnRender` 時に `m_ScreenW/m_ScreenH` を更新します。1 フレームも描画していない初期状態では既定 (1280×720) が使われます。実用上は問題になりませんが、リサイズ直後の 1 フレームだけ古い値になり得ます。
- **shake は累積 (trauma) 方式。** `AddShake` は「揺れ量を直接セット」ではなく trauma に**加算**します。毎フレーム呼ぶと際限なく積む (上限 1.0 でクランプ)。通常はイベント発生時に 1 回だけ呼びます。揺れは `trauma²` でスケールするので、`0.5` を渡しても見た目の揺れは控えめです。派手にしたいなら `SetShakeAmplitude` を上げるか trauma を大きめに。
- **`SetTargetPos` を呼ぶ限り `SetPosition` は上書きされる。** 追従を有効にしたまま `SetPosition` しても、次の `Tick` で目標へ寄せ直されます。手動配置に戻したいときは `ClearTarget()` してから `SetPosition` を。
- **`Tick` は自分で呼ばない。** `CSceneServices::_PostUpdate` が `OnUpdate` の後にカメラを tick します。手動 `Tick` を足すと二重更新になります。
- **回転 (`SetRotation`) はラジアン。** 度ではありません。`+` で CCW (反時計回り)。`ScreenToWorld` は回転も逆解決しますが、回転を多用する 2D ゲームは稀です。

---

## 動くサンプル

- **`acs/samples/55_HelloScene2D/Scene2DStarter.cpp`** — `AScene2D` 派生 + `SetTargetPos(player, 8.0f)` でのプレイヤー追従、`SetPosition` / `SetZoom` の初期化、`SetPixelsPerUnit(64)` を実際に動かして screenshot 検証済みのスターター。カメラ追従の挙動はまずここを動かすのが早いです。

実装本体は `acs/src/gameframework/Camera2D.h` (全ロジックがヘッダに inline)、シーン側の座標変換は `acs/src/gameframework/Scene.cpp` の `AScene::ScreenToWorld` を参照してください。
