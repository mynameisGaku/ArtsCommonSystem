# Foundation Optimization Wave M/T

## 目的

Wave M/T は、描画品質を変えずに upload page、描画順key、render graph alias候補、
階層可視性の共通化を実描画経路へ導入する。最適化は削減したGPU resource数または
決定的な作業量で検証し、実時間は環境差を切り離した補助診断値として扱う。

## 実装と実経路

### T73: transient upload arena

- `FTransientUploadArena` は同じ大きさの論理定数sliceを256 byte境界で一つの
  CPU writable GPU pageへまとめる。容量超過時だけページを追加し、通常の
  `BeginFrame()` はcursorと検索位置を戻すだけでO(1)である。
- `FStandardShader` と `FPbrShader` のper-object定数はこのarenaを実使用する。
  初期64 drawは64個のRHI bufferから1ページへ、512 draw予約は従来の幾何成長で
  729個まで増えていた個別RHI bufferから2ページへ減る。
  shader、定数layout、draw順、解像度は変更していない。
- `IRhiBuffer::BindingBuffer()` と `BindingOffset()` は論理sliceをbackend bindへ渡す。
  Raw DX12は親upload bufferのGPU仮想addressへoffsetを加え、Diligentは
  `SetBufferRange()`で同じ範囲を指定する。通常bufferは従来どおり自身とoffset 0を返す。
- `UINT32_MAX`予約、積算overflow、ページ生成失敗はGPU API前または公開前に拒否し、
  既存ページとslice addressを保持する。

Raw DX12のCPU writable bufferは2個のframe slotを内部で持ち、submit fenceを記録して
次slot選択前に完了を待つ。Diligentの`UpdateBuffer` copyは同一queue上で順序づけられ、
frame slotの再利用前にfence完了を待つ。したがってrenderer通常のframe境界でarenaの
CPU cursorを戻しても、実行中GPUが読む範囲は上書きされない。

### T74: compile-time描画sort key

- `TDrawPacketSortKeyLayout<TFieldBits...>` はfield数、bit幅、shift、64 bit上限を
  compile timeで確定し、優先順どおりのkey生成を重複なく記述する。
- `FSpriteSortList` はlayerとIEEE 754 depthを64 bit keyへ変換する。負値を含む数値順、
  正負zeroの同値、NaNの決定的な正の無限大扱いを定義した。
- 24件以下は初期化費用の小さい挿入sort、25件以上は可変byteだけを走査する安定LSD
  radix sortを使う。command本体は動かさず、scratchとkey配列は次frameへ再利用する。
- 同じlayer/depthはstable sortで提出順を維持する。textureやmaterialによる再配置は
  行わないため、透明合成と既存描画品質は変わらない。

16,384 commandの専用harnessでは、逆順挿入sortの基準134,209,536比較に対し、
key生成とradixの決定的な走査は98,304件、削減率99.927%だった。Release計測時間は
補助値であり、合否は8 pass以下、18N走査以下、順序parityで判定する。opaque
materialの状態遷移sortは別要件であり、削減を証明するconsumerがないため導入しない。

### T75: render graph transient alias 候補

- `FRenderGraphTransientAliasPlanner` は inclusive lifetime が重ならず、backend の
  heap/alignment 制約を含む compatibility class が一致する transient resource
  だけを候補にする。slot size は割り当て候補の最大値になる。
- resource ID 順の決定的な assignment、重複 ID、無効範囲、overflow、OOM の
  fail-closed 契約を持つ。
- post process の luma chain から candidate plan を構築し、heap bytes と
  potential saved bytes を診断値として公開する。

これは候補診断だけであり、現時点で GPU memory は alias していない。実際の
memory 削減には backend ごとの placed resource、alias barrier、queue lifetime
追跡が必要である。Bloom target 数と解像度は変更しておらず、品質低下はない。

### T76: hierarchy visibility batch

- `FHierarchyVisibilityBatch` は DFS pre-order を一度走査し、親の visible/enabled
  状態を子へ伝播する。
- editor scene mesh prepass で一度評価し、scene cache、vertex build、frustum
  candidate、SSSS material 検査、PBR draw count が同じ結果を再利用する。
- malformed order や boundary 不一致では scalar parent walk へ戻り、部分結果を
  公開しない。
- 公開 `usize` count の加算 overflow は確保前に拒否し、部分結果を残さない。

再現テストは hidden parent、disabled child、malformed fallback に加え、1024 node
の DFS 順を 64 回 scalar baseline と比較する。

## 期待効果と依存関係

| 対象 | 期待効果 | 依存関係 | 現在の保証 |
|---|---|---|---|
| upload arena | 初期64 drawのGPU buffer生成を64から1へ削減 | backend range bindとframe fence | 512 drawを2ページで保持、既存shader品質と順序を維持 |
| draw sort key | 大量sprite sortをO(N²)から最大8 byte passへ変更 | stable radixとcompile-time field layout | layer、depth、提出順parity |
| alias planner | backend alias 実装前に安全な候補と上限を可視化 | post process lifetime metadata | candidate only、実 memory 削減なし |
| visibility batch | consumer ごとの親 walk を一回の prepass へ統合 | DFS pre-order scene enumeration | scalar parity と fail-safe fallback |

## 互換性

`FTransientUploadArena`、`TDrawPacketSortKeyLayout`、`ObjectBufferPageCount()`は
公開source APIの追加である。既存の`BeginFrame()`、`SetObject()`、通常bufferのbindは、
`IRhiBuffer::BindingBuffer()`と`BindingOffset()`の既定実装によりsource互換を保つ。

一方で`IRhiBuffer`へのvirtual関数追加はvtableを変更し、`FStandardShader`、
`FPbrShader`、`FSpriteSortList`は所有memberの置換または追加でobject layoutが変わる。
このwaveはbinary ABI互換ではないため、render moduleとそれらの公開型を使用する
plugin・consumerを同じrevisionで再buildする。`IRhiCommandList`も統計領域を各具象へ
移すprivate pure virtual hookがvtable末尾へ増えるため、独自派生classは再buildと
hook実装が必要である。draw/dispatchの記録は具象memberを直接更新し、hot pathに
統計取得用virtual callを追加しない。
配布物のsingle header、import library、consumer検証も同じbuildから作る。

## 検証手順

1. Debug/Release で `acs_unit_tests`、`acs_render_packet_performance`、
   `acs_render_graph_transient_alias_tests`、`acs_editor_abi_lifecycle_tests` をbuildする。
2. CTest の `ACS.UnitTests`、`ACS.RenderPacketPerformance`、
   `ACS.RenderGraphTransientAlias`、`ACS.EditorAbiLifecycle` を実行する。
3. `audit_changed_cpp_rules.py --base-ref origin/main` と convention、
   reference type、Module source、single header、amalgamation、distribution
   の各 audit を実行する。
4. `scripts/amalgamate.py --write` 後に `dist/acs.h` の drift と syntax を検査する。

今回の検証ではDebug `ACS.UnitTests`が **1172 passed / 0 failed**、Releaseが
**1168 passed / 0 failed**、両構成の`ACS.RenderPacketPerformance`が **pass**だった。
backend単独構成もRaw DX12 Debugが **1170 passed / 0 failed**、Diligent D3D12 Debugが
**1139 passed / 0 failed**で、各構成の`ACS.RenderPacketPerformance`も **pass**だった。
配布物を含む最終値はwave全体の公開前gateで更新する。

現在の統合構成はRaw DX12とDiligent D3D12を同時にbuildし、unit testの既定RHIで
実GPU page、range bind、512 drawを検証する。backend単独構成では同じsort負荷と
各RHIの初期化・描画・終了経路を検証する。
