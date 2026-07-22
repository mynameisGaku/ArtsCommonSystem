# FBehaviorTree 永続化の安全性

`FBehaviorTreeEditorPanel` は編集可能なビヘイビアグラフを、行指向の
`ACSBT` テキスト形式で永続化します。新規コードでは次の検証付き API を使います。

- `TryParseGraphText(text, size)` — 長さを明示したメモリ上の文書を解析する。
- `TryLoadGraph(path)` — 上限付きの完全なファイルスナップショットを読み込む。
- `TrySaveGraph(path)` — canonical v4 形式でシリアライズし、原子的に置換する。

従来の `LoadGraph` と `SaveGraph` は互換 wrapper として残り、対応する検証付き操作が
成功したかを返します。新しい serializer は `ACSBT 4` を書き出し、reader は引き続き
version 1〜4 を受け付けます。

## 上限と文法

- 文書: 最大 256 KiB、物理行 256 行。
- 1 行: CR/LF を除いて最大 255 bytes。
- グラフ: 最大 128 nodes、親 chain の深さ 64。
- 動的 blackboard: 最大 64 variables。
- node 表示名と variable 名: 最大 47 bytes。
- path: 終端 NUL を除いて最大 1,023 bytes。

すべての node ID は `0..node_count-1` にちょうど 1 回だけ現れる必要があります。
親参照は宣言済み node を指すか、root を示す `-1` でなければなりません。親 cycle、
深さ超過、leaf node への子の追加、2 個以上の子を持つ decorator は拒否します。
enum 値は範囲を検証し、node 座標、比較定数、浮動小数点 blackboard 値は有限値だけを
受け付けます。

v4 の blackboard section は必須で、`BB <count>` の後に正確に
`<name> <type> <value>` records が続きます。名前は印字可能な単一 token とし、
重複を拒否します。bool は `0` または `1`、整数は `i32` の範囲、浮動小数点数は
locale 非依存の文法でなければなりません。埋め込み NUL、不完全または余分な非空
record、長すぎる行、未対応 version、誤った magic はすべてエラーです。

## transaction とファイル保証

parser は node と blackboard 値を staging storage に構築します。参照、構造、深さ、
重複の検証を完了してから最終 node array を確保し、検証と確保がともに成功した場合
だけ live graph を置き換えます。失敗時は graph、選択/layout 状態、呼び出し側が渡した
動的 blackboard のいずれも変更しません。blackboard section を持たない legacy v1〜v3
文書では、現在の動的 blackboard を維持します。

ファイル読み込みは初期 size を snapshot し、その byte 数を正確に読み、予期しない
伸長、read、close の失敗を検証してから transactional parser を呼びます。短縮または
同時伸長されたファイルは `FileChanged` として報告します。

保存前にメモリ上の全 field を検証し、整数と浮動小数点数を locale 非依存の
`to_chars` でシリアライズします。出力先 directory に一意な一時ファイルを書き、
flush、close 後に出力先を原子的に置換します。シリアライズ、write、flush、close、
置換のいずれかが失敗した場合、既存の出力先を維持して一時ファイルを削除します。

## 診断とテスト

`FBtGraphPersistenceResult` は安定した `EBtGraphPersistenceError`、source line、
処理済み byte 数、任意の OS error を返します。`ErrorName` はログ向けの安定した
診断名を提供します。

`tests/behavior_tree_persistence_safety_tests.cpp` は canonical parse と runtime bake、
v1 互換、graph/blackboard 状態不変、ID/名前重複、不正参照、cycle、非有限値、
埋め込み NUL と size 上限、canonical file round-trip、検証失敗時の出力先保護、
安定した error 名を検証します。
