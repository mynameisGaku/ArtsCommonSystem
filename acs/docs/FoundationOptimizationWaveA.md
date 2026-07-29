# Foundation Optimization Wave A

## 目的

Foundation Optimization Wave A は、描画・エディタ・ゲームプレイの上位機能が共通して使う
Container、Memory、Math の基盤経路を高速化し、同時に失敗時の状態保証を明文化する変更である。
対象は T01–T05 と T21–T25 の十項目に限定し、既存 API の互換経路を残したまま opt-in
（利用側が明示的に選択する方式）の高速経路を追加した。

性能時間は 2026-07-30 に Windows x64、MSVC 19.51、同一 Release 構成で取得した一回の
参考値である。OS のスケジューリング、CPU 周波数、キャッシュ状態の影響を受けるため、時間の
増減は合否条件ではない。合否は API 契約テスト、割り当て・hash・lock の決定的カウンタ、
Release/Debug の全単体テスト、モジュール生成検査、および配布生成で判定する。

## タスク対応表

| ID | 目的と実装 | 期待効果 | 主な依存 | 検証方法 |
|---|---|---|---|---|
| T01 | `TArray` の成長を `Realloc` 優先にし、自己参照 append を staged copy（確定前の退避）で保護 | 成長時の alloc/copy/free を削減 | T21、`FAllocator::Realloc` | 基本型・非基本型・自己参照・OOM rollback テスト、alloc/realloc counter |
| T02 | `THashMap::TryInsert` で既存 key の更新を成長判定より先に行い、hash を一回だけ計算 | 更新時の不要な確保と再 hash を除去 | `HashBytes`、allocator | 更新・衝突・OOM・overflow テスト、update alloc/hash counter |
| T03 | `FString` 成長の `Realloc` 化と alias 安全な append、`FStringView` の byte 比較・検索 | append と検索の割り当て・走査コストを削減 | Memory allocator | SSO/heap、自己 append、OOM、境界・検索テスト、allocation counter |
| T04 | JSON の非 escape 文字列を run 単位で処理し、writer を transaction 化 | parse/write の一文字単位処理を削減し、失敗時出力を保全 | T03、T23 | parse/write round-trip、OOM、上限、非有限数、出力不変テスト |
| T05 | `FPoolAllocator` に `AllocBatch` / `FreeBatch` を追加 | 複数 block 操作の lock 取得回数を削減 | pool lock、T24 | 部分成功・不正 pointer・重複 free・並列テスト、lock counter |
| T21 | `TIsTriviallyRelocatable<T>` opt-in trait と byte relocation 経路を追加 | 適格型の move/destroy loop を byte 移動へ置換 | T01 | compile-time trait、lifetime、非 opt-in 型の fallback テスト |
| T22 | constexpr runtime 同値 hash、`FStableStringKey`、`FStringHasher`、`FindByHash` を追加 | literal key の再 hash と一時 `FString` を回避 | T02、T03 | runtime/constexpr 同値、衝突安全性、異種検索、null 入力テスト |
| T23 | JSON byte 分類と hex nibble を 256-entry constexpr table 化 | parse 分岐を固定 table lookup に集約 | T04 | 全 byte 分類、escape/unicode、invalid input テスト |
| T24 | `TTypedPoolAllocator<T, N>` を追加 | 型サイズ・alignment を compile time に固定した pool 利用 | T05、`TIsTriviallyRelocatable` | construction/destruction、capacity、duplicate destroy、alignment テスト |
| T25 | `EBatchTransformPolicy` と `TransformBatchStatic` を追加し runtime wrapper を共通実装へ委譲 | policy 既知の呼び出しで分岐除去・inline 展開を可能にする | Math `FVec3` / `FMat4` | Point/Vector の runtime/static 同値、配列 overload、compile-time policy 検査 |

## 公開 API と配置

### Container

- `src/foundation/TypeTraits.h`
  - `TIsTriviallyRelocatable<T>` と `IsTriviallyRelocatableV<T>`。
- `src/container/ConstexprHash.h`
  - `HashBytesConstexpr` と `HashLiteral`。大きい constexpr 実装を汎用 `Hash.h` から分離し、
    通常の hash 利用側の compile footprint を抑える。
- `src/container/StableStringKey.h`
  - `FStableStringKey` と `MakeStableStringKey`。
- `src/container/StringHasher.h`
  - `FString` と `FStringView` を同一規則で hash する明示 hasher `FStringHasher`。
- `src/container/HashMap.h`
  - `Find(const Q&)`、`Contains(const Q&)` の異種検索と
    `FindByHash(const Q&, u64)` の prehashed（呼び出し側で hash 済み）検索。
- `src/container/Json.h`
  - allocator 指定 `ParseJson`、`kMaxJsonInputBytes`、`TryWriteJson`。

`src/container/Module.cmake` は `ConstexprHash.h`、`StableStringKey.h`、
`StringHasher.h` を公開 header として列挙する。

### Memory

- `src/memory/PoolAllocator.h`
  - `AllocBatch`、`FreeBatch`、計測用 `LockAcquisitionCount`。
- `src/memory/TypedPoolAllocator.h`
  - `TTypedPoolAllocator<T, BlockCount>`。

`src/memory/Module.cmake` は `TypedPoolAllocator.h` を公開 header として列挙する。

### Math

- `src/math/BatchTransformPolicy.h`
  - `EBatchTransformPolicy::{Point, Vector}` と `TransformBatchStatic`。
- `src/math/MathDispatch.cpp`
  - 既存 runtime entry point は static policy 実装へ委譲する。

`src/math/Module.cmake` は `BatchTransformPolicy.h` を公開 header として列挙する。

### Test / benchmark

- `tests/foundation_optimization_bench.cpp`
  - 十項目の時間と決定的 counter を同じ入力で出力する。
- `tests/CMakeLists.txt`
  - Release benchmark target `acs_foundation_optimization_bench` を登録する。
- `tests/container_tests.cpp`、`json_tests.cpp`、`memory_tests.cpp`、`math_tests.cpp`
  - 正常系だけでなく OOM、overflow、衝突、部分成功、lifetime を検証する。

## 失敗時契約

### Array / String

- size/capacity の加算・倍増が `usize` を超える要求は失敗し、既存要素を保持する。
- `TArray` の自己参照 append は、成長前に値を退避してから確保を試みる。確保失敗時は
  size と既存要素を変更しない。
- `FString` の自己 append と内部範囲 append は alias を offset として追跡し、移動後に
  再解決する。確保失敗時は元の文字列を保持する。

### Hash / HashMap

- `HashBytes(nullptr, 0)` と `HashBytesConstexpr(nullptr, 0)` は空入力の hash を返す。
  `nullptr` かつ長さが非ゼロの無効入力は `0` を返し、メモリを参照しない。
- `FindByHash(Query, Hash)` の `Hash` は、その map に指定した hasher が `Query` に対して
  返す値と一致しなければならない。hash 一致後も key 比較を行うため、衝突で誤一致しない。
- stable string key を使う map は
  `THashMap<FString, V, FStringHasher>` とし、
  `Map.FindByHash(Key.View, Key.Hash)` で検索する。generic map に stable-key 専用 overload は
  設けない。
- 新規 `TryInsert` は bucket rehash 成功後に value 確保が失敗する場合がある。この場合、
  既存 key/value と検索可能性は保持され、新規 key は追加されない。ただし内部 capacity は
  増加済みのまま残ることがある。capacity 不変は保証しない。
- 既存 key の更新では成長用確保を行わず、hash を一回だけ計算する。

### JSON

- parser の入力上限は 64 MiB、入れ子上限は 256。上限超過は error を返し、範囲外を
  参照しない。
- allocator 指定 parse は root、文字列、key、子 value まで同じ allocator を伝播する。
- `TryWriteJson` は staging buffer に全体を書き、成功時だけ出力へ move する。深さ上限、
  byte 上限、OOM、不正値で失敗した場合、呼び出し側の出力文字列は変更しない。
- parser は既存互換性のため、数値変換が overflow して非有限値になっても number として
  受理する。writer は JSON に表現できない NaN と infinity を拒否する。

### Pool

- `AllocBatch(Output, Count)` は取得できた件数を返す。capacity 不足時は prefix のみ成功し、
  残りの出力 slot を `nullptr` にする。
- `FreeBatch(Pointers, Count)` は有効かつ現在 allocation 中の block だけを一度解放し、
  解放件数を返す。pool 外 pointer、misaligned pointer、null、同一 batch 内の重複は
  解放件数に含めない。
- `TTypedPoolAllocator::Create` が失敗した場合は constructor を呼ばない。
  `Destroy` は生存中 object だけを破棄し、重複呼び出しは `false` を返す。
- typed pool の所有者は allocator の破棄前に全 object を `Destroy` または `Deallocate`
  しなければならない。Debug destructor は live allocation がゼロであることを assert する。

### Static math

- `EBatchTransformPolicy::Point` は平行移動を含み、`Vector` は平行移動を含まない。
- 未対応 policy は template instantiation 時の `static_assert` で拒否する。
- 入力と出力が null でなく、`Count` 個の `FVec3` を参照可能であることは呼び出し側の責務。

## ベンチマーク

### 方法

次の target を同じ Release 構成で build し、一つの process 内で task ごとの固定反復を実行した。

```powershell
cmake --build --preset dx12-release --target acs_foundation_optimization_bench --parallel 8
.\acs_foundation_optimization_bench.exe
```

baseline と Wave A は同一マシン、Windows x64、MSVC 19.51 で取得した。値は nanosecond。
T21 と T23 は compile-time/table 状態、T02 と T05 は確保・hash・lock counter を主要判定にする。

### 結果

| ID | baseline | Wave A | 差分 / 決定的結果 |
|---|---:|---:|---|
| T01 Array push | 34,935,700 | 28,367,500 | -18.80%; alloc 23→1、realloc 0→22、free 23→1 |
| T21 relocation | 未対応 | `relocatable_u32=1` | `u32` byte relocation 経路を compile-time 選択 |
| T02 HashMap find | 8,837,000 | 9,778,700 | +10.66% の単発揺れ; update alloc 1→0、update hash 13→1 |
| T22 dynamic lookup | 未計測 | 86,295,400 | 比較用 |
| T22 stable lookup | 未対応 | 50,861,000 | dynamic 比 -41.06%; literal hash は runtime と一致 |
| T03 String find | 500,695,300 | 267,562,500 | -46.56%; append alloc 17→1、realloc 0→16 |
| T04 JSON parse | 48,607,700 | 20,516,400 | -57.79%; 11,787 bytes ×100 |
| T04 JSON write | 未対応 | 10,653,400 | 1,178,700 bytes を transaction write |
| T23 classifier | 未対応 | 256 entries | 全 byte を constexpr table 化 |
| T05 pool individual | 80,293,100 | 85,102,400 | +5.99% の単発揺れ; 4,096,000 operations |
| T05 pool batch | 未対応 | 11,431,300 | individual 比 -86.57%; lock 4,096,000→1,000 (-99.98%) |
| T24 typed pool | 未対応 | 68,823,900 | block size 8、block count 4,096 |
| T25 runtime math | 1,335,854,900 | 1,319,599,400 | -1.22%; 20,480,000 points |
| T25 static math | 未対応 | 1,337,675,000 | runtime 比 +1.37% の単発揺れ |

T25 の static policy は devirtualization（実行時分岐を compile-time 選択へ変えること）と
拡張性の API であり、この単発測定では速度向上を確認していない。runtime wrapper と static
経路の結果一致を契約テストで保証し、性能上の主張は追加の統計測定まで行わない。

## Build・binary・配布物

| 項目 | baseline | Wave A | 備考 |
|---|---:|---:|---|
| configure | 49.6037 s | 52.2216 s | +5.28%; cold directory、参考値 |
| clean `acs_unit_tests` target build | 17.8235 s | 39.0918 s | +119.33%; filesystem/cache の差を含む一回値、非 gate |
| `acs_unit_tests.exe` | 10,692,096 B | 5,713,920 B | Release layout |
| `acs_container.lib` | 103,264 B | 102,168 B | -1.06% |
| `acs_memory.lib` | 560,782 B | 486,540 B | -13.24% |
| `acs_math.lib` | 70,778 B | 45,960 B | -35.07% |

clean build 時間は悪化した一回値をそのまま記録する。巨大 constexpr hash と `FStringHasher` は
それぞれ `ConstexprHash.h` / `StringHasher.h` に分離し、汎用 `String.h` と `Hash.h` から
不要な template 展開を除いたが、統計的な compile-time 改善はまだ立証していない。

Release package は次で生成した。

```powershell
cmake --build engine/cmake-build-release-wave-a --target package --parallel 8
```

- artifact: `engine/cmake-build-release-wave-a/ACS-0.1.0-win64.zip`
- size: 846,260 bytes
- SHA-256: `2E1BD9D7D50E130238B265C80AC471B1C269C5519CDB570AB3B78EF00C0A79A2`
- 内容確認: `hello_easy/hello_easy.exe`、mimalloc header/library/CMake metadata を格納。

## 検証結果

2026-07-30 に以下を実行した。

| Gate | 結果 |
|---|---|
| Release `acs_unit_tests` | 1,108 passed、0 failed |
| Debug `acs_unit_tests` | 1,112 passed、0 failed |
| `acs_foundation_optimization_bench` | T01–T05 / T21–T25 の全行と `sink=493373514` を出力 |
| `dotnet run --project tools/acsbuild -- --check` | 28 modules、`acsbuild: OK` |
| Release CPack `package` | ZIP 生成成功、内容一覧と SHA-256 を確認 |
| changed C++ rule audit | changed file、header guard、comment、layout 規則を検査 |
| `git diff --check` | whitespace error なし |

## Rollout と残留注意

- `TIsTriviallyRelocatable` の opt-in は強い契約である。特殊化する型は byte relocation 後に
  元 storage を destructor なしで破棄でき、移動先で通常どおり destructor を呼べなければ
  ならない。判断できない型は特殊化せず既存 move/destroy 経路を使う。
- stable key の hash は map hasher と同じ規則で作る。文字列 map では明示的に
  `FStringHasher` を指定する。
- `THashMap::TryInsert` の OOM 後は既存内容と検索は有効だが、capacity が増えている可能性を
  許容する。
- JSON の 64 MiB / depth 256 上限を超える資産は streaming parser など別経路が必要である。
- typed pool は ownership を自動回収しない。所有者が生存 object を明示破棄する。
- benchmark の時間値と clean build 時間は一回測定であり、回帰 gate に採用しない。
  CI gate 化する場合は複数回の中央値、同一電源設定、warm-up、外れ値基準を先に定義する。
