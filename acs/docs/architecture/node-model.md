# ノードモデル

`ANode` はACS Game Frameworkの所有対象で、1つの親、子ノード、`AComponent`、`FTransform3D` を保持します。

## 所有権

- ノードは `NewObject<T>()` で生成します。
- `AddChild` が成功すると、親ノードが子ノードの強参照を所有します。
- 外部から保持する場合は `TWeakObjectPtr<ANode>` を使い、ノードの寿命を延長しません。
- `AComponent` は追加先ノードが所有し、所有元より長く生存しません。

`TryAddChild` は `EAddChildResult` で結果を返します。null、自己追加、循環、既に親を持つ子、親または子の破棄予定、追加後のツリー深度が `kNodeMaxTreeDepth` の512を超える場合を拒否します。失敗時は渡された強参照の所有権を変更しません。

## ライフサイクル

```text
AddChild
  └─ OnSpawn
       ├─ OnUpdate
       ├─ OnFixedUpdate
       └─ OnDraw
Destroy
  └─ OnDespawn
```

`AddChild` / `TryAddChild` は成功時に親子関係へ即時反映し、未生成の子へ `OnSpawn` を呼びます。更新走査中に追加された子は、同じフレームの後続走査へ参加する場合があります。

`Destroy` と `Reparent` は `ResolveStructuralChanges` まで保留されます。`Destroy` の確定時は `OnDespawn` を呼びますが、`Reparent` では `OnSpawn` / `OnDespawn` を呼びません。破棄予定のノードは新しい処理へ参加させません。

## 変換の扱い

正本は `FTransform3D` です。2D処理では `Position2D`、`Rotation2D`、`Scale2D` と設定関数を使います。

- `SetPosition2D` はZ位置を保持します。
- `SetScale2D` はZ軸の倍率を保持します。
- `SetRotation2D` はクォータニオン全体をZ軸回転へ置き換えます。

親の変換は子のワールド変換へ伝播します。循環した階層を作れないため、ワールド変換の計算は親方向へ有限回で終了します。

## シーンとの関係

`FScene2D` はルート `ANode` を所有し、2D更新と描画へ接続します。`FScene3D` は独立した3Dシーングラフで、`FScene` との互換接続には `FLegacyScene3DAdapter` を使用します。

個別の型とメンバーは[機能・APIリファレンス](../reference/index.html)から参照できます。
