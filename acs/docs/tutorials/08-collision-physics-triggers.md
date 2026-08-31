# 08 衝突、物理、トリガー

`FCollisionWorld2D` はAABB、円、凸多角形、OBBを形状として登録し、重なり判定、押し戻し、レイキャストを提供します。

```cpp
acs::game::FCollisionWorld2D world;
const acs::game::FShapeId wall = world.AddAabb(
    acs::FAabb2{acs::FVec2{0.0f, 0.0f}, acs::FVec2{2.0f, 0.5f}});

acs::TArray<acs::game::FShapeId> hits;
world.OverlapCircle(acs::FCircle{acs::FVec2{0.0f, 0.0f}, 0.75f}, hits);
```

レイヤーは形状の所属、マスクは問い合わせ対象を表します。問い合わせから自分の形状を除く場合は `exclude` を指定します。

`FAabb2` は最小点と最大点ではなく、中心と半サイズで構築します。`APhysicsBody2D` は `OnUpdate` の可変 `dt` で速度を積分し、移動後の重なりを解消します。移動経路を連続判定するCCDではないため、1フレームで大きく移動すると薄い障害物を通過する可能性があります。速度と障害物の厚みをこの契約に合わせます。

`FTriggerWorld2D` は重なりの開始、継続、終了を管理します。`ATriggerComponent` がコンポーネントへ転送する通知は開始と終了です。`FScene2D` の既定サービスにトリガーは含まれないため、使用するシーンは `WantedServices` に `ESvc::Triggers` を追加します。トリガーの形状同期はコンポーネントの `OnUpdate`、重なり判定と通知はサービスの `PostUpdate` で行われます。

[次章: シーン遷移と保存](09-scene-flow-save.md)
