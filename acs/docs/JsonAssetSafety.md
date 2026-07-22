# JSONアセット境界の安全契約

Tiled mapとAseprite / TexturePacker atlasは、信頼できない外部入力として
`FTilemap::TryLoadTiledJson` と `FSpritePack::TryLoadAtlasJson` から読み込みます。
従来の `LoadTiledJson` / `LoadAtlasJson` は互換ラッパーで、checked結果の安定した
エラー番号を `FErrorCode::subcode` へ伝えます。

## 共通JSONパーサーとの責務分担

共通の `ParseJson` はJSON文法、Unicode escape、末尾junk、再帰深度256を検査します。
一方、次の制約は持ちません。

- 入力全体、文字列、node数の用途別上限
- objectの重複key
- 数値overflow後のInf
- アセットschema、次元、配列長、積算サイズ
- DOM構築中の回復可能なOOM通知

各checked loaderは、DOMを作る前に入力byte、embedded NUL、文字列のraw長、
深度64を走査します。DOM作成後は全nodeを巡回し、node数、decode後文字列長、
重複key、NaN / Infを検査します。その後に用途別schemaを全件検証し、最後に
`TryReserve` / `TryResize` / `TryAppend`だけで別storageへ構築します。

共通パーサー内部のDOM allocationは現状fail-fastです。このためloaderの事前上限は
DOM allocation量を抑える重要な防御ですが、OSレベルの極端なOOMを
`TryLoad...` の戻り値へ変換するものではありません。loaderが所有する最終storageの
allocation failureは `AllocationFailure` として回復可能で、既存状態を変更しません。

## FTilemap

| 項目 | 上限 |
|---|---:|
| JSON入力 | 8 MiB |
| JSON深度 | 64 |
| JSON文字列 | 4,096 bytes |
| JSON node | 1,100,000 |
| width / height | 各2,048 |
| 1 layerのcell | 262,144 |
| tile layer | 32 |
| 全layerのcell合計 | 1,048,576 |

`width`、`height`は正の整数、`tilewidth`は正の有限数、`layers`は配列でなければ
なりません。各tile layerの`data`は必須で、要素数は厳密に`width * height`です。
不足分のゼロ埋めや余剰分の切り捨ては行いません。全GIDは有限な`u32`整数として検証し、
Tiledのflip bitを除去した後、従来互換の`u16` clampを適用します。

`width * height`と`cells * layers`は除算による上限確認後だけ乗算し、整数wrapと
過少確保を防ぎます。全layerを別の二重`TArray`へ構築後、失敗しないmoveだけで
commitします。失敗時は既存の次元、tile値、capacity、`LayerData` pointerが不変です。

Tiledはexporterやplugin固有memberが多いため、未知memberと非tile layerは許可します。
既知memberの型違いと、object内の重複memberは拒否します。

`TryInit`も同じ次元・積算・allocation契約を使います。互換`Init`は従来どおり0を
最小値へ補正しますが、上限超過やOOMでは既存mapを保持します。

## FSpritePack

| 項目 | 上限 |
|---|---:|
| JSON入力 | 4 MiB |
| JSON深度 | 64 |
| JSON文字列 | 4,096 bytes |
| JSON node | 100,000 |
| frame数 | 4,096 |
| frame名 | 255 bytes |
| image path | 1,024 bytes |
| atlas width / height | 各65,535 |

hash形式とarray形式を受け付けます。`meta.image`と`meta.size.w/h`は必須です。
frameの`x/y/w/h`は`u32`整数、`w/h`は1以上でなければなりません。
`x + w`を直接計算せず、`w <= atlas_width - x`の形でatlas内包とoverflowを
同時に検証します。pivotが存在する場合は`x/y`両方を必須とし、有限な`[0,1]`だけを
許可します。

JSON objectの重複keyと、hash / array両形式の重複frame名を拒否します。exporter固有の
`rotated`、`trimmed`等の未知memberは前方互換のため許可します。

image path、frame名、frame配列をすべて別storageへ構築後にmove commitし、最後に
内部所有image pathへ`m_Info.atlas_texture_path`を結び直します。失敗時はframe配列、
owned name/image、capacity、`AllFrames` / `FindFrame` / `Info`が公開済みのpointerを
含めて不変です。

両クラスはallocator注入constructorを持ちます。allocatorはオブジェクトより長く
生存させる必要があります。すべての永続配列と文字列は同じ注入allocatorを使用するため、
OOMテストとゲーム側のallocation policyを一貫させられます。

## 診断と更新規則

checked結果は固定enum、元JSON parserのsubcode、失敗したlayer / elementまたはframe
indexを返します。ログには `TilemapLoadErrorName` / `SpritePackLoadErrorName` を
使用できます。既存enum値は変更・再利用せず、新しい理由は末尾へ追加してください。
