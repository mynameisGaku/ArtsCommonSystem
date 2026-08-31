# 06 カメラ

`FScene2D` は `FCamera2D` サービスを使ってワールド描画の中心と拡大率を決めます。

```cpp
acs::game::FCamera2D& camera = Services().Camera();
camera.SetTargetPos(player.Position2D());
camera.SetZoom(1.25f);
camera.SetDeadzone(acs::FVec2{2.0f, 1.0f});
```

`SetTargetPos` は位置の値を保存するため、動く対象を追従する場合は `OnTick` などで毎フレーム更新します。追従、デッドゾーン、中心位置の境界、`trauma`による画面揺れを組み合わせられます。`SetBounds` はカメラ中心を制限し、ビューポート全体が境界内に収まることまでは保証しません。

`FScene2D::ScreenToWorld` は、シーンの1単位あたりの画素数、ズーム、カメラ中心と対応する選択判定用変換です。`FCamera2D` 単体の `ScreenToWorld` / `WorldToScreen` は画面サイズを明示して使います。

`FCamera2D::SetRotation` は値を保持しますが、現在の `FScene2D` のSpriteBatchビューと `FScene2D::ScreenToWorld` は回転を適用しません。2Dシーンの描画や選択判定が回転するものとして扱いません。

[次章: 入力](07-input.md)
