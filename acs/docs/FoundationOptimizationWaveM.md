# Foundation Optimization Wave M

## 目的

Wave M の T65〜T72 は、世代付きハンドル、timer、immutable byte decode、
待機、event 配送、package path 検索の共有基盤を、観測可能な契約を弱めずに
監査・最適化する。採用判断は次の四点で行った。

- 目的: 実際に繰り返される RMW、文字列比較、所有参照を減らす。
- 効果: 決定的な回数または layout で差分を説明できる。
- 依存: 既存の `FJobGraph`、`FMessageBroker`、`.acpak` 正規形を再利用する。
- 検証可能性: Debug/Release、nested mutation、Reset 後再構築、package
  round-trip、公開 layout、規約・reference・module・配布物監査から確認できる。

品質、完全一致規則、package format、公開 handle の意味は変更しない。

## T69〜T72: 待機・event・package path

## T69: 完了カウンタの一括予約

### 採用

変更前の `FJobGraph` は、Submit で entry job 群と一時 guard を加算し、依存が
解けるたびに後続 job を `Add(1)` していた。entry が一つの127 job graph では、
完了数を増やす RMW が127回必要だった。

変更後は Submit 冒頭で `m_JobCount` を一回だけ加算する。未投入の entry と
依存待ち job も先に残数へ含まれるため、後続公開の隙間で Wait に 0 を見せない。
各 job は従来どおり一度だけ `Done()` し、投入失敗時の同期 fallback も同じ一票を
消費する。127 job の構造指標は、加算 RMW が127回から1回、余分な guard の
`Done()` が1回から0回になった。

既存 `FJobGraphDiagnostics` は Win64 40B ABI を維持した。現在の一括予約だけを
返す `FJobGraphCompletionDiagnostics` を独立した16B値型にし、`FJobGraph`
本体には member を増やしていない。診断は累計ではない。Submit 済みなら
`{1, JobCount()}`、未 Submit または Reset 後なら `{0, 0}` を返す。この契約は、
Reset 後に job を追加できる既存挙動とも矛盾しない。

### 検証

- 127 job の二分木依存を16回反復し、Wait 後に全2,032 job が完了する。
- 各 Submit の現在予約が `{1, 127}`、各 Reset 直後が `{0, 0}` になる。
- Reset 後に job を1件追加し、次の予約が `{1, 128}`、実行が128件増える。
- ThreadPool 未初期化時の同期 fallback と既存 cycle 検出は従来テストで維持する。

## T70: hot atomic の cache-line 分離

### 保留

Wave I の実測対象は `FWorkerDeque` の owner-only `bottom` と stealer 更新の
`top` であり、同 wave が二つを64B境界へ分離する。Wave M が同じ箇所を重複して
変更する必要はない。

`FPoolState` の `outstanding`、`queued_work`、`api_users` 等を一律
`alignas(64)` にする案も精査したが、独立して競合しているという測定根拠がなく、
owner state と診断 counter を大きく膨らませる。`FCompletionCounter` も共有する
一つの値なので内部で分離する対象がない。根拠のない padding を入れず、Win64
4B layout を維持した。新しい profiler 証跡で別 owner の二 atomic が同一 line
上で競合すると確認できるまで T70 は `Deferred` とする。

## T71: allocation-free event snapshot の寿命固定

### 採用

`FMessageBroker` は Publish 開始時の `slots.Size()` を境界として保持する。
発行中の Subscribe は末尾へ追加されるため outer Publish には混入せず、次の
Publish と nested Publish から見える。Unsubscribe は `active=false` を即時反映し、
同じ配送内の未実行 callback を抑止する。別の dispatch 配列を追加して再利用する
案は、現在の allocation-free 境界 snapshot より走査・所有コストを増やすため
採用しなかった。

callback 呼び出し前に関数 pointer と user pointer を値へ退避した。callback 内の
大量 Subscribe が `slots` を再確保しても、実行中 callback の寿命が無効化された
slot 参照へ依存しない。

### 検証

一回の outer Publish 内で96購読を追加して再確保を起こし、未実行購読を即時解除し、
nested Publish も実行する複合試験を追加した。

- 発行中に追加した96購読は outer では0回、nested では各1回。
- 即時解除した購読は outer / nested とも0回。
- 既存購読は outer / nested の各1回。
- 次の Publish では追加済み96購読が各1回増える。

## T72: `.acpak` 正規形 path の hash-once

### 採用

Writer と Reader に重複していた仮想 path 検証を
`IsCanonicalAcpakVirtualPath` へ統一した。正規形は相対 path、`/` 区切り、
空でない通常 segment、妥当な UTF-16 であり、`\`、drive、先頭・末尾区切り、
`.` / `..` segment を拒否する。

`HashCanonicalAcpakVirtualPath` は大文字小文字や Unicode を変換せず、
従来の完全一致規則を保つ。Writer は AddFile 時に一度 hash して pending entry
へ保持し、重複候補だけを文字列比較する。Reader は manifest 読み込み時に各 path
を一度 hash して `m_PathHashes` に保持し、FindEntry は要求を一度 hash して
一致候補だけを従来の `wcscmp` 相当で確認する。hash collision 時も文字列比較する
ため、異なる path を同一視しない。

ディスク format、version、file table は一切変更していない。`Assets/Texture.bin`
と `assets/texture.bin` は従来どおり別 entry であり、backslash query は一致しない。

### 所有コスト

Win64 Release の `FAcpakReader` は304Bから336Bへ32B増えた。追加分は allocator
対応 `TArray<u64>` owner 一つであり、manifest 保持量は entry ごとに8B増える。
公開 `FAcpakFileEntry` とディスク layout は不変である。この固定 owner コストと
8B/entry を、反復検索・重複検査から長い UTF-16 比較を除くために採用した。

### `.acpak` v1 format reference

format の詳細は include のたびに読む宣言 header へ長文で置かず、ここで管理する。
archive は little-endian の header、file payload 群、末尾 file table の順に並ぶ。

- header は magic 8B、version 4B、flags 4B、file count 4B、padding 4B、
  file table offset 8B、reserved 4B の計36B。
- table entry は UTF-16LE path 長と path、payload offset、展開後 size、格納 size、
  展開後 plaintext の CRC32 を持つ。
- 暗号化時だけ96-bit nonce と128-bit authentication tag の計28Bを追加する。
- writer は compress-then-encrypt、reader は decrypt-then-decompress の順で処理する。
- `flags == 0` では暗号 field を書かず、従来の v1 raw layout と一致する。
- `wchar_t` は Windows の UTF-16 code unit として永続化し、数値 field は明示的な
  byte 読み書きで扱う。構造体 padding を disk size として使用しない。

`AcpakFormat.h` は各定数・型・関数の直前に短い日本語契約だけを残した。設計説明を
ここへ移したため、format の監査可能性を保ったまま公開 header の読解範囲を限定する。

## 公開 layout

Win64 Release の実測値:

| 型 | `sizeof` | 契約 |
|---|---:|---|
| `FCompletionCounter` | 4B | 根拠のない cache-line padding を追加しない |
| `FJobGraphDiagnostics` | 40B | 既存 ABI を維持 |
| `FJobGraphCompletionDiagnostics` | 16B | 2個の `u64`、独立値型 |
| `FJobGraph` | 4,032B | member 追加なし、既存上限を維持 |
| `FAcpakReader` | 336B | hash owner 32B を明示的に許容 |

## 検証コマンド

```powershell
dotnet run --project acs\tools\acsbuild -- gen --root acs
cmake -S acs\engine -B .wave-m-build -DACS_LAYOUT_ROOT=<worktree>\.wave-m-layout
cmake --build .wave-m-build --config Release --target acs_foundation_optimization_wave_m_tests acs_foundation_public_layout_tests --parallel 16
cmake --build .wave-m-build --config Debug --target acs_foundation_optimization_wave_m_tests acs_foundation_public_layout_tests --parallel 16
.\.wave-m-layout\Binaries\Release\acs_foundation_optimization_wave_m_tests.exe
.\.wave-m-layout\Binaries\Debug\acs_foundation_optimization_wave_m_tests.exe
ctest --test-dir .wave-m-build -C Release --output-on-failure -R "^(ACS.ChangedCppRulesAuditSelfTest|ACS.FoundationOptimizationWaveM|ACS.FoundationPublicLayout|ACS.CppConventionsAuditSelfTest|ACS.CppConventionsAudit|ACS.ReferenceTypeNamesAuditSelfTest|ACS.ReferenceTypeNamesAudit|ACS.ModuleSourcesAuditSelfTest|ACS.ModuleSourcesAudit|ACS.AmalgamationDrift|ACS.DistributionConventions|ACS.DistributionHeaderSyntax)$"
python acs\scripts\audit_changed_cpp_rules.py --root . --base-ref a537c07cc66096837e4aba58fd050d4039200ee8
powershell -NoProfile -ExecutionPolicy Bypass -File acs\scripts\build_single_header.ps1
```

最終確認では Wave M 専用試験4/4と公開 layout 試験1/1が Debug / Release の
双方で合格した。Release 専用試験の反復 stress は100/100、全 unit は
Release 1,144/1,144、Debug 1,148/1,148で合格した。規約・reference type・module
source・amalgamation drift・配布物監査をまとめた Release CTest は12/12で合格し、
changed C++ rules は Wave M 基点で20 files / 752 lines、Wave K 基点を含む広い範囲で
27 files / 2,476 linesを合格した。reference JavaScript 2件の構文検査も合格した。
単一 header と Diligent / xxhash を含む Debug / Release 統合 library は
`build_single_header.ps1` で再生成し、配布 pipeline の完走を確認した。
## T65〜T68: ハンドル・timer・入力記録

Wave M は、世代付きハンドル、疎な timer 走査、少数 lookup、immutable byte decode
を、既存の意味論と所有権を変えずに監査・最適化する。汎用部品を先に増やさず、
実 consumer、失敗条件、寿命、結果 parity を確認できる経路だけを採用する。

## 採用判断

| Task | 判断 | 期待効果 | 依存関係 | 検証可能性 |
|---|---|---|---|---|
| T65 handle layout | 採用 | 異種ハンドルの ABI drift を compile time で検出 | 既存の object/entity/timer/subscription handle | offset・幅・size の static assert と既存の stale handle 再利用テスト |
| T66 word-batched bitset | 既存実装を正式採用 | inactive slot の field 読み出しを避ける | `FTimerManager::m_ActiveWords` と x64 bit scan | 8192 timer 中 1 active の訪問数・word 数、callback mutation/Clear |
| T67 tiny lookup policy | 延期 | 閾値なしの policy 増加を回避 | 代表 workload と transition/cycle 指標が未整備 | 実 consumer の計測値が得られるまで未実装 |
| T68 immutable decode | 採用 | header/sample の中間 copy・decode 前 allocation を除去 | `TSpan<const u8>`、既存 `.acsr` CRC/上限 | 正常 field parity、truncation、CRC、範囲外、transactional load |

## T65: generation handle layout trait

`TGenerationHandleLayoutTraits<T>` は物理配置だけを記述する。無効 identity は
`FObjectHandle` が `0xFFFFFFFF`、`FEntityId` が `0xFFFFFFFF`、timer/subscription が
`0` と異なり、generation の進め方も各 owner が保持する。このため共通 base handle
や pack/unpack API は作らず、identity/generation の offset・byte 幅、domain prefix、
型全体の size/alignment だけを各 handle header で特殊化した。

`FSubscriptionHandle` の channel は domain prefix として明示し、
`FObjectHandle` の 64bit generation と padding も縮めない。これにより既存 stale
handle 判定を変えず、ABI の意図しない並べ替えだけをテストで検出できる。

## T66: timer active word

`FTimerManager` は既に `m_ActiveWords` を production の `Tick` で走査し、
set bit だけを bit scan している。generic bitset を追加して二重管理せず、この経路を
採用した。callback は timer の追加、cancel、`Clear` を実行できるため、各 callback
後に同じ word を再読込する。初回 snapshot に固定すると、後続 slot の cancel を
見落とすか、再利用 slot を誤って処理するため、この再読込は削除しない。

8192 timer のうち 8191 件を cancel した試験では active slot 訪問は 1 件であり、
word load は 128 件で固定する。次段の summary bitset は、この word load 自体が
代表 workload の bottleneck と計測された場合だけ検討する。

## T67: tiny lookup の延期条件

repository 内の少数 lookup を調査したが、linear から sorted/hash へ切り替える件数
閾値を再現可能に決める production consumer と計測値は得られなかった。汎用 policy
を追加しても呼び出し・状態遷移の削減を検証できないため延期する。

再開条件は、実 consumer、件数分布、lookup/更新比、比較回数または cycle/hardware
counter を同じ workload で取得し、選択した閾値が少なくとも二構成で改善すること。

## T68: bounded immutable recording view

`FInputRecordingView::Decode` は caller-owned `.acsr` を借用し、次を allocation 前に
全件検証する。

- magic `ACSR`、version 1、tick rate と sample 件数の製品上限
- header 16 byte + `29 * sample_count` + CRC footer 4 byte の厳密サイズ
- sample 領域の CRC32
- mouse x/y の NaN・Infinity 拒否

一 sample の layout は tick 4 byte、key code 8 byte、key state 8 byte、mouse x/y
各 4 byte、button mask 1 byteの合計 29 byteであり、`FInputSample::sizeof` に依存
しない。`TSpan::TrySubSpan` は null、範囲外、加算 overflow を assert なしで拒否し、
失敗時に出力 view を変更しない。

format constant、LE read/write、CRC32、finite mouse 検査、sample encode/decode は
非公開 `input_recording_detail` の `InputRecordingFormat.h/.cpp` に一元化する。
serializer、owned loader、immutable view が同じ helper を使うため、29 byte layout
や CRC の片側だけが更新される drift point を残さない。shared helper は owner や
update lifecycle を持たないため subsystem にはしない。

view は buffer を所有しない。view の使用中、caller は元 buffer を生存かつ不変に
保つ。mapped file の close と競合する公開 view は導入していない。
`FInputRecorder::TryLoadFromBuffer` はこの decoder を使い、全検証後にだけ owned
sample array を staging するため、既存の失敗時 state 不変契約も維持する。

## InputRecorder の役割と範囲

`FInputRecorder` は OS 寄りの key state と mouse 入力を tick 順に保存し、TAS、
自動テスト、バグ再現へ使う。`FLockstep` の解釈済み button/axis 記録とは独立する。
通常は input event を `FInputSample` に変換した直後に `Capture` し、再生時は
`ConsumeSample` の結果を OS 入力の代わりに渡す。

storage は `TArray<FInputSample>`、検索は記録順を前提とした cursor 前進である。
録画・再生・idle は排他的で、コピーとムーブを禁止する。現時点の範囲外は一 tick
9 件以上の key 変化、wheel/gamepad analog、圧縮、巻き戻し、複数 recorder の協調である。

## 検証

- clean Debug `acs_unit_tests`: 1151 passed / 0 failed
- clean Release `acs_unit_tests`: 1147 passed / 0 failed
- handle layout static assert と公開 layout 出力
- timer sparse word diagnostics と callback mutation/Clear
- immutable view の正常 decode、truncation、CRC、span fail-closed
- changed C++ rule audit: 15 files / 325 lines
- C++ conventions audit: 1177 files
- module source manifest audit: 29 source directories / 296 `.cpp`
- `acsbuild gen` の再実行で同一 `Module.cmake` を生成
