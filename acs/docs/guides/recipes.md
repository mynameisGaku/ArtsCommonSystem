# ACS レシピ集

[クイックスタート](../getting-started/quickstart.md)でビルドと最小シーンを確認した後に使う、短い実装例です。各型と関数の引数、戻り値、メンバーは[機能・APIリファレンス](../reference/index.html)から個別ページを開けます。

## 2Dノードを作る

```cpp
acs::game::ANode& node =
    Root().AddChild(acs::NewObject<acs::game::ANode>());
node.SetPosition2D(acs::FVec2{2.0f, 1.0f});

auto& sprite = node.AddComponent<acs::game::ASprite2DComponent>(
    acs::FVec2{1.5f, 1.5f},
    acs::FVec4{1.0f, 0.35f, 0.2f, 1.0f});
```

`ASprite2DComponent` はテクスチャ未設定時に単色矩形を描きます。`SetTexture` に渡す `IRhiTexture*` は非所有なので、描画中はシーンまたはアセット管理側でテクスチャの寿命を維持します。

## 名前付き入力を評価する

```cpp
inline constexpr acs::game::FActionId kMoveX{"MoveX"};

void ConfigureInput(acs::game::FInputMap& input_map) noexcept
{
    input_map.BindAxisKeys(kMoveX, acs::EKey::A, acs::EKey::D);
}

void OnFixedTick(acs::f32 fixed_dt) noexcept override
{
    const acs::game::FInputActionState move =
        Services().Input().Evaluate(kMoveX, Services().FixedInput());
    const acs::f32 distance = move.axis * 4.0f * fixed_dt;
    Root().SetPosition2D(
        Root().Position2D() + acs::FVec2{distance, 0.0f});
}
```

`Services().FixedInput()` は押下と解放を1つの固定ステップへ一度だけ配送します。可変更新で現在のプラットフォーム入力を評価する場合は、`FInputMap::IsPressed`、`IsHeld`、`IsReleased`、`Axis` も使用できます。

## 複数の保存値を一度に変更する

```cpp
const bool applied =
    acs::TryApplyStorageBatch(storage, updates, update_count);
```

`false` の場合は、`storage` が呼び出し前の状態を保ちます。入力配列の定義、検証条件、必要なヘッダーは[CStorageの原子的バッチ更新](runtime/storage-batch.md)を参照してください。ファイルへの保存は `CStorage` の保存APIで別に行います。

## RHIリソースの作成失敗を扱う

```cpp
acs::FTextureDesc description{};
description.width = 64u;
description.height = 64u;
description.format = acs::EFormat::R8G8B8A8_UNorm;
description.initial_data = pixels;
description.initial_data_size = 64u * 64u * 4u;

auto texture_result = acs::CreateRhiTexture(device, description);
if (texture_result.IsErr())
{
    ACS_LOG_ERROR("textureを作成できません");
    return;
}
acs::TUniquePtr<acs::IRhiTexture> texture =
    acs::Move(texture_result.Value());
```

`TResult<T>` の `Value()` は `IsErr()` を確認した後に読みます。作成したRHIリソースは、記録済みの描画命令が完了するまで有効な所有者が保持します。

## HUDへ文字を描く

```cpp
void OnDrawHud(
    acs::game::FRenderContext& render_context,
    acs::FSpriteBatch& sprite_batch) noexcept override
{
    if (!render_context.HasFont())
        return;
    sprite_batch.DrawString(
        render_context.GetFont(),
        "スコア 1200",
        24.0f,
        24.0f,
        acs::FVec4{1.0f, 1.0f, 1.0f, 1.0f});
}
```

フォントは `FGame` が `FRenderContext` へ配線します。未準備のフレームでは `HasFont()` が `false` です。

## 3D水面へ波紋を追加する

```cpp
if (!water.AddDisturbanceForSurface(
        surface_id,
        hit_position,
        0.35f,
        0.8f))
{
    // 対象水面の衝撃波用保存枠が満杯です。
}
```

描画時の `DrawMesh` または `DrawAdaptivePlane` に同じ `surface_id` を渡します。詳細は[3D水面](rendering/interactive-water-3d.md)を参照してください。

## 次に参照する文書

- [Game Frameworkチュートリアル](../tutorials/README.md)
- [アセットガイド](assets/README.md)
- [Editorガイド](editor/README.md)
- [描画ガイド](rendering/README.md)
- [機能・APIリファレンス](../reference/index.html)
