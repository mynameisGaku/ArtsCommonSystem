# Foundation Optimization Wave M/T

## 目的

Wave M/T は、描画品質や公開 API を変えずに render graph alias 候補の可視化と
階層可視性の共通化を実描画経路へ導入する。効果を証明できない upload arena と
状態ソートは採用せず、将来の backend 実装に必要な境界を明示する。

## 実装と実経路

### T73: transient upload arena を不採用

検討した共通 arena は採用しなかった。既存 PBR object constant buffer pool も
buffer を retained slot として所有し、`BeginFrame()` は cursor reset だけで O(1)
だった。新案も object ごとに別 RHI buffer を使うため、production path の allocation
件数、RHI 呼び出し数、command 数を削減できない。抽象化だけを最適化完了とせず、
source、test、Module 登録、本番統合をすべて撤去した。

再検討には suballocation 可能な backend upload page、dynamic offset、または同じ
品質を保った descriptor bind 削減と、その決定的な計測が必要である。

### T74: 状態ソートを不採用

検討した opaque material hash sort は採用しなかった。現行 PBR backend は pipeline
を既に再利用する一方、draw ごとに必要な texture bind は残るため、hash と sort の
毎フレーム費用を追加しても状態遷移削減を証明できない。utility、test、Module 登録、
draw loop 変更をすべて撤去した。

material path batch hash を scene cache invalidation に使う案も、既存 key が
material loaded state と実描画値を保持しているため correctness 効果を証明できず
撤去した。T50 と T74 はともに production consumer が成立するまで未完了である。

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
| upload arena | 既存 pool より改善なし | backend suballocation 未実装 | 不採用 |
| alias planner | backend alias 実装前に安全な候補と上限を可視化 | post process lifetime metadata | candidate only、実 memory 削減なし |
| visibility batch | consumer ごとの親 walk を一回の prepass へ統合 | DFS pre-order scene enumeration | scalar parity と fail-safe fallback |

## 検証手順

1. Debug/Release で `acs_unit_tests`、`acs_render_graph_transient_alias_tests`、
   `acs_editor_abi_lifecycle_tests` を build する。
2. CTest の `ACS.UnitTests`、`ACS.RenderGraphTransientAlias`、
   `ACS.EditorAbiLifecycle` を実行する。
3. `audit_changed_cpp_rules.py --base-ref origin/main` と convention、
   reference type、Module source、single header、amalgamation、distribution
   の各 audit を実行する。
4. `scripts/amalgamate.py --write` 後に `dist/acs.h` の drift と syntax を検査する。

最終検証では Debug `ACS.UnitTests` が **1116 passed / 0 failed**、Release が
**1112 passed / 0 failed**、専用 alias test は両構成とも **2 passed / 0 failed**
だった。editor ABI lifecycle も両構成で exit 0 を確認した。

raw DX12 を実 build/runtime 対象とする。Diligent は source 契約へ同じ metadata
検査を追加したが、依存 backend がローカル構成されていないため runtime 検証の
対象外である。
