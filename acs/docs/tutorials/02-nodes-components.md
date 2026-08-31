# 02 ノードとコンポーネント

`ANode` は階層、変換、コンポーネントを所有します。機能は `AComponent` としてノードへ追加します。

```cpp
class ARotateComponent final : public acs::game::AComponent
{
public:
    ACS_GAME_COMPONENT_KIND(ARotateComponent)

    void OnUpdate(acs::f32 dt) noexcept override
    {
        Owner().SetRotation2D(Owner().Rotation2D() + dt);
    }
};
```

シーンの `OnReady` では次のように追加します。

```cpp
acs::game::ANode& node = Root().AddChild(acs::NewObject<acs::game::ANode>());
node.AddComponent<ARotateComponent>();
```

`AddComponent<T>` は新しいコンポーネントを追加し、`GetComponent<T>` は既存コンポーネント、`GetOrAddComponent<T>` は既存または新規コンポーネントを返します。`RemoveComponent<T>` は対象コンポーネントの `OnDetach` を呼んでその場で除去します。コンポーネントを走査している途中では呼ばず、更新の前後など走査外の位置で使います。`Destroy` は破棄予定を記録し、次の `ResolveStructuralChanges` でノードを除去します。

`AddChild` の簡易 API では失敗理由を得られないため、入力を検証する必要がある処理では `TryAddChild` と `EAddChildResult` を使います。`OnSpawn` はまだ生成されていない子へだけ呼ばれます。

[次章: スプライト描画](03-sprites.md)
