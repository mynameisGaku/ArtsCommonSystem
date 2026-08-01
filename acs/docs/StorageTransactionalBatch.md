# FStorage の文字列一括設定

`FStorage::TrySetStringBatch` は複数の文字列設定を一つの処理として扱い、
全項目を反映できる場合だけ対象インスタンスを更新します。新しい保存サービスや
共有状態は作らず、呼び出し側が所有する既存の `FStorage` と allocator を使います。

```cpp
#include "platform/Storage.h"

acs::FStorage storage;
const acs::FStorageStringBatchEntry entries[] = {
    {"audio.master", "0.8"},
    {"player.name", "タロウ"},
};

auto result = storage.TrySetStringBatch(entries, 2u);
if (result.IsErr()) {
    // storage は呼び出し前の件数、値、借用ポインタを維持する。
}
```

## 成功値

- `count == 0` は `entries == nullptr` でも `Ok(0)` を返す。
- 成功値は、新規追加した項目と既存値を実際に変更した項目の合計。
- 既存値と同じ値は no-op とし、成功値へ含めない。
- 後述する既存 key の identity 検査を通過し、全項目が no-op の場合は確保せずに
  `Ok(0)` を返し、既存状態を置き換えない。

`value == nullptr` は既存の `TrySetString` と同じく空文字列として扱います。
非 null の `value` は最初の終端までを byte 列として取り込み、batch 固有の UTF-8、
制御文字、INI 文字の検査や変換を追加しません。この wave では既存の単体 setter より
値の制約を強めず、ファイル保存時は従来の INI 表現の契約をそのまま使います。
削除や型付き変換はこの API の責務に含みません。

## 入力契約

一回の入力と反映後のストアは `FStorage::kMaximumStringBatchEntryCount` の
4,096 項目を上限とします。上限件数のストアでも既存項目だけの更新はできます。
既存互換の `TrySetString`、`SetString`、読み込み経路はこの wave で同じ上限へ変更しないため、
呼び出し前から 4,096 件を超える場合があります。その状態への非0件 batch は、全項目が
no-op でも最終件数検査で確保前に拒否し、件数、値、借用ポインタ、allocator 使用量を維持します。
反映前の検査では次を拒否します。

- 非0件数に対する null または非整列の `entries`
- 項目数、配列バイト数、配列終端アドレス、反映後件数の overflow
- null、空、有効でない UTF-8、改行、`=`、制御文字を含む key
- `#`、`;`、`[` で始まり INI のコメントやセクションと衝突する key
- 先頭または末尾に ASCII space (U+0020) を持つ key
- 同じ batch 内の重複 key

`LoadFromBytes` は `=` より前の key について先頭と末尾の ASCII space と tab を
取り除きます。tab は制御文字として拒否し、ASCII space は境界にある場合だけ拒否するため、
batch から追加する key は Save/Load 後も同じ raw identity を維持します。
`"foo bar"` のような内部の ASCII space は許可し、Save/Load 後もその位置を維持します。

既存互換の単体 setter は境界 space/tab を持つ key を保持できるため、非0件 batch のたびに
既存全 key を `LoadFromBytes` と同じ境界 trim で allocation 前に比較します。正規化した
既存 key 同士が同じになる場合と、byte 列が異なる既存 key と batch key が同じになる場合は
エラーにします。たとえば既存 `"foo "` と batch `"foo"`、既存 `"foo"` と `"\tfoo "` が
ある状態への無関係な batch は拒否します。既存 `"foo"` と batch `"foo"` のように byte 列も
同じ場合は衝突ではなく、通常の update または no-op です。衝突しない legacy key はこの検査
だけを理由には拒否も正規化もしません。たとえば既存に `"foo "` だけがあり、batch が
`"bar"` を追加する場合は成功します。その後の Save/Load では loader の従来契約により
`"foo "` は `"foo"` へ変わります。この API が保証するのは trim 後の重複を作らず
`LoadFromBytes` の duplicate key エラーを防ぐことであり、legacy key の raw identity 維持
ではありません。

この identity 検査は no-op 判定と一時領域の確保より前に完了します。拒否時は allocator へ
確保要求を出さず、件数、全値、呼び出し前に借用した全ポインタを維持します。
loader-trim identity 検査では、4,096 件を超える既存ストアを走査前に拒否します。
既存件数 `n`、batch 件数 `m` が各4,096以下のとき、既存同士を `n(n-1)/2` 回、
既存と batch を `n*m` 回、最大25,163,776組比較します。これはAPI全体の計算量ではなく、
各組の byte 比較量には key 長も影響します。検査用の追加領域は `O(1)` です。

値は終端までの byte 列として読み取り、周囲の空白を含めてそのまま保持します。

`FStorageStringBatchEntry` は `key` を offset 0、`value` を一 pointer 分の offset に置く
二 pointer サイズ・pointer alignment の aggregate です。standard-layout かつ
trivially-copyable であることを公開 header の `static_assert` で固定します。

## 原子的な反映と allocator

入力と既存 key の正規化衝突検査後、すべての入力 key/value を対象 `FStorage` と同じ
allocator へ所有退避します。
そのため、入力ポインタが `GetString` から借用した内部値を指す場合も扱えます。
続いて既存状態を同じ allocator の候補へ複製し、最終件数を予約してから変更を適用します。
完成した候補だけを一度の move で反映します。

入力所有化、候補複製、容量予約のいずれかが失敗した場合はエラーを返し、対象の件数、
値、呼び出し前に `GetString` から借用したポインタを維持します。一時配列と文字列も
すべて対象と同じ allocator から確保します。

実変更を含む成功では、既存項目も複製した候補全体へ置き換えるため、呼び出し前に
`GetString` から借用した全ポインタが無効になります。全項目が no-op の `Ok(0)` と
失敗では候補を反映せず、借用ポインタを同じアドレスで維持します。

## Kit からの吸収範囲

Kit Persistence のうち、インスタンス所有の文字列 upsert を一括反映する最小責務だけを
`FStorage` へ統合しています。既存の single / atomic I/O と string upsert batch は統合済みです。
transactional remove、typed codec、owned snapshot、document export/summary は未統合であり、
process-global storage、subsystem、mutex、thread-local 状態も追加していません。
