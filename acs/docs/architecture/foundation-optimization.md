# ACS 基盤の責務と検証契約

## 目的

ACS の基盤機能は、module と機能フォルダーを責務境界とし、既存の owner、memory、event、
build 機構へ接続する。局所状態、決定論的な値、単純な計算は値型または通常の機能型に置き、
複数の利用者が共有する寿命と更新・終了処理を持つ機能だけを subsystem とする。

本書は基盤機能の現在の責務、所有権、失敗条件、互換性、検証入口をまとめる。

## 構成原則

- `container`、`memory`、`math` は値と計算を提供し、アプリケーション寿命を所有しない。
- `threading`、`event`、`platform` は明確な owner の下で待機、配送、OS 境界を管理する。
- `asset` と package reader は読み取り中の snapshot と一時領域を所有し、利用側へ部分的な
  状態を公開しない。
- `ecs` と `reflection` は世代、型識別、field 適用を管理し、scene や editor の固有状態を
  所有しない。
- `render` は backend resource と GPU 完了境界を所有し、editor は表示・編集・起動段階を
  管理する。描画用の値型や分類表を subsystem へ移さない。
- 公開主要型は一 header に一つを原則とし、実装を持つ型は同名 source へ分離する。
- 公開 header が参照する module は公開依存へ含め、内部 library の header は配布 header へ
  露出させない。

## Container、Memory、Math

### 配列、文字列、hash

- `TArray` と `FString` の成長は再確保を優先し、自己参照 append は移動前の位置を保持して
  再解決する。容量計算の overflow または確保失敗では既存の要素と文字列を維持する。
- `THashMap::TryInsert` は既存 key の更新を成長判定より先に行う。検索済み hash を受け取る
  経路も key 比較を省略せず、hash collision を同一 key と扱わない。
- `FStableStringKey` と `FStringHasher` は同じ文字列規則を使用する。stable key を利用する
  map は対応する hasher を明示する。
- `HashBytes` の空入力は有効である。null と非 0 長の組み合わせは memory を参照せず失敗値を
  返す。

### JSON と pool

- JSON 入力は 64 MiB、入れ子は 256 段を上限とする。writer は staging へ全体を書き、
  深さ、出力長、確保、数値表現の検査に成功した場合だけ呼び出し側の文字列を更新する。
- `CPoolAllocator::AllocBatch` は取得できた prefix 件数を返し、残りの出力を null にする。
  `FreeBatch` は pool 外、位置不正、null、重複、解放済みの pointer を拒否する。
- `TTypedPoolAllocator` は構築に成功した object だけを破棄対象とする。owner は allocator の
  終了前に全 object を明示的に破棄または解放する。
- `TIsTriviallyRelocatable` の特殊化は、byte 移動後の元 storage を destructor なしで破棄できる
  型だけに限定する。

### Arena と transform

- `CArenaAllocator::AllocBatch` は同じ size と alignment の領域を一つの連続予約として扱う。
  失敗時は全出力を null にし、利用量と診断値を更新しない。
- `Reset(false)` は世代を進め、先頭 page だけを即時再利用する。残りの page は再公開前に
  初期化する。`Reset(true)` は保持 page を backing allocator へ返す。
- `EBatchTransformPolicy::Point` は平行移動を含み、`Vector` は含まない。batch 経路は scalar
  経路と同じ結果を返し、未対応 policy は compile 時に拒否する。

## Threading、Event、Timer

### Thread pool と job graph

- `CThreadPool` は idle worker を条件変数で待機させ、投入済み仕事量と待機数に応じて通知する。
  外部投入は FIFO を保ったまま bounded batch で取得する。
- `SubmitCallable` が受理する callable は構築、呼び出し、破棄を例外なしで行える必要がある。
  inline または heap の保存先にかかわらず、実行、投入失敗、終了の各経路で一度だけ破棄する。
- `CThreadPool::ParallelFor` は通常規模の context を呼び出し側 storage に置き、大きい処理は
  owner が持つ固定 pool を利用する。固定 pool を利用できない場合も局所 heap へ退避して結果を
  維持する。終了処理は利用中 storage の寿命が切れるまで待つ。
- `CJobGraph` は依存 topology を検証して再利用し、投入単位の完了数を一度に予約する。cycle、
  不正依存、投入失敗の同期 fallback でも各 job の完了票を一度だけ消費する。

### Message pipe と broker

- 既定の `TMessagePipe` は MPMC FIFO、bounded push、batch 順序を維持する。SPSC 特殊化は
  2 の累乗 capacity と acquire/release ordering を使い、値操作に例外なしを要求する。
- `CMessageBroker` は publish 開始時の購読範囲を固定する。publish 中の追加は次の配送から
  有効になり、解除は未実行 callback へ即時反映される。nested publish と slot 再確保中も
  実行中 callback の関数と user pointer の寿命を保持する。
- 購読 handle の generation は最大値を使用後に飽和し、古い handle と一致する slot を
  再発行しない。

### Timer

- `CTimerManager` は active word と set bit だけを走査し、generation 付き handle で対象 slot を
  直接 cancel する。callback が同じ word を変更した後は word を読み直す。
- callback 中に追加された timer は次の tick から処理する。Once、Repeating、不正 period、
  nested callback、`Clear` の各経路で slot と handle の整合を維持する。
- timer generation も最大値を使用後に飽和し、古い handle の再一致を防ぐ。

## Platform、Input、Log

- `CFileSystem` は text を caller buffer へ直接読み込み、4 GiB を超える要求を OS 呼び出し前に
  拒否する。atomic write は対象と同じ directory の一時 file を flush 後に置換し、失敗時は
  旧内容を保って一時 file を削除する。置換直前にも reparse 対象を検査する。
- directory 作成は通常 file、中間 file、device namespace、不正な UNC を成功扱いしない。
- Network は null、0 byte、OS の長さ上限を呼び出し前に分類する。TCP の 0 byte は OS を
  呼ばず、UDP の空 datagram は有効な空 packet として送信する。受信元は受信成功時だけ更新する。
- 接続済み gamepad は毎 frame 取得し、未接続 port は bounded scheduler で分散確認する。
- 最小化中の application loop は更新と描画を止め、message または待機上限まで休止する。
  frame clock は進め、復帰直後の delta time に最小化期間を混ぜない。
- 非同期 logger は空から非空へ変わる境界で writer を起こす。固定文字列と無効な固定 severity は
  compile 時に不要な整形と評価を除去する。

### 入力記録

- `FInputRecordingView` は caller が所有する `.acsr` byte 列を借用する。使用中は元 buffer を
  生存かつ不変に保つ。
- version 1 は 16-byte header、1 sample あたり 29 byte、4-byte CRC footer である。magic、
  tick rate、件数、厳密長、CRC、有限な mouse 座標を allocation 前に検査する。
- decode 失敗時は出力 view と recorder の既存状態を変更しない。

## Asset、Package、Storage

- `CAssetRegistry::LoadAsync` は同じ `FAssetId` の未完了要求を一つの state、job、物理 read へ
  合流する。成功、読込失敗、確保失敗、終了の全経路で in-flight entry と active operation を
  解放する。
- async path は `CAssetPathInterner` が registry の寿命内で共有する。保持上限を超えた場合や
  全 entry が使用中の場合は、呼び出し側だけが所有する path へ退避する。
- Derived Data Cache の path pool は Cook と thumbnail の各 owner が個別に所有する。Asset の
  path pool と寿命を混在させず、owner 不在の global cache を作らない。
- 大きい immutable `.acpak` は read-only mapping を試し、失敗時は buffered I/O へ戻る。
  reader は保持 scratch を一回の read だけへ貸し、競合時または上限超過時は局所 buffer を使う。
- raw、圧縮、暗号化の各経路は CRC、展開、認証を最終 scratch 上で完了してから caller buffer を
  更新する。失敗時に caller buffer を部分更新しない。
- `ReadFiles` は入力順に commit する。後続失敗時は先行成功数を `CompletedCount` へ返すため、
  all-or-nothing API ではない。
- Scene dependency は kind と完全 path で重複排除し、初出順を維持して batch 読み込みする。
  公開 Scene は全 decode と参照解決に成功した後だけ更新する。

### `.acpak` version 1

- archive は little-endian header、payload 群、末尾 file table の順に並ぶ。
- header は magic 8 byte、version 4 byte、flags 4 byte、file count 4 byte、padding 4 byte、
  file table offset 8 byte、reserved 4 byteの計 36 byteである。
- table entry は UTF-16LE path 長と path、payload offset、展開後 size、格納 size、展開後
  plaintext の CRC32 を持つ。暗号化時は 96-bit nonce と 128-bit authentication tag を持つ。
- writer は圧縮後に暗号化し、reader は復号後に展開する。`flags == 0` では暗号 field を持たない。
- 仮想 path は相対 path、`/` 区切り、空でない通常 segment、有効な UTF-16 とする。drive、
  `\\`、先頭・末尾区切り、`.`、`..` segment を拒否する。
- path hash は候補抽出だけに使い、最終一致は大文字小文字と Unicode を変換せず文字列比較する。

## ECS と Reflection

- query snapshot は世代付き `FEntityId` を保持し、destroy と slot 再利用後の entity を同じ snapshot
  へ混入させない。required component は entity と型ごとに一度取得する。
- command buffer は小さい trivially-copyable value を command 内に保持し、それ以外は既存の
  allocator 経路を使う。逐次・並列のどちらも記録順と失敗状態を維持する。
- `TComponentTypeTraits` と query の template 特殊化は既知の component を compile 時に分類する。
  plugin など動的登録は runtime ID 経路を維持する。
- Reflection は `EFieldKind` ごとの適用・読み出し関数を immutable descriptor table で管理する。
  未対応 kind、範囲外、schema 不一致は field を適用せず失敗として返す。

## Render と Editor

- format の block size、block dimensions、color/depth/stencil aspect、圧縮属性は共通の constexpr
  table を利用し、backend ごとの判定差を作らない。
- graphics と compute の pipeline bind cache は domain を分離し、同一 native pipeline の連続 bind
  だけを省略する。resource transition、UAV barrier、描画順は変更しない。
- shader parameter layout は backend 呼び出し前に検証する。pipeline state key は shader、render
  target、depth、input、resource、sampler、raster、stencil、MSAA の有効状態を含み、独立した二値と
  descriptor 比較で collision を確認する。
- descriptor slot pool は batch の全件成功または全件失敗を保証し、二重返却を拒否する。
- scene command は小規模分を inline 保持し、超過後の動的容量を再利用する。priority の安定順、
  flush 中 enqueue、one-shot と repeating の意味を維持する。
- frustum と transform の batch 経路は scalar parity、端数、無効入力、階層順を検証する。
- transient upload storage、render graph の alias 候補、描画 sort key は GPU resource の所有権と
  frame 完了境界の中で管理する。
- Editor renderer は段階的に起動し、CPU で準備できる shader bytecode と owner thread で作成する
  RHI resource を分離する。scene が要求していない描画機能は遅延初期化する。
- pipeline 再構築は完成した候補だけを公開する。失敗した候補は live resource を置換しない。
  GPU 完了を確認できない終了経路は解放順を保ち、使用中 resource の早期解放を避ける。

## スクリプティング、型付きイベント、音声

- `CLuaVm::RegisterNativeFunction` は同じ VM への登録中再入を拒否し、Lua stack と再入状態を
  全 return 経路で復元する。closure は登録識別番号を持ち、古い closure が後続登録を呼ばない。
- Typed Event と delegate は rvalue reference を拒否し、値引数の copy 可否と実際の callback の
  例外なし呼び出しを compile 時に検査する。
- Audio volume は全公開経路で有限値を `[0, 1]` へ制限し、NaN と無限大を 0 とする。
  再生開始中の backend 設定失敗では確保済み voice と buffer を破棄し、無効 handle を返す。
  既存 voice の更新失敗では直前の backend 値を維持する。

## 互換性と公開レイアウト

- 永続化形式の magic、version、field 順、CRC、予約 byte は共通の明示的な byte 読み書きで管理する。
  C++ 構造体の padding を保存形式へ使用しない。
- 公開 handle は identity、generation、必要な domain を既存の幅と順序で保持する。共通 base へ
  無理に統合せず、layout trait と compile-time 検査で意図しない変更を検出する。
- 公開型の size と alignment は Win64 の layout test で上限を持つ。上限は機能追加を妨げる
  完全一致ではなく、hot descriptor と inline storage の設計容量を守る予算である。
- virtual table または公開 owner layout を変更した構成では、利用 module と配布 consumer を
  同じ ACS revision で再 build する。

## 生成物と検証

`acs/tools/acsbuild` は各 module 配下の header と source を収集して `Module.cmake` を生成する。
生成結果を手編集せず、同じ入力から再生成して差分がないことを確認する。公開 API リファレンス、
単一 header、配布 manifest も C++ 宣言と同じ source 集合から検査する。

静的検証の入口:

```powershell
python -B acs/scripts/audit_cpp_conventions.py --root acs
python -B acs/scripts/audit_cpp_type_roles.py --root acs/src --migration-debt acs/scripts/data/cpp_type_role_migration_debt.json
python -B acs/scripts/audit_reference_type_names.py --root acs
python -B acs/scripts/audit_module_sources.py --root acs
python -B acs/scripts/amalgamate.py --check
```

release 判定では、Debug と Release の全 CTest、Raw DX12 と Diligent の有効構成、公開 layout、
並行所有権 stress、package 生成、単一 header consumer の構文・link・実行を確認する。実時間は
診断値として扱い、失敗時状態、exact byte、回数、順序、上限、layout を決定的な合否条件とする。
