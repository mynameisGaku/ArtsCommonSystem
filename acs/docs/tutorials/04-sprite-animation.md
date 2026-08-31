# 04 スプライトアニメーション

`ASpriteAnimComponent` は `ASprite2DComponent` のUVをフレームごとに更新します。等間隔のスプライトシートは `InitGrid`、任意配置はUVフレーム列を使います。

```cpp
void ConfigureAnimation(acs::game::ANode& node, acs::IRhiTexture& sprite_sheet) noexcept
{
auto& sprite = node.AddComponent<acs::game::ASprite2DComponent>(
    acs::FVec2{1.0f, 1.0f});
sprite.SetTexture(&sprite_sheet);

auto& animation = node.AddComponent<acs::game::ASpriteAnimComponent>();
animation.InitGrid(2, 1, 2, 4.0f);
animation.Play();
}
```

`EPlayMode` でループ、一度だけ、往復を選び、再生、停止、速度を制御します。フレーム位置の変更とフレームイベントの登録には `animation.Animator().SetCurrentFrame(...)` と `animation.Animator().AddFrameEvent(...)` を使います。`AddFrameEvent` は更新後に到達したフレームと一致するイベントを通知します。大きな `dt` で飛び越えた全フレームのイベントを列挙する契約ではありません。

`FSpritePack` はアトラスメタデータ、スプライト検索、UV計算をまとめます。`ASprite2DComponent` が保持するテクスチャポインターは非所有です。テクスチャリソースはシーンまたはアセット管理側で描画中の寿命を維持します。

[次章: タイルマップ](05-tilemap.md)
