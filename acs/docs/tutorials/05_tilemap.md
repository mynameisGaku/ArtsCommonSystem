# タイルマップ (FTilemap / ATilemapComponent)

格子状にタイル ID を並べてマップを作る仕組み。`FTilemap` が **データ本体**（タイル ID の 2D 配列＋レイヤー＋座標変換）、`ATilemapComponent` がそれを `AScene` 上で **アトラステクスチャを使って描画**する `AComponent` です。床/壁/水たまりのようなステージ地形を組むとき、当たり判定付きの地面を作るときに使います。

> 名前空間は `acs::game`。タイル ID は `FTileId{u16}` で、**0 = 空（描画されない）**、1 以上が実タイルです。

---

## 最小例

ノードに `ATilemapComponent` を付け、`Map()` でデータを初期化してから atlas を割り当てるだけで描画されます。

```cpp
#include "gameframework/GameFramework.h"
using namespace acs;
using namespace acs::game;

class ALevelScene final : public AScene {
    /** 2D の標準サービス構成 (Default2D | Camera2D | Physics2D) を要求する。 */
    ESvc WantedServices() const noexcept override { return kScene2DServices; }

public:
    void OnReady() noexcept override {
        SetPixelsPerUnit(48.0f);                 // world 1.0 = 48px で見る

        auto node = NewObject<ANode>();
        node->SetPosition2D(FVec2{-8.0f, -5.5f});   // マップ原点（左上寄り。row 0 が上）

        auto& tm = node->AddComponent<ATilemapComponent>();
        tm.Map().Init(/*w=*/16, /*h=*/11, /*layers=*/1, /*tile_size=*/1.0f);
        tm.Map().Fill(FTileId{1});                       // 全面を tile 1 で
        tm.Map().FillRect(0, 0, 15, 0, FTileId{2});      // 上端 1 列を tile 2 で

        // アトラスを割り当てる (未設定だと tile id 由来の色でデバッグ描画)
        // m_Atlas は 2x2 セルのテクスチャと仮定
        tm.SetTexture(m_Atlas.Get());
        tm.SetAtlasGrid(/*cols=*/2, /*rows=*/2);

        Root().AddChild(Move(node));
    }
private:
    TUniquePtr<IRhiTexture> m_Atlas;   // OnReady でテクスチャを生成して保持
};
```

ポイントは3つだけ：`Init(w, h, layers, tile_size)` → タイルを置く（`Fill`/`FillRect`/`SetTile`）→ `SetTexture` + `SetAtlasGrid`。

---

## 主要API

### FTilemap（データ本体）

| メソッド | 説明 |
| --- | --- |
| `Init(u32 w, u32 h, u32 layers=1, f32 tile_size=16.0f)` | グリッドを生成し全タイルを 0 でクリア。引数 0 は安全な既定にフォールバック（`tile_size` の NaN/負値は 16.0f に） |
| `TryInit(...) -> FTilemapLoadResult` | 上限・積算オーバーフロー・確保失敗を検査して、成功時だけ置換する厳格版。外部入力にはこちらを推奨 |
| `SetTile(u32 x, u32 y, FTileId t, u32 layer=0)` | 1 マスを設定。範囲外は no-op |
| `GetTile(u32 x, u32 y, u32 layer=0) -> FTileId` | 1 マス取得。範囲外は `FTileId{0}`（空） |
| `Fill(FTileId t, u32 layer=0)` | レイヤー全体を埋める |
| `FillRect(u32 x0, u32 y0, u32 x1, u32 y1, FTileId t, u32 layer=0)` | **閉区間** `[x0..x1]×[y0..y1]` を塗る。反転は swap、境界で clamp |
| `Clear()` | 全レイヤーを空に（サイズは保持） |
| `Width() / Height() / LayerCount() / TileSize()` | 寸法の取得 |
| `TileToWorld(u32 x, u32 y) -> FVec2` | tile (x,y) の **中心** world 座標（`(x+0.5)*tile_size`） |
| `WorldToTile(FVec2 world, u32& out_x, u32& out_y) -> bool` | world → tile。グリッド外/負値は `false`（成功時のみ out に書き込む） |
| `LayerData(u32 layer) -> const FTileId*` | レイヤーの生配列（row-major `y*W+x`、要素数 `W*H`）。範囲外は `nullptr` |

```cpp
FTilemap map;
map.Init(64, 48, /*layers=*/2, /*tile_size=*/1.0f);
map.SetTile(10, 5, FTileId{2}, /*layer=*/0);
map.FillRect(0, 0, 4, 4, FTileId{3}, /*layer=*/1);   // layer1 に 5x5 (閉区間!) 塗り
FTileId t = map.GetTile(10, 5, 0);                   // -> FTileId{2}
```

> `FTilemap` は **非コピー・非ムーブ**です（シーン所有/プール前提）。`ATilemapComponent::Map()` 経由で触るのが基本。

### FTileId

```cpp
struct FTileId {
    u16 value = 0;
    bool IsEmpty() const;          // value == 0
    // operator== / != あり
};
```

`FTileId{0}` は空（描画されない）。**実タイルは 1 から**始まり、atlas のセル index `(value - 1)` に対応します。

### ATilemapComponent（描画＋当たり判定）

| メソッド | 説明 |
| --- | --- |
| `Map() -> FTilemap&` | 所有するタイルマップへのアクセス。Init/Fill/SetTile はここ経由 |
| `SetTexture(IRhiTexture* tex)` | アトラステクスチャ（**非所有**）を設定。未設定だと ID 由来色でデバッグ描画 |
| `SetAtlasGrid(u32 cols, u32 rows)` | アトラス内のタイル格子（既定 1×1）。0 は 1 に補正 |
| `SetTint(FVec4 tint)` | 全タイルの乗算カラー（既定 白 `{1,1,1,1}`） |
| `BuildCollision(FCollisionWorld2D& world, u32 layer, u32 collision_layer_bit)` | 指定レイヤーの非空タイルを 1 マス = 1 AABB として物理ワールドへ登録 |
| `OnDraw(FRenderContext& rc)` | フレームワークが自動で呼ぶ。手動呼び出し不要 |

---

## よく使うパターン

### 1. 枠付きの部屋を作る（サンプル 58 そのまま）

`Fill` で下地を敷き、`FillRect` で 1 マス幅の壁を 4 辺に置きます。`FillRect` が **閉区間**なので、右端は `15..15`（マップ幅 16 → 最大 index 15）と書きます。

```cpp
auto& tm = node->AddComponent<ATilemapComponent>();
tm.Map().Init(/*w=*/16, /*h=*/11, /*layers=*/1, /*tile_size=*/1.0f);
tm.Map().Fill(FTileId{1});                  // 全面 草 (cell0)
tm.Map().FillRect(0,  0, 15,  0, FTileId{2}); // 上端 石 (cell1)
tm.Map().FillRect(0, 10, 15, 10, FTileId{2}); // 下端 石
tm.Map().FillRect(0,  0,  0, 10, FTileId{2}); // 左端 石
tm.Map().FillRect(15, 0, 15, 10, FTileId{2}); // 右端 石
tm.Map().FillRect(6,  4,  9,  6, FTileId{3}); // 中央に 水たまり (cell2)
tm.SetTexture(m_Atlas.Get());
tm.SetAtlasGrid(2, 2);                       // 2x2 アトラス → cell0..3
```

### 2. アトラスを手で作る（テクスチャアセットが無いとき）

サンプル 58 は 2×2（各 32px）の色ブロックを CPU で生成しています。`SetAtlasGrid(cols, rows)` の値と、置く `FTileId` が指すセルが一致していることが重要です（`value-1` が `row*cols+col`）。

```cpp
// 2x2 = 4 セルのテクスチャを作り、cell index と tile id を対応させる:
//   FTileId{1} -> cell0,  FTileId{2} -> cell1,  FTileId{3} -> cell2,  FTileId{4} -> cell3
FTextureDesc td{};
td.width = 64; td.height = 64; td.format = EFormat::R8G8B8A8_UNorm;
td.initial_data = px; td.initial_data_size = 64 * 64 * 4u;
auto r = CreateRhiTexture(*dev, td);
if (r.IsOk()) m_Atlas = Move(r.Value());
```

### 3. 壁レイヤーを当たり判定に変換する

ソリッドにしたいタイルだけ別レイヤーに置いておき、そのレイヤーを物理ワールドへ流し込みます。1 タイル = 1 AABB（中心 = `TileToWorld + origin`、ハーフサイズ = `tile_size/2`）になります。

```cpp
// 例: layer 1 を「壁」専用にして、kWall ビットで衝突世界へ
constexpr u32 kWall = 1u << 0;
tm.BuildCollision(Services().Physics(), /*layer=*/1, /*collision_layer_bit=*/kWall);
```

> `BuildCollision` は呼んだ時点のタイル配置を **スナップショット**して AABB を追加します。後からタイルを書き換えても既存の AABB は更新されません（再構築は利用者責任）。

### 4. マウス座標からタイルを引く（hit-test）

`WorldToTile` でワールド座標 → タイル index に変換。ただし `FTilemap` 内部はマップ原点を考慮しないので、**ノードの world 位置を引いてから**渡します。

```cpp
const FVec2 origin = node->World2D().position;
FVec2 local = FVec2{mouse_world.x - origin.x, mouse_world.y - origin.y};
u32 tx, ty;
if (tm.Map().WorldToTile(local, tx, ty)) {
    FTileId hovered = tm.Map().GetTile(tx, ty, 0);
}
```

---

## 注意点 (gotcha)

- **tile id 0 は空**。実タイルは 1 から。`SetAtlasGrid(cols, rows)` のとき `FTileId{v}` は atlas セル `v-1`（= `col = (v-1)%cols`, `row = ((v-1)/cols)%rows`）に対応。セル数を超える ID を置くと wrap して別セルが出ます。
- **`FillRect` は閉区間** `[x0..x1]×[y0..y1]`。半開区間ではないので、幅 16 のマップ右端は `15`、`16` ではありません。`x0>x1` は自動 swap、はみ出しは clamp。
- **マップ原点はノードの world 位置**。`FTilemap` 自体は原点 (0,0) 基準で `TileToWorld` を返し、描画/`BuildCollision` 時に `Owner().World2D().position` を足します。マップ全体を動かしたいときはノードを動かす。
- **`TileToWorld` はタイルの中心**を返す（`(x+0.5)*tile_size`）。グリッド線ではありません。スプライト描画基準に合わせた仕様。
- **`WorldToTile` は負値/原点未満を `false`** にします（早期 reject）。マップ原点をオフセットしているときは前述のとおりローカル座標に直してから渡すこと。
- **`SetTexture` は非所有**。テクスチャの寿命はあなたが管理（サンプルではシーンが `TUniquePtr<IRhiTexture>` で保持）。コンポーネントより先に破棄すると dangling。
- **`tile_size` は world 単位**。サンプル 58 は `tile_size=1.0f` + `SetPixelsPerUnit(48.0f)` で「1 タイル = 画面上 48px」。ピクセル基準にしたいなら `tile_size=16.0f`/`32.0f` + PPU=1 などにする。
- **`FTilemap` は非コピー・非ムーブ**。値で持ち回らず、`AComponent` 経由か参照で扱う。
- **インポータ（実装済み / 未対応の区別）**: Tiled の **JSON 形式 `.tmj` は
  `FTilemap::TryLoadTiledJson(text, len)` で厳格・トランザクション的に読み込めます**。
  `tilelayer` の `data` を行優先で取り込み、GID の上位 flip フラグは除去して
  `FTileId` に clamp します。`LoadTiledJson` は `TResult<void>` を返す互換APIです。
  ファイル文字列は `FFileSystem::ReadAllText` で読んで渡します。検証例は
  `62_HelloPersistVerify` の `TestTilemapLoad` です。一方 **`.tmx`（XML 形式）は未対応**
  です。auto-tiling / per-tile flip の反映 / chunk 化も現状ありません。

---

## 動くサンプル

- **`acs/samples/58_HelloTilemap/TilemapDemo.cpp`** — Package C デモ。Title→Level のフェード遷移付きで、`ATilemapComponent` が 2×2 アトラスのセルとして枠付きの部屋＋中央の水たまりを描画します（実機スクショ確認済み）。アトラステクスチャを CPU で生成する `MakeTileAtlas` も参照になります。

ヘッダ／実装は以下：
- `acs/src/gameframework/Tilemap.h` / `Tilemap.cpp` — データ本体
- `acs/src/gameframework/TilemapComponent.h` / `TilemapComponent.cpp` — 描画＋`BuildCollision`
