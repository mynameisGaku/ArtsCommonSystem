# 10 エフェクト、照明、ステンシル、テキスト

`FScene2D` のノードへ2Dエフェクト用コンポーネントを追加すると、利用側で個別シェーダーを実装せずにシーン描画へ接続できます。

```cpp
acs::game::ANode& water = Root().AddChild(acs::NewObject<acs::game::ANode>());
auto& water_effect = water.AddComponent<acs::game::AWater2DComponent>();
water_effect.SetRect(acs::FVec2{0.0f, 0.0f}, acs::FVec2{4.0f, 1.5f});

acs::game::ANode& fire = Root().AddChild(acs::NewObject<acs::game::ANode>());
fire.AddComponent<acs::game::AFire2DComponent>();
```

`AWater2DComponent` は `SetRect`、`SetEllipse`、`SetRiver`、`SetPolygon` などで水域の形状を設定してから使います。`ContainsPoint` は実際の水面三角形を検査し、`ContainsX` と `SurfaceY` は境界と基準水面を使います。平面反射はシーンで `SetReflectionEnabled(true)` を呼び、水面側で `SetReflection(true, ...)` を設定します。

軌跡は `ATrail2DComponent`、クリップ領域は `AStencilClip2DComponent` を使います。ステンシルを使うシーンでは `SetStencilMaskEnabled(true)` を呼び、`AStencilClip2DComponent` に `SetRect`、`SetCircle`、`SetEllipse`、`SetPolygon` のいずれかで形状を設定します。このコンポーネントは、所有ノードの子ツリーをマスクの内側または外側に限定して描きます。

`CLighting2D` はシーンサービスではなく、描画リソースを所有する側が明示的に保持します。ライト一覧、遮蔽物、描画先の準備と終了を所有者が管理します。

HUDテキストは `OnDrawHud` で `FRenderContext::HasFont()` を確認してから、渡された `FSpriteBatch` で `sb.DrawString(rc.GetFont(), ...)` を呼びます。フォントがないフレームで固定パスから暗黙に読込まないようにします。

[チュートリアル一覧](README.md)
