# エフェクト(水/炎/トレイル) & 2Dライト & ステンシル & HUDテキスト

シェーダーもアセットも書かずに「波打つ水・揺れる炎・残像トレイル・点光源+ソフト影・窓型くりぬき・HUD文字」を出すための drop-in コンポーネント集です。`AddComponent` してパラメータを設定するだけで動きます。`acs::game` の `AWater2DComponent` / `AFire2DComponent` / `ATrail2DComponent` / `AStencilClip2DComponent`、`acs` の `CLighting2D` / `FFont` を扱います。「水辺・たき火のある2Dシーンを作りたい」「暗い洞窟に松明を灯したい」「窓越しに別シーンを覗かせたい」ときに使います。

> すべて `CSpriteBatch` のプリミティブ(三角形/矩形)を毎フレーム手続き生成して描くので HLSL は不要です。

---

## 最小例

`AScene` を継承し、`OnReady()` でノードにエフェクトコンポーネントを付けるだけです。

```cpp
#include "gameframework/GameFramework.h"
#include "gameframework/Effects2D.h"   // AWater2D/AFire2D/ATrail2D/AStencilClip2D の各 Component
using namespace acs;
using namespace acs::game;

class AMyScene final : public AScene {
    /** 2D の標準サービス構成 (Default2D | Camera2D | Physics2D) を要求する。 */
    ESvc WantedServices() const noexcept override { return kScene2DServices; }

public:
    void OnReady() noexcept override {
        SetPixelsPerUnit(48.0f);

        // 海 (横視点の矩形水面)
        auto n = NewObject<ANode>();
        n->SetPosition2D(FVec2{0.0f, 0.0f});
        auto& w = n->AddComponent<AWater2DComponent>();
        w.SetRect(FVec2{0.0f, 5.0f}, FVec2{11.0f, 2.0f});   // center, half_size (owner相対)
        w.SetWaves(0.16f, 1.6f);                             // 振幅, 速度
        w.SetDepthColors(FVec3{0.22f,0.55f,0.78f}, FVec3{0.03f,0.14f,0.30f}); // 浅→深
        w.SetFoam(FVec3{0.95f,0.98f,1.0f});                  // 陸際の白泡
        Root().AddChild(Move(n));

        // たき火
        auto fn = NewObject<ANode>();
        fn->SetPosition2D(FVec2{-4.0f, 1.5f});
        auto& f = fn->AddComponent<AFire2DComponent>();
        f.SetSize(0.85f, 2.1f);                              // 幅, 高さ
        Root().AddChild(Move(fn));
    }
};

class CMyGame final : public CGame {
protected:
    TUniquePtr<AScene> InitialScene() noexcept override { return MakeUnique<AMyScene>(); }
};
ACS_GAME_MAIN(CMyGame)
```

`SetReflectionEnabled` / `SetStencilMaskEnabled` を呼ばない限り、従来どおりの単一パス描画でコスト増はありません。

---

## 主要API

### AWater2DComponent — 波打つ水面

| API | 説明 |
| --- | --- |
| `SetStyle(EWaterStyle)` | `SideView`(横視点・海/プール、上端が波打ち平面反射) / `TopDown`(見下ろし・radial深度+コースティクス+全周泡) |
| `SetRect(center, half_size, top_segments=32)` | 矩形。`SetArea` は同義の旧API |
| `SetEllipse(center, rx, ry, segments=28)` | 楕円(水溜まり/池) |
| `SetRiver(path, count, width)` | polyline を太さ `width` の帯に膨らませる川 |
| `SetSplineRiver(control, count, width, samples_per_seg=8)` | Catmull-Rom 制御点で曲がる川 |
| `SetPolygon(pts, count)` / `SetSplineRegion(...)` | 任意多角形 / 滑らかな閉ループ湖 |
| `SetColor(rgb)` | 後方互換。deep を自動で `rgb*0.45` に設定 |
| `SetDepthColors(shallow, deep)` / `SetDepthAlpha(sa, da)` | 浅部→深部の頂点カラー勾配 |
| `SetWaves(amplitude, speed)` | 波の振幅と速度 |
| `SetFlow(dir, speed)` | 波・泡が `dir` 方向へ流れる(内部で正規化、既定+X) |
| `SetFoam(color, width=0, speed=3, alpha=0.9)` | 陸際の白泡。`width=0` で bbox から自動 |
| `SetRim(color, width=0, alpha=0.5)` | 水際の縁取り |
| `SetReflection(enable, tint, alpha=0.55)` | 平面反射(後述、`SetReflectionEnabled(true)` が前提) |
| `SetCaustics(tint, intensity=0.55, scale=1.4, speed=0.7)` | 光の網目(加算)。実用上 `TopDown` 向け |
| `EnableGlints(on)` | 波頭の散発的きらめき(加算、両スタイル) |
| `SetSkyTint(rgb, alpha)` | 見下ろしで薄く乗せる空色(`alpha=0`で無効) |
| `SetMeshResolution(a, b)` | メッシュ密度の手動指定(0=自動)。細かいほどコースティクスがくっきり |
| `Disturb(FVec2 world, strength)` | その点から放射状リップル(輪)を立てる |
| `Disturb(f32 world_x, strength)` | 後方互換。水面(SurfaceY)上の点として波紋 |
| `ContainsPoint(world)` / `ContainsX(x)` / `SurfaceY()` | 入水判定ヘルパ(bbox近似) |

```cpp
auto& w = node->AddComponent<AWater2DComponent>();
w.SetStyle(EWaterStyle::TopDown);
w.SetEllipse(FVec2{0,0}, 2.4f, 1.7f, 36);
w.SetCaustics(FVec3{0.55f,0.85f,1.0f}, 0.6f, 2.6f, 0.85f); // 見下ろしの光網目
w.Disturb(FVec2{0.5f, 0.0f}, 0.6f);                        // クリック位置で splash
```

### AFire2DComponent — 手続き炎

- `SetSize(width, height)` — 炎のサイズ。
- `SetIntensity(f32)` — 勢い(負値は0にクランプ)。近づくと強める等の演出に使う。
- `Intensity()` — 現在値の取得。

### ATrail2DComponent — owner追従の残像

- `SetColor(FVec3)` / `SetWidth(f32)` — 色と帯幅。
- `SetPoints(u32 k)` — 履歴点数(2〜48にクランプ)。owner ノードを自動で追従するので、ノードを動かすだけで残像が出ます。

### AStencilClip2DComponent — 任意形状で子ツリーをクリップ

| API | 説明 |
| --- | --- |
| `SetRect(center, half_size)` / `SetCircle(center, radius, segments=40)` | マスク形状(owner相対) |
| `SetEllipse(...)` / `SetPolygon(pts, count)` | 楕円 / 単純多角形マスク |
| `SetInvert(bool clip_outside)` | `true` で外側だけ残す(穴あき) |
| `SetRef(u8)` | 入れ子用のステンシル参照値(1..255、既定1) |
| `SetDebugVisualize(on, color)` | マスク形状自体を可視化(デバッグ用) |

### CLighting2D — 点光源 + ソフト影 (render層、生の `acs`)

`AScene` には組み込まれていません。`CApplication::OnCustomFrame` 等で自前のレンダーループを組む低レベルAPIです。

- `Init(device, color_format, w, h)` / `Resize(w, h)` — どちらも `TResult<void>`。成功確認後に使用する
- `Shutdown()` — GPU リソースを明示解放する
- `SetAmbient(FVec3)` — 環境光。暗くしておくと光源周りだけ照らされる Core Keeper 風に。
- `ClearLights()` / `AddLight(const FLight2D&)`(上限16、超過で `false`) / `LightCount()`
- `SetShadowQuality(march_steps, ray_count)` — レイ本数が多いほど penumbra が滑らか/重い。
- `BeginScene/EndScene` → `BeginOccluders/EndOccluders` → `Composite` の順で呼ぶ(後述)。

`FLight2D{ pos, radius=256, color, intensity=1, softness=0.5 }`。座標は**スプライトと同じピクセル空間(左上原点)**です。

### FFont / DrawString — HUDテキスト

- `FFont font; auto r = font.LoadFromFile(device, L"C:/Windows/Fonts/meiryo.ttc", 32.0f, 1024, false)` — TTF/TTC をアトラスに焼き、`TResult<void>` を返す。`IsOk()` を確認してから使う。漢字は第5引数を `true` にする（アトラスは必要に応じて2048等へ拡大）。
- `CSpriteBatch::DrawString(font, utf8_text, x, y, color=白)` — `(x,y)` は行の左上、`\n` で改行。
- `FRenderContext::HasFont()` / `GetFont()` — `AScene::OnDrawHud` ではこの2つで安全に描けます。

---

## よく使うパターン

### 1. マウスで水をなぞる/クリックして波紋を立てる

`AScene::ScreenToWorld`（ppu 対応の picking）で画面座標を world 座標へ変換し、`ContainsPoint` で当たった水域に `Disturb` します。

```cpp
void OnTick(f32 /*dt*/) noexcept override {
    const FVec2 mw = ScreenToWorld(FInput::MousePos());
    const FVec2 md = FInput::MouseDelta();
    const f32 sp = Sqrt(md.x*md.x + md.y*md.y);                 // px/frame
    const bool click = FInput::IsMouseButtonPressed(EMouseButton::Left);
    for (u32 i = 0; i < m_WaterCount; ++i) {
        AWater2DComponent* w = m_Waters[i];
        if (sp > 1.0f && w->ContainsPoint(mw)) w->Disturb(mw, 0.05f + sp*0.0006f);
        if (click && w->ContainsPoint(mw))     w->Disturb(mw, 0.55f);
    }
}
```

### 2. 平面反射を有効にする (横視点の海)

`AWater2DComponent::SetReflection(true, ...)` だけでは映りません。シーン側で `SetReflectionEnabled(true)` を呼んで「world→RT→swapchain」の3パス描画にする必要があります。

```cpp
void OnReady() noexcept override {
    auto& w = node->AddComponent<AWater2DComponent>();
    w.SetRect(FVec2{0,5}, FVec2{11,2});
    w.SetReflection(true, FVec3{0.85f,0.92f,1.0f}, 0.5f);  // 反射色と強さ
    w.SetReflectionDistortion(1.2f);
    // ...水面より上(=小さいY)にオブジェクトを置くと映る...
    SetReflectionEnabled(true);   // ★これが無いと反射RTが配線されず無視される
}
```

### 3. 窓越しに隠しシーンを覗かせる (ステンシル)

マスクノードの**子ツリー**が、マスク形状の内側だけに描かれます。`SetStencilMaskEnabled(true)` が前提です。

```cpp
void OnReady() noexcept override {
    SetStencilMaskEnabled(true);                 // world を stencil 付き DSV で描く

    auto win = NewObject<ANode>();
    win->SetPosition2D(FVec2{0,0});
    m_Clip = &win->AddComponent<AStencilClip2DComponent>();
    m_Clip->SetCircle(FVec2{0,0}, 2.4f, 48);     // 窓の形 (owner相対)

    // 子 = 窓の中だけに見える隠しシーン
    auto hidden = NewObject<ANode>();
    hidden->AddComponent<AWater2DComponent>().SetRect(FVec2{0,4.5f}, FVec2{12,2.4f});
    win->AddChild(Move(hidden));
    Root().AddChild(Move(win));
}
void OnTick(f32) noexcept override {
    if (m_Clip) m_Clip->SetCircle(ScreenToWorld(FInput::MousePos()), 2.4f, 48); // 窓を動かす
    // m_Clip->SetInvert(true); で「窓の中だけ隠す = 視界の穴」
}
```

### 4. 点光源+ソフト影の合成パス (低レベル)

`CLighting2D` は描画ブラケットの呼び順が厳格です。

```cpp
m_Lighting.SetAmbient(FVec3{0.05f,0.05f,0.08f});   // 暗い洞窟
m_Lighting.ClearLights();
FLight2D torch; torch.pos = mousePos; torch.radius = 420.0f;
torch.color = FVec3{1.0f,0.72f,0.42f}; torch.intensity = 1.5f; torch.softness = 0.6f;
m_Lighting.AddLight(torch);

// 1) world を scene RT へ
m_Lighting.BeginScene(*cl, FVec4{0.02f,0.02f,0.03f,1});
  sceneBatch.Begin(*cl, w, h); DrawWorld(); sceneBatch.End();
m_Lighting.EndScene(*cl);
// 2) 影を落とすスプライトを「白tint」で occluder RT へ
m_Lighting.BeginOccluders(*cl);
  occBatch.Begin(*cl, w, h); DrawOccluders(/*tint=*/FVec4{1,1,1,1}); occBatch.End();
m_Lighting.EndOccluders(*cl);
// 3) backbuffer に scene × light を合成
cl->BeginRenderToSwapchain(*sc, idx, FClearColor{0,0,0,1});
  m_Lighting.Composite(*cl, w, h);
  // ...この後に HUD を重ねる...
cl->EndRenderToSwapchain(*sc, idx);
```

### 5. HUD にテキストを描く

`AScene::OnDrawHud` で `rc.HasFont()` をガードしてから描きます。

```cpp
void OnDrawHud(FRenderContext& rc, CSpriteBatch& sb) noexcept override {
    sb.DrawRect(8.0f, 8.0f, 640.0f, 30.0f, FVec4{0,0,0,0.45f});  // 背景帯
    if (rc.HasFont()) {
        sb.DrawString(rc.GetFont(), "Effects2D  [Esc]", 16.0f, 15.0f,
                      FVec4{0.9f,0.95f,1.0f,1.0f});
    }
}
```

---

## 注意点 (gotcha)

- **反射は2段階**: `SetReflection(true)` は水コンポーネント側のフラグに過ぎません。シーンで `SetReflectionEnabled(true)` を呼ばないと反射RTが配線されず無視されます。逆に反射を使わないシーンでONにすると world を2度描く無駄コストがかかります。
- **コースティクスは実用上 TopDown**: `SetCaustics`/`SetSkyTint` は見下ろし(`EWaterStyle::TopDown`)の見せ方です。横視点は反射+泡+縁で表現します。`SetCaustics` を呼ぶと内部で `EnableCaustics(true)` も立ちます。
- **ステンシルも2段階**: `SetStencilMaskEnabled(true)` を呼び忘れると DSV が無く、`AStencilClip2DComponent` は素通し(クリップしない)になります。また反射のRTパスなど stencil バッファの無いパスでは自動的に素通しです。入れ子マスクは `SetRef` を別値にしてください。
- **クリップ対象は「子ツリー」**: マスクは attach したノード自身ではなく、その子に追加したノード群に効きます。`win->AddChild(...)` で中身を足す構造にします。
- **座標系の混在に注意**: `AWater2DComponent` / `AStencilClip2DComponent` の `SetRect` 等はすべて **owner ノード相対**で、world +Y が画面下です。一方 `CLighting2D` と `FFont` / `DrawString` は **スクリーンのピクセル空間（左上原点）** です。混同しないこと。
- **ピッキングは `ScreenToWorld` を使う**: マウス→ワールド変換は `AScene::ScreenToWorld`(ppu と camera zoom を考慮)を使います。`CCamera2D::ScreenToWorld`(ppu非考慮)では水域とズレます。
- **`CLighting2D` の呼び順**: `BeginScene/EndScene` → `BeginOccluders/EndOccluders` → ターゲットbind → `Composite` の順序が必須。occluder は**白tint**で黒地に焼くとシルエットが遮蔽物になります。光源は最大16個(`AddLight` が `false` を返したら上限)。
- **メッシュ上限**: 水は頂点320/三角形560が上限。強い凹形状の `SetPolygon` は centroid 扇塗りで破綻しやすいので、複数の水域に分けるのが安全です。
- **トレイルは owner を追う**: `ATrail2DComponent` はノードを動かして初めて軌跡が出ます。静止ノードでは何も描かれません。
- **コンポーネントの生ポインタは owner 寿命内だけ**: 例の `m_Clip` / `m_Waters` は各ノードが所有するコンポーネントへの非所有参照です。ノードを `Destroy()` した後は参照せず、必要なら同時に `nullptr` へ戻してください。
- **初期化結果を確認する**: `CLighting2D::Init` / `Resize` と `FFont::LoadFromFile` は `TResult<void>` を返します。`IsOk()` / `IsErr()` で分岐し、失敗した機能を使わないでください。
- **`FFont` は事前ロード**: `FFont::LoadFromFile` 失敗時(フォントが無い環境)は `rc.HasFont()` が `false` になり、テキストは単に描かれません(クラッシュはしない)。`FFont` はコピー不可で、device 破棄前に `Shutdown()` します。

---

## API の責務

- `AWater2DComponent` は水域形状、法線、反射、岸辺表現を描画します。
- `AFire2DComponent` と `ATrail2DComponent` は node に追従する時間変化を保持します。
- `AStencilClip2DComponent` は子 tree の内外を任意形状で切り替えます。
- `CLighting2D` は点光源、遮蔽物、soft shadow を合成します。
- `FFont` と `CSpriteBatch` は HUD text の atlas と描画を担当します。

ヘッダ実体: `acs/src/gameframework/Effects2D.h` / `acs/src/render/Light2D.h` / `acs/src/render/Font.h`。
