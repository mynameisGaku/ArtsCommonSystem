# 05 タイルマップ

`FTilemap` はグリッド寸法、タイル寸法、`FTileId` を保持します。`ATilemapComponent` をノードへ追加すると、シーンの2D描画へ接続できます。

```cpp
acs::game::ANode& node = Root().AddChild(acs::NewObject<acs::game::ANode>());
auto& tilemap = node.AddComponent<acs::game::ATilemapComponent>();
tilemap.Map().Init(32, 18, 1u, 1.0f);
tilemap.Map().Fill(acs::game::FTileId{1});
tilemap.Map().FillRect(4, 4, 11, 6, acs::game::FTileId{2});
```

`Init` の第3引数はレイヤー数、第4引数はワールド単位のタイルサイズです。`FillRect` の `x0`、`y0`、`x1`、`y1` は両端を含むセル座標です。上の例は横8セル、縦3セルを変更します。テクスチャアトラスを設定しない場合は、タイルID由来のデバッグ色で描画します。

`TryLoadTiledJson` は対応するJSONをタイルマップへ読み込みます。読込失敗時は部分的なマップを公開しません。

`BuildRigidColliders` は呼び出した時点の非空タイルを行ごとにまとめ、`FRigidWorld2D` へ静的AABBを追加します。既存形状は削除せず、ノードの変換も自動では反映しません。タイル変更後に再構築する場合は、専用ワールドを初期化し直すか、生成した形状の所有範囲を呼び出し側で管理します。

[次章: カメラ](06-camera.md)
