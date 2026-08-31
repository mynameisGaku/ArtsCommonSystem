# 03 スプライト描画

`ASprite2DComponent` はノードの2D変換、サイズ、色合い、テクスチャを使ってスプライトを描画します。テクスチャを指定しない場合は単色矩形として描画できます。

```cpp
acs::game::ANode& node = Root().AddChild(acs::NewObject<acs::game::ANode>());
node.SetPosition2D(acs::FVec2{2.0f, 1.0f});
node.AddComponent<acs::game::ASprite2DComponent>(
    acs::FVec2{1.5f, 1.5f},
    acs::FVec4{1.0f, 0.35f, 0.2f, 1.0f});
```

`FScene2D::OnDrawWorld` 内の `CSpriteBatch` はシーンのカメラビューが設定されるため、座標はワールド単位です。単体の `CSpriteBatch` は画面座標で使用できます。同じAPIでも接続されているビューを確認します。

テクスチャアセットは `CAssetRegistry::Load` で読み込み、`UploadTexture` でGPUリソースへ変換します。GPUテクスチャは描画中に有効な所有者が保持します。

HUDテキストは `FRenderContext::HasFont()` を確認し、`OnDrawHud` へ渡された `FSpriteBatch` から `sb.DrawString(rc.GetFont(), ...)` を呼びます。

[次章: スプライトアニメーション](04-sprite-animation.md)
