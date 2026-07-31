# Foundation Optimization Wave K

## 目的と採用基準

Wave K は、アセット内容・`.acpak` の on-disk format・CRC/GCM/LZ4 の検証順を
変えずに、アセット path の所有、immutable package の読み取り、展開 scratch、
Scene3D の依存解決を既存 owner の中で効率化する変更である。別系統の VFS、
generic async I/O scheduler、DDC service、profiler は作らない。後続の Editor
実装で明確な DDC owner が追加されたため、T57 の追補ではその owner 内だけに
path pool を置く。

採用判断は次の四点で行った。

- 目的: 固定長 async path、large package の毎回 file read と中間確保、展開用
  allocation、重複 Scene dependency read、batch ごとの lifecycle lock を減らす。
- 効果: byte parity、物理 read/job 数、保持 code unit、batch entry 数、scratch
  reuse/fallback、dependency read 数で決定的に観測できること。
- 依存: `Asset`、`AssetPack`、`GameFramework` の既存 owner、allocator、lock、
  `TResult` だけを利用すること。
- 検証可能性: Release/Debug の専用テスト、全 unit、atomic replace、CRC-before-
  commit、並行 read、規約・module・single-header 監査で再現できること。

## 採用範囲

| ID | 状態 | 実装と判断 |
|---|---|---|
| T53 | 既存 owner 内で完了 | `FAssetRegistry::m_InFlight` が同じ `FAssetId` の `LoadAsync` を一つの job/physical read に合流する既存機構を診断値と shutdown stress で固定した。generic file-read coalescer は追加していない。 |
| T54 | 完了 | 256 KiB 以上の immutable `.acpak` snapshot に read-only file mapping を試し、raw/compressed/encrypted pipeline の入力に使う。mapping 失敗時は従来の `ReadFile` へ戻る。 |
| T55 | 完了 | reader 所有の stored/final scratch を各 16 MiB まで保持する。`TryLock` 競合または上限超過時は呼び出し局所 buffer へ戻り、read を直列化しない。 |
| T56 | 完了 | Scene3D の mesh/material dependency を kind+完全 path で重複排除し、初出順に最大 8 entry、合計 32 MiB を目安とする batch で読む。32 MiB を超える単一依存は進行保証のため一件だけで読む。現在の外部依存は相互依存を持たない leaf なので、初出順が安定した topological order になる。 |
| T57 | 完了 | 新規 async job の完全 path は `FAssetPathInterner`、Cook/thumbnail DDC の canonical entry path は owner 固有 `DerivedDataCachePathPool` で共有する。Asset と DDC の異なる寿命は混在させない。 |
| T58 | stream batch 完了 | low-level reader と `IAssetPackReader` に stable-order `ReadFiles` を追加した。既存 backend は既定の逐次実装で source compatibility を維持する。format version と archive bytes は変更しない。 |
| T63 | 保留 | editor profiler/RHI は単一 writer で、既存 snapshot は一回の copy である。競合改善の再現値がないため sharding しない。package 診断も単一 relaxed atomic 群とし、reader 常駐 layout を肥大化させない。 |

## 所有権と安全性

### Async path

旧 async job は path 長に関係なく 1,024 `wchar_t` を job ごとに予約していた。
新規 job は `L + 1` code unit の immutable path を共有参照する。初回は path object/
control block の allocation が増えるため、初回 latency 改善とは扱わない。同じ path を
unload 後に再び job 化した場合に初めて path allocation と所有 copy を再利用できる。

interner は最大 256 path、NUL 込み 65,536 code unit を保持する。追い出せるのは pool
以外に参照がない要素だけである。全要素が使用中なら、新規 path は pool に保持せず
呼び出し側だけが所有する bypass へ戻る。したがって上限到達によって使用中 job の
address/lifetime は変わらない。`Intern`、job allocation、submit の失敗経路は
`m_InFlight`、active operation、future counter を必ず完了状態へ戻す。

### Mapped package snapshot

mapping は manifest の前後で同じ file identity、size、last-write time を確認した
同一 read handle から作る。handle は write sharing を許可せず delete sharing だけを
許可するため、atomic replace 後も既存 reader は旧 snapshot、新規 reader は新 snapshot
を読む。`Close` は view、mapping handle、file handle の順で閉じる。mapping 作成失敗は
archive open の失敗ではなく、buffered I/O fallback になる。

raw mapped read は mapped bytes の CRC が成功した後だけ caller buffer へ copy する。
暗号化・圧縮経路も final scratch 上で GCM/LZ4/CRC を完了してから caller buffer へ
commit する。エラー時に caller buffer を部分更新しない。

### Scratch と batch

保持 scratch は一回の read だけが借りる。競合時に lock 待ちを増やさず局所 scratch を
使うため、同じ reader の並行 read を維持する。`Close` は lifecycle exclusive lock で
全 read 完了後に mapping と scratch を解放する。

`ReadFiles` は entry 順に commit する API で、all-or-nothing ではない。後続 entry が
失敗しても先行出力は更新済みであり、任意の `CompletedCount` に成功済み件数を返す。
Scene3D は公開 Scene ではなく private parsed document/buffer に読む。batch 後半が
失敗した場合も completed entry を初出順に decode し、旧逐次経路と同じ先行 decode
error を優先してから、最終 commit 前に失敗を返す。

診断 snapshot/reset は lifecycle exclusive lock で read 完了境界を作る。snapshot は
完了済み read だけを集約し、reset 前後の increment を混在させない。

## 保留・追補判断

### Generic async I/O coalescer

現時点で file request の cancellation token、owner、callback lifetime、priority を
一括管理する storage abstraction がない。sync `Load` を async job に合流させると、
loader callback から同じ path を再入した場合に自己 wait となる。所有者不在の汎用
table を新設しても期待効果を測れず lifetime だけが悪化するため保留した。

再検討条件は、storage 層に cancellation と request owner を持つ非同期 API が入り、
同一 `(snapshot, offset, size, transform)` の要求頻度と物理 I/O 削減を計測できること。

### DDC path interning

初回 Wave K 時点では DDC owner と eviction policy がなく保留した。その後、
Editor に package operation 単位の `DerivedDataCache` と、Asset Browser session
単位の `ThumbnailDerivedDataCache` が追加されたため、T57 の再検討条件を満たした。

両 owner は共通の `DerivedDataCachePathPool` を一つずつ所有する。pool は小文字
SHA-256 key だけを Release でも検証し、prefix directory と entry path を初回だけ
構築する。最大512 key、key/directory/entry path 合計256 Ki code unitをLRUで保持し、
上限を超える単一pathは保持せず有効な結果だけを返す。返却済みstringは不変なmanaged
objectなので、evictionまたはReset後も呼び出し側の参照寿命を短縮しない。

Thumbnail DDC は既存のcache gate、Cook DDC は専用path gateでpool操作と診断snapshotを
直列化する。Asset側の`FAssetPathInterner`は非同期job/registry lifetimeのままであり、
Editor DDCへ流用しない。static pipelineでowner境界を持たないprocessed-import DDCも
このpoolへ入れず、所有者不明のglobal cacheを新設しない。

`DerivedDataCachePathPoolDiagnostics` はrequest、hit、miss、eviction、bypass、現在保持
path数/code unitを一括取得する。canonical key生成そのもの、payload、disk cacheの
eviction規則、path/reparse検証順は変更しない。

### Profiler sharding

editor profiler は owner thread が frame snapshot を publish し、consumer はまとまった
snapshot を読む。RHI command statistics も単一 owner である。8 diagnostic shard を
試作した時点の `FAcpakReader` は 768 bytes / align 64 まで増えたが、parallel throughput
改善を再現できなかったため撤去した。単一 atomic 群では 304 bytes / align 8 である。

2026-07-30 の再監査では、editor profiler snapshot は256 bytes、cloud workload
snapshot は168 bytesだった。Profiler panel は表示中100 ms、非表示中500 ms、
interaction health は500 ms間隔で、いずれも描画と同じWPF dispatcher上から読む。
表示中の上限でも profiler 12 copy/s と cloud 10 copy/s、合計4,752 bytes/sであり、
producer間で共有するcounterはない。二つのABI呼び出しをbatch化しても削減できるのは
10 call/sだけで、描画hot pathの作業量や競合は減らない。

複数 producer の cache-line contention が profiler trace で支配的と確認された場合だけ
T63 を再検討する。

## 公開 API と Win64 layout

公開補助型は一主要型一 header に分け、owner header から include する。

| 型 | 用途 | `sizeof` / `alignof` (Win64 Release) |
|---|---|---:|
| `FAssetPathInternerDiagnostics` | path pool の hit/miss/保持量 | 56 / 8 |
| `FAssetRegistryDiagnostics` | async 合流/job/read/cache | 96 / 8 |
| `FAcpakReadDiagnostics` | mapped/buffered/scratch/batch | 80 / 8 |
| `FAssetPackReadRequest` | UTF-8 batch read の一要素 | 24 / 8 |
| `FAssetPathInterner` | bounded path owner | 136 / 8 |
| `FAcpakReader` | package reader owner | 304 / 8 |

`IAssetPackReader::ReadFiles` は pure virtual にせず、既存 `ReadFile` を順番に呼ぶ既定実装を
持つ。既存 backend/mocks は override なしで従来結果と部分完了順を維持できる。virtual
table は拡張されるため、バイナリ互換性はなく、利用 module は同じ revision で再 build
する必要がある。

## 決定的な比較と参考計測

| 観測項目 | 変更前 | 変更後 |
|---|---:|---:|
| 同一 path の同時 `LoadAsync` 2件 | job/read 1、path 固定領域 1 | job/read 1、interner request 1、coalesced 1 |
| async job の path 予約 | 常に 1,024 `wchar_t` | 新規 path は `L+1`、再 job は同じ immutable path |
| 同じ DDC key の path 構築 16,384 回 | directory/entry path を16,384回再構築 | 構築1、共有16,383、path loop allocation 1,152 bytes |
| 同じ DDC key の path loop allocation | 16,252,968 bytes | 1,152 bytes |
| raw mapped entry の per-read temporary | final `TArray` + file read | 0 temporary、CRC 後 1 caller copy |
| scene の同一 material 2参照 | size/read/decode 2回 | size/read/decode 1回、`DependenciesLoaded=2` |
| compressed 2 MiB、warmup後4並行 | per-read temporary | retained 2,734,843 bytes、reuse 1、競合 fallback 3 |
| `FAcpakReader` 試作8 shard | 768 bytes / align 64 | 単一診断群 304 bytes / align 8 |

Release の mapped raw 2 entry（合計 327,680 bytes）を24反復した同一 process の参考値は、
sequential `12,070 us`、batch `12,055 us` だった。差は小さく、速度向上の断定には
使わない。採用根拠は一 batch 一 lifecycle lock、byte/CRC parity、部分完了数、
重複 dependency read の決定的削減である。

## 検証

専用 target/CTest は `acs_foundation_optimization_wave_k_tests` /
`ACS.FoundationOptimizationWaveK`。次を検証する。

- interner の hit、256 entry/65,536 unit 上限、全 pin 時 bypass、reset 後の参照寿命。
- DDC path pool のhit、512 entry/256 Ki code unit上限、LRU eviction、oversize bypass、
  Reset後のstring寿命、Releaseでの非canonical key拒否。
- Cook DDC の同一key hitとThumbnail DDCのmiss/store/hitがowner固有poolを共有すること。
- cache/in-flight hit が interner を通らず、新規/repeat job だけが miss/hit になること。
- async job 中の shutdown wait と path/future lifetime。
- large raw package の mapped batch、atomic replace 前後の旧/新 snapshot。
- CRC 失敗時の caller buffer 不変、batch の `CompletedCount` と部分 commit。
- compressed read の保持 scratch、4並行 byte parity、reuse/fallback、決定的 reset。
- Scene dependency の重複排除、初出順、先行 decode error、外部 Scene の transaction。
- Win64 公開型 layout report。

最終確認コマンド:

```powershell
dotnet run --project acs\tools\acsbuild -- gen
cmake --build .wave-k-build --config Release --target acs_unit_tests --clean-first --parallel 16
cmake --build .wave-k-build --config Debug --target acs_foundation_optimization_wave_k_tests --parallel 16
.\.wave-k-layout\Binaries\Release\acs_unit_tests.exe
.\.wave-k-layout\Binaries\Release\acs_foundation_optimization_wave_k_tests.exe
.\.wave-k-layout\Binaries\Debug\acs_foundation_optimization_wave_k_tests.exe
ctest --test-dir .wave-k-build -C Release --output-on-failure -R "^(ACS.FoundationOptimizationWaveK|ACS.CppConventionsAuditSelfTest|ACS.CppConventionsAudit|ACS.ReferenceTypeNamesAuditSelfTest|ACS.ReferenceTypeNamesAudit|ACS.ModuleSourcesAuditSelfTest|ACS.ModuleSourcesAudit|ACS.SingleHeaderPipelineSelfTest|ACS.AmalgamationDrift|ACS.DistributionConventions|ACS.DistributionHeaderSyntax)$"
python acs\scripts\audit_changed_cpp_rules.py --root . --base-ref 13b5a617f8416f7b921dfa3fd9e258a0a594e051
```

2026-07-30 の Win64 検証では、Debug/Release 専用テストが各 7/7、Release 反復が
100/100、clean build 後の全 unit が 1,138/1,138、上記 CTest が 11/11 合格した。
変更行監査も `changed_cpp_rules=ok files=18 lines=1738` である。生成後の `dist/acs.h`
は 4,212,883 bytes、SHA-256 は
`D1540AB02A62A74FBEE1F5E5771BB28D283530BF7345601371D7E91ABBBEDDB0`。

Temp worktree のため MSBuild は `MSB8029` を警告する。これは中間/出力 directory の
場所に対する警告であり、build/test の pass signal とは分けて記録する。

T57追補のRelease Editor検証では、thumbnail DDC self-testが26/26、
package-responsiveness self-testがexit 0、Editor buildが警告0で合格した。
`verify_editor.ps1 -Mode managed -NativeConfiguration Release`も全managed suite、
package CLI build、deterministic package smokeを含めて合格した。16,384回の
同値だが別instanceのcanonical keyを使った参考計測は、path構築1回、共有16,383回、
pool側allocation 1,152 bytes、毎回再構築側16,252,968 bytesだった。tick値はhostと
tiered JITに依存するため合否に使わず、構築回数、allocation、保持上限を採用根拠とする。
独立source reviewでは、`DerivedDataCache.cs`を直接compileする`acsassetdb`への
pool source登録漏れを検出して修正し、同CLIのDebug/Release buildと各59 self-testで
参照project間のsource構成も確認した。
