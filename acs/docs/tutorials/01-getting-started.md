# 01 はじめに

最小のACSゲームは、`FScene` 派生と `FGame` 派生を1つずつ持ちます。2Dゲームでは `FScene2D` を使うと、ルートノード、カメラ、`SpriteBatch`、固定更新が接続されます。

```cpp
class FMainScene final : public acs::game::FScene2D
{
protected:
    void OnReady() noexcept override
    {
        acs::game::ANode& square = Root().AddChild(acs::NewObject<acs::game::ANode>());
        square.AddComponent<acs::game::ASprite2DComponent>(
            acs::FVec2{64.0f, 64.0f},
            acs::FVec4{0.15f, 0.65f, 1.0f, 1.0f});
    }
};

class FMainGame final : public acs::game::FGame
{
protected:
    acs::TUniquePtr<acs::game::FScene> InitialScene() noexcept override
    {
        return acs::MakeUnique<FMainScene>();
    }
};

ACS_GAME_MAIN(FMainGame)
```

`OnReady` は新しいシーンがスタックへ入ったときに呼ばれます。上に積まれたシーンが取り除かれて再び最上位へ戻った場合は、`OnReady` ではなく `OnResume` が呼ばれます。`OnTick` は可変更新、`OnFixedTick` は固定更新、`OnDrawWorld` と `OnDrawHud` は描画用のフックです。

反射と水深取得を有効にしたフレームでは、`OnDrawWorld` がシーンカラー、水深、メインビューの各描画工程で最大3回呼ばれます。状態変更は更新フックで行い、`OnDrawWorld` では渡された描画先に必要な命令だけを積みます。

シーン切り替え要求は安全な適用境界で処理されます。`FGame::TransitionTo` はフェードの中点で次のシーンへ切り替えます。

[次章: ノードとコンポーネント](02-nodes-components.md)
