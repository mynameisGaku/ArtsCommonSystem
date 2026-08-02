# スプライト描画 & プリミティブ & テクスチャ取得

2D の絵を画面に出す手段は ACS に 2 系統あります。

- **`ASprite2DComponent`** — `ANode` に貼る描画コンポーネント。ノードの transform（位置/回転/スケール）に従って矩形 or テクスチャを **ワールド単位** で描く。ゲームのキャラ・地形など「シーンの一部」はこれ。
- **`CSpriteBatch`** — ピクセル座標で直接矩形/三角形/テクスチャ/文字を描く低レベルバッチ。HUD・デバッグ描画・素の RHI アプリ用。`AScene` 内では `rc.Sprites()` で `CGame` 共有インスタンスを取れる。

このページは「ノードにスプライトを付ける」「単色矩形を出す」「PNG をテクスチャに読む」を実コードで通します。

---

## 最小例

`AScene` 上でノードに 1 枚スプライトを付ける（sample 55 を抜粋）。テクスチャ無しコンストラクタは **単色矩形** になります。

```cpp
#include "gameframework/GameFramework.h"
using namespace acs;
using namespace acs::game;

class AMyScene final : public AScene {
    /** 2D の標準サービス構成 (Default2D | Camera2D | Physics2D) を要求する。 */
    ESvc WantedServices() const noexcept override { return kScene2DServices; }

public:
    void OnReady() noexcept override {
        SetPixelsPerUnit(64.0f);                 // 1 ワールド単位 = 64px

        auto node = NewObject<ANode>();
        node->SetPosition2D(FVec2{0.0f, 0.0f});
        // 0.9x0.9 ワールド単位、水色の単色矩形（テクスチャ無し）
        node->AddComponent<ASprite2DComponent>(
            FVec2{0.9f, 0.9f}, FVec4{0.15f, 0.85f, 1.0f, 1.0f});
        Root().AddChild(Move(node));

        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
        GetGame().SetClearColor(0.04f, 0.045f, 0.055f);
    }
};

class CMyGame final : public CGame {
protected:
    TUniquePtr<AScene> InitialScene() noexcept override {
        return MakeUnique<AMyScene>();
    }
};
ACS_GAME_MAIN(CMyGame)
```

`CGame` が game 寿命で共有する `CSpriteBatch` を `AScene` がフレーム毎に `FRenderContext` へ差し込み、各 `ASprite2DComponent::OnDraw` がそれを使って描画します。自分で `CSpriteBatch` を初期化する必要はありません。

---

## 主要 API

### `ASprite2DComponent`（`gameframework/Sprite2DComponent.h`）

ノードに 1 個付けて使う描画コンポーネント。サイズは **ワールド単位**、位置/回転/スケールは所有ノードの transform 由来。

| メンバ | 説明 |
| --- | --- |
| `ASprite2DComponent(FVec2 size, FVec4 tint={1,1,1,1})` | サイズ（ワールド単位）と色で構築。テクスチャ未設定なら単色矩形。 |
| `SetTexture(IRhiTexture*)` / `Texture()` | 貼るテクスチャ（**非所有** — 寿命は呼び出し側が管理）。`nullptr` で単色に戻る。 |
| `SetSize(FVec2)` / `Size()` | 描画サイズ（ワールド単位）。`scale` と乗算される。 |
| `SetTint(FVec4)` / `Tint()` | 乗算色（RGBA）。テクスチャありなら色掛け、無しなら塗り色。 |
| `SetPivot(FVec2)` / `Pivot()` | 原点。`0..1` の正規化値。既定 `{0.5, 0.5}` = 中心。`{0,0}` で左上基準。 |
| `SetUvRect(u0,v0,u1,v1)` / `SetUvRect(FVec4)` | UV サブ矩形。スプライトシートの 1 フレーム切り出し。既定 `{0,0,1,1}` = テクスチャ全体。 |
| `UvMin()` / `UvMax()` | 現在の UV 左上 / 右下。 |

> 内部実装（`Sprite2DComponent.cpp`）はテクスチャありなら `DrawRotated`、無しなら `DrawRectRotated` を `rc.Sprites()` に対して呼ぶだけ。`rc.HasSprites()==false` のパスでは何も描かない。

```cpp
auto& spr = node->AddComponent<ASprite2DComponent>(FVec2{2.0f, 2.0f});
spr.SetTexture(myTex);                 // テクスチャを貼る
spr.SetTint(FVec4{1, 1, 1, 0.5f});     // 半透明
spr.SetUvRect(0.0f, 0.0f, 0.25f, 1.0f); // 横 4 分割の 1 枚目
```

### `CSpriteBatch`（`render/SpriteBatch.h`）

ピクセル座標（左上原点・Y 下向き）で描く低レベルバッチ。**`AScene` のシーン内では `rc.Sprites()` 経由で使い、自前 Init は不要**。素の `CApplication` で使うときだけ自分で `Init`/`Begin`/`End` する。

| メソッド | 説明 |
| --- | --- |
| `Draw(tex, x, y, w, h, tint={1,1,1,1})` | テクスチャ全体を矩形に描く。 |
| `DrawSub(tex, x, y, w, h, u0,v0,u1,v1, tint)` | テクスチャの一部（UV 0..1）を描く。 |
| `DrawRect(x, y, w, h, color)` | 単色矩形（テクスチャ無し、内部 1x1 白テクスチャ使用）。 |
| `DrawRotated(tex, cx,cy, w,h, radians, u0,v0,u1,v1, tint)` | `(cx,cy)` 中心に回転してテクスチャ（の一部）を描く。 |
| `DrawRectRotated(cx,cy, w,h, radians, color)` | `(cx,cy)` 中心に回転した単色矩形。 |
| `DrawTriangle(x0,y0, x1,y1, x2,y2, color)` | 単色塗りつぶし三角形。 |
| `DrawTriangleVC(x0,y0,x1,y1,x2,y2, c0,c1,c2)` | 頂点カラー三角形（グラデーション）。各頂点に別色。 |
| `DrawTriangleSub(tex, x0,y0,x1,y1,x2,y2, u0,v0,u1,v1,u2,v2, tint)` | per-vertex UV でテクスチャを貼る三角形（反射等）。 |
| `DrawString(font, utf8, x, y, color={1,1,1,1})` | UTF-8 テキスト。`(x,y)` は行の左上。`\n` 改行、未収録グリフはスキップ。 |
| `SetView(cam_x, cam_y, zoom)` | 2D カメラ。`(cam_x,cam_y)` を画面中心に、`zoom` 倍で拡縮。**`Begin()` で恒等にリセット**。 |
| `SetClipRect(x,y,w,h)` / `ClearClipRect()` | 以降の描画を矩形内（画面座標）に制限。 |
| `SetBlendMode(EBlendMode)` | ブレンド切替。`Additive` で加算（光のきらめき等）。戻すときは `AlphaBlend`。 |
| `SetStencilMode(EStencilMode, ref=1)` | ステンシルマスク。**stencil 付き DSV のパス専用**（`AScene::SetStencilMaskEnabled(true)`）。 |

素の `CApplication` で使う場合の初期化：

```cpp
CSpriteBatch sb;
auto initResult = sb.Init(*device, renderer.ColorFormat(), /*max_sprites=*/4096);
if (initResult.IsErr()) {
    // initResult.Error() をログへ出し、この描画経路を開始しない
    return;
}
// 毎フレーム:
sb.Begin(*cmdList, screen_w, screen_h);
sb.Draw(tex, 100, 200, 64, 64);
sb.End();
// 終了時:
sb.Shutdown();
```

### テクスチャ生成・取得

| API | 用途 |
| --- | --- |
| `CreateRhiTexture(device, FTextureDesc)`（`render/IRhiTexture.h`） | CPU ピクセル配列から直接 GPU テクスチャを作る（手続き生成・自前デコード）。 |
| `registry.Load(L"hero.png")`（`asset/AssetRegistry.h`） | ファイルを `FImageAsset` として読む（stb_image: png/jpg/bmp/tga/gif/hdr 等）。 |
| `UploadTexture(device, FImageAsset)`（`render/RenderAssets.h`） | `FImageAsset` を GPU テクスチャにアップロード。**PNG→texture の決め手**。 |

---

## よく使うパターン

### 1) PNG を読んでスプライトに貼る（実経路）

gameframework に「ファイル名 1 行でスプライト」する薄いラッパは **ありません**。素の経路は `FAssetRegistry::Load`（→`FImageAsset`）→ `UploadTexture` → `ASprite2DComponent::SetTexture` の 3 段です。次は `easy/Easy.cpp` の実コードと同じ手順を移植したものです。

```cpp
#include "asset/AssetRegistry.h"
#include "asset/ImageAsset.h"
#include "render/RenderAssets.h"

// シーンのメンバとして寿命を持たせる（テクスチャは非所有で貼るため）
FAssetRegistry           m_Assets;
TUniquePtr<IRhiTexture>  m_Tex;

void OnReady() noexcept override {
    m_Assets.RegisterDefaultLoaders();   // Image/Audio/Mesh/Text/Binary ローダ登録

    auto r = m_Assets.Load(L"assets/hero.png");
    if (r.IsOk()) {
        TSharedPtr<FAsset> asset = r.Value();
        FAsset* base = asset.Get();
        // FAsset を画像かチェックしてからダウンキャスト（Easy.cpp と同じ流儀）
        if (base && base->Type() == FImageAsset::StaticType()) {
            auto* img = static_cast<FImageAsset*>(base);
            IRhiDevice* dev = GetGame().GetRenderer().Device();
            if (dev != nullptr) {
                auto tx = UploadTexture(*dev, *img);
                if (tx.IsOk()) m_Tex = Move(tx.Value());
            }
        }
    }

    auto node = NewObject<ANode>();
    auto& spr = node->AddComponent<ASprite2DComponent>(FVec2{1.0f, 1.0f});
    if (m_Tex) spr.SetTexture(m_Tex.Get());   // 非所有: m_Tex がテクスチャを保持し続ける
    Root().AddChild(Move(node));
}
```

> ポイント: `SetTexture` は **非所有ポインタ**を取るだけ。`m_Tex`（`TUniquePtr`）をシーンが生存中ずっと保持していないと、描画時に解放済みメモリを指します。

### 2) 手続き生成テクスチャ（PNG を用意しない）

ピクセルを CPU で作って `CreateRhiTexture` に渡す。RGBA 8bit・tightly-packed・上→下/左→右の順（sample 02 / 56 がこれ）。

```cpp
constexpr u32 N = 64;
u8 px[N * N * 4];
for (u32 y = 0; y < N; ++y)
  for (u32 x = 0; x < N; ++x) {
      const usize i = (y * N + x) * 4;
      px[i+0] = 220; px[i+1] = 80; px[i+2] = 200; px[i+3] = 255; // RGBA
  }

FTextureDesc td{};
td.width = N; td.height = N;
td.format = EFormat::R8G8B8A8_UNorm;
td.initial_data = px;
td.initial_data_size = sizeof(px);
auto r = CreateRhiTexture(*device, td);
if (r.IsOk()) m_Tex = Move(r.Value());
```

### 3) HUD をピクセル座標で描く（`OnDrawHud`）

`AScene::OnDrawHud(rc, sb)` は **画面ピクセル座標**で呼ばれます（world view ではない）。背景バー + テキスト（sample 56 抜粋）：

```cpp
void OnDrawHud(FRenderContext& rc, CSpriteBatch& sb) noexcept override {
    sb.DrawRect(8.0f, 8.0f, 470.0f, 32.0f, FVec4{0, 0, 0, 0.45f});  // 半透明バー
    if (!rc.HasFont()) return;            // フォントが無い環境では描かない
    sb.DrawString(rc.GetFont(), "Score: 1200", 16.0f, 15.0f,
                  FVec4{0.9f, 0.95f, 1.0f, 1.0f});
}
```

### 4) world にプリミティブ（グリッド線・三角形）を直接描く（`OnDrawWorld`）

`OnDrawWorld(rc, sb)` は **ワールド座標**（カメラ適用済み）で呼ばれます。sample 55 のグリッド：

```cpp
void OnDrawWorld(FRenderContext& /*rc*/, CSpriteBatch& sb) noexcept override {
    for (i32 x = -8; x <= 8; ++x) {
        const FVec4 c = (x == 0) ? FVec4{0.25f,0.35f,0.45f,0.65f}
                                 : FVec4{0.12f,0.14f,0.18f,0.45f};
        sb.DrawRect(x - 0.01f, -3.0f, 0.02f, 6.0f, c);  // 縦線(ワールド単位)
    }
}
```

---

## 注意点（gotcha）

- **座標系が view で違う**。`OnDrawWorld` はワールド単位（カメラ/`SetPixelsPerUnit` 適用後）、`OnDrawHud` は画面ピクセル（左上原点・Y 下向き）。`CSpriteBatch` 単体の素のデフォルトもピクセル左上原点。`ASprite2DComponent` のサイズは常にワールド単位。
- **`SetView` は world view 用**。`AScene` が world パスで `SetView` を設定するので、HUD 描画とは別 view。素の `CSpriteBatch` で手動カメラを使うときは `Begin()` が view を恒等にリセットする点に注意（`Begin` 後に `SetView` する）。
- **テクスチャは非所有**。`ASprite2DComponent::SetTexture(IRhiTexture*)` も `CSpriteBatch::Draw(IRhiTexture&)` もポインタ/参照を受けるだけ。`TUniquePtr<IRhiTexture>` をシーン/アプリが生かし続けること。解放済みを指すと描画でクラッシュ/破損。
- **`max_sprites` 上限**。素の `CSpriteBatch::Init(..., max_sprites)` の上限を超えるとそのフレームの溢れ分は描かれない。HUD + world + パーティクルが多いシーンは余裕を持って（既定 4096）。
- **`pivot` は 0..1 正規化**。ピクセル値ではない。`{0.5,0.5}` が中心、`{0,0}` が（サイズ基準で）端。
- **PNG ラッパは無い**。「文字列 1 行でスプライト」は `acs::easy`（`src/easy/`、`FSprite LoadSprite(path)`）にはあるが、gameframework 側には無い。gameframework では上記パターン 1 の `Load`→`UploadTexture`→`SetTexture` を自分で書く。
- **`UvRect` を使うとアニメと衝突しうる**。`ASpriteAnimComponent` が毎フレーム `SetUvRect` を上書きするので、アニメ付きノードで手動 `SetUvRect` しても上書きされる（アニメ側に任せる）。
- **`SetStencilMode` の前提**。stencil 付き深度バッファが bind されたパス（`AScene::SetStencilMaskEnabled(true)`）以外で呼ぶと DSV 不整合。マスクが要らないなら触らない。

---

## 動くサンプル

- `acs/samples/55_HelloScene2D/Scene2DStarter.cpp` — `ASprite2DComponent`（単色矩形）+ `OnDrawWorld` のグリッド + `OnDrawHud` のバー + カメラ追従。基本形はこれ。
- `acs/samples/56_HelloSpriteAnim/SpriteAnimDemo.cpp` — 手続き生成スプライトシート（`CreateRhiTexture`）+ `SetTexture` + UV アニメ + `DrawString` HUD。
- `acs/samples/02_HelloSprite/HelloSpriteApp.cpp` — 素の `CSpriteBatch` を自前 `Init`/`Begin`/`Draw`/`DrawRect`/`End` する低レベル例（`AScene` を使わない）。
- PNG→texture の実経路（`Load`→`UploadTexture`→`static_cast<FImageAsset*>`）は `acs/src/easy/Easy.cpp` の `LoadSprite` 実装が参考になる。
