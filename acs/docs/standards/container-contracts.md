# ACS Container 契約

`Container` は、ACS の所有型、非所有 view、hash、JSON 値を提供する Runtime
module である。公開依存は `Foundation` と `Memory` に限り、各 container は利用側が
値として所有する。共有 owner、frame 更新、終了順序を持つ service ではないため、
この module 自体を subsystem として登録しない。

公開signatureの索引と短い例は[機能・APIリファレンス](../reference/index.html)から参照する。
本書は所有権、allocator、失敗、storage 再利用、参照無効化の正規契約を扱う。

## 責務と source 境界

| 責務 | 公開型 | 正規 source |
|---|---|---|
| 連続した動的要素の所有 | `TArray<T>` | [`Array.h`](../../src/container/Array.h) |
| 少数要素の直接保持と動的領域への移行 | `TInlineArray<T, InlineCapacity>` | [`InlineArray.h`](../../src/container/InlineArray.h) |
| 連続領域の非所有参照 | `TSpan<T>` | [`Span.h`](../../src/container/Span.h) |
| UTF-8 文字列の所有 | `FString` | [`String.h`](../../src/container/String.h)、[`String.cpp`](../../src/container/String.cpp) |
| UTF-8 バイト列の非所有参照 | `FStringView` | [`StringView.h`](../../src/container/StringView.h) |
| key と value の所有検索表 | `THashMap<K, V, H>` | [`HashMap.h`](../../src/container/HashMap.h) |
| hash 値の生成 | `THasher<T>`、`HashBytes`、`HashMix64` | [`Hash.h`](../../src/container/Hash.h) |
| 文字列検索 key と hash の再利用 | `FStableStringKey`、`FStringHasher` | [`StableStringKey.h`](../../src/container/StableStringKey.h)、[`StringHasher.h`](../../src/container/StringHasher.h) |
| JSON 値の所有、parse、write | `FJsonValue`、`ParseJson`、`TryWriteJson` | [`Json.h`](../../src/container/Json.h)、[`Json.cpp`](../../src/container/Json.cpp) |

template container の実装は同名 header に置く。`FString` と `FJsonValue` の非 template
実装は同名 cpp に置き、他 module の owner や更新処理を `container` folder へ持ち込まない。

## 所有型と非所有 view

`TArray`、`TInlineArray`、`FString`、`THashMap`、`FJsonValue` は保持値を所有する。
move 後は確保済み領域と allocator の参照が移動先へ移り、move 元は破棄可能な空状態に
なる。`TArray` と `THashMap` は copy を禁止する。`TArray` の複製は `Clone()` で明示する。
`FJsonValue` も子の所有権を持つため copy を禁止する。

`TSpan`、`FStringView`、`FStableStringKey::View`、`FJsonValue::AsString()`、
`FJsonValue::MemberKey()` は領域を所有しない。view の利用側は、参照先 owner の寿命と
mutation 規則を守る。空 view は `nullptr` と size 0 を取り得るため、空状態で終端 pointer
を算術評価しない。

## allocator と寿命

所有型へ渡した `IAllocator` は container に所有されない。allocator は container の破棄、
move 先の破棄、または storage の明示解放が終わるまで有効でなければならない。省略時は
`DefaultAllocator()` を使う。

- `TArray::GetAllocator()` と `FString::GetAllocator()` は現在の確保元を返す。
- `TInlineArray` は直接領域を超えた時だけ、構築時の allocator から動的領域を確保する。
- `THashMap` は bucket と密な value 配列の両方を同じ allocator から確保する。
- allocator 付き `ParseJson` は root、子、key、文字列を指定 allocator で構築する。
- `TryWriteJson` は `Output.GetAllocator()` で一時文字列を構築し、成功時だけ `Output` へ移す。

## storage の保持と解放

内容だけを破棄する操作と、確保済み領域を返す操作を分ける。

| 型 | 容量を保持する操作 | 容量を解放する操作 |
|---|---|---|
| `TArray` | `Reset()` | `Empty()` |
| `TInlineArray` | `Reset()`。動的領域へ移行済みなら、その領域を保持する | 破棄時に解放する |
| `FString` | `Clear()` | `ReleaseStorage()` |
| `THashMap` | `Reset()` | `Empty()` |
| `FJsonValue` | 種別変更時に子 container の容量を保持する | 値の破棄時に解放する |

容量解放後も container に設定された allocator は維持する。周期的に同じ上限まで増える
一時 collection は保持操作を使い、長寿命 owner が大きな一時 storage を手放す時だけ
解放操作を使う。

## checked API と失敗を返す API

`Add`、`Emplace`、`Reserve`、`SetNum`、`Append` などの checked API は、呼び出し側が
容量確保の成功を前提にできる経路で使う。確保失敗や size overflow は `ACS_CHECKF` で
契約違反として検出する。

外部入力、任意容量、回復可能な runtime 経路では対応する `Try` API を使う。

| API | 失敗の通知 | 失敗時の契約 |
|---|---|---|
| `TArray::TryReserve`、`TrySetNum`、`TryAdd` | `false` | 要素、size、既存領域を維持する |
| `TArray::TryEmplace` | `nullptr` | 要素、size、既存領域を維持する |
| `TInlineArray::TryAdd` | `false` | 動的領域への移行に失敗した場合、直接領域の要素を維持する |
| `FString::TryReserve`、`TryAppend` | `false` | 内容、size、既存領域を維持する |
| `THashMap::TryReserve` | `false` | key、value、bucket を維持する |
| `THashMap::TryAdd` | `false` | key と value の集合を維持する。先行した bucket 拡張は保持される場合がある |
| `TSpan::TrySubSpan` | `false` | 出力 view を変更しない |
| `ParseJson` | error を持つ `TResult` | root 値を返さず、構文、深さ、終端、数値、escape、size の subcode を返す |
| `TryWriteJson` | `false` | `Output` を変更しない |

現在の `ParseJson` は DOM 構築中の allocator 枯渇を `TResult` の subcode として返さない。
allocator の容量を保証できない経路では、入力上限と owner の memory budget を先に検証する。

## 配列と view

`TArray` は要素を連続領域へ保持する。`Reserve` は容量だけを増やし、`SetNum` は要素の
構築または破棄を伴う。`Remove` は順序を保ち、`RemoveAtSwap` は末尾要素を削除位置へ移して
順序を保たない。`Shrink` は容量を現在 size まで縮めるが、再確保失敗を通知しない。失敗時は
要素と現在容量を維持する。`TInlineArray` は `InlineCapacity` までは本体内に保持し、超過時に
一度だけ動的領域へ移行する。移行後は `Reset()` しても直接領域へ戻らない。

`TSpan::SubSpan` は範囲が正しいことを前提とする checked API である。外部 offset や count
には `TrySubSpan` を使う。`TSpan` の変換は ownership を移さない。

## 文字列と文字列 view

`FString` は NUL 終端を維持し、`Size()` は終端を含まない UTF-8 バイト数、`Capacity()` は
終端を除く格納可能バイト数を返す。22 バイト以下は本体内に保持する。`Append` と
`TryAppend` は自分自身を指す `FStringView` も扱う。

`FStringView` の比較、検索、接頭辞・接尾辞判定は byte 単位で行う。view 自体は NUL 終端を
保証しないため、`Data()` を C 文字列として渡す場合は呼び出し側が終端を保証する。
`FString::Clear()` 後は storage address が同じでも、以前の内容を表す view は再利用しない。

## hash と map

`THashMap` は探索距離と 8-bit 指紋を持つ bucket 配列と、`TPair<K, V>` を連続保持する
value 配列を分ける。`Add` と `TryAdd` は既存 key の value を上書きする。`Remove` は bucket
列を詰め、value 配列では末尾要素を削除位置へ移す。

`Find` が返す pointer と range-for の参照は map が所有する value 配列を指す。新しい key の
追加は value 配列を再配置する場合があり、全 pointer、参照、iterator が無効になり得る。
`Remove` は削除要素に加え、末尾から移動した要素の address を変える。`Reset`、`Empty`、move、
破棄は既存参照を無効にする。`Reserve` による bucket だけの再構築は value address を変えない。

`FStableStringKey` は文字列 view と事前計算済み hash を保持する。元文字列の storage が無効に
なった後は key を検索へ使わない。

## JSON

`FJsonValue` は `Null`、`Bool`、`Number`、`String`、`Array`、`Object` のいずれかを所有する。
型不一致の accessor は既定値、空 view、または静的 Null 値を返す。`At`、`Get`、`Find`、
`MemberKey` が返す参照や view は DOM の mutation、move、破棄で無効になる。

`ParseJson` は既定で入力を 64 MiB、入れ子を 256 段まで受け入れる。失敗時の
`FErrorCode::message` は呼び出した thread に属し、同じ thread で次に `ParseJson` を呼ぶまで
有効である。別 thread の parse はこの message を上書きしない。

`TryWriteJson` は制御文字と埋め込み NUL を JSON escape へ変換する。非有限 number、深さ超過、
出力 byte 上限超過、確保失敗では `false` を返し、既存 `Output` を維持する。成功時は完全な
UTF-8 JSON だけを `Output` へ反映する。

## thread 境界

container object 自体は内部 lock を持たない。同じ object に mutation が含まれる並行 access は
利用側が `FMutex` または `FRwLock` で同期する。公開後に storage と内容を変更しない object は、
参照先と allocator の寿命を固定した上で複数 reader から参照できる。

## 検証

| 契約 | test |
|---|---|
| 配列、直接保持配列、文字列、hash map の基本動作と確保失敗 | [`container_tests.cpp`](../../tests/container_tests.cpp) |
| `TSpan::TrySubSpan` の出力維持 | [`input_recording_view_tests.cpp`](../../tests/input_recording_view_tests.cpp) |
| JSON parse 上限、allocator 伝播、write のtransaction | [`json_tests.cpp`](../../tests/json_tests.cpp) |

変更時は `acs_unit_tests` に加え、次の ACS gate を実行する。

```powershell
python acs/scripts/audit_cpp_conventions.py --root acs
python acs/scripts/audit_reference_type_names.py --root acs
python acs/scripts/audit_module_sources.py --root acs
python acs/scripts/amalgamate.py --check
```
