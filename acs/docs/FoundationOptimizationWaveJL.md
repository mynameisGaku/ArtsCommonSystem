# Foundation Optimization Wave J/L

## 目的

Wave J/L は、描画品質・数値精度・公開 API の互換性を下げず、CPU 側の反復、
コマンド保持、ディスクリプタ管理、PSO 検索の固定費を減らすための基盤更新である。
既存の単体経路はフォールバックおよび結果比較の基準として維持する。

## 期待効果・依存関係・検証可能性

| 対象 | 期待効果 | 依存関係 | 検証方法 |
|---|---|---|---|
| frustum/transform batch | 同じ結果を保ったまま反復固定費を削減 | x64 SSE は任意。非 x64 は scalar fallback | scalar parity と端数・無効入力テスト |
| ECS sparse prefetch | 固定距離の改善を証明できず cache pollution の可能性あり | 代表 workload と cache miss 指標が未整備 | 不採用 |
| inline command storage | 通常規模の queue allocation を除去 | 既存 allocator と `TArray` の spill storage | allocation count と queue 順序テスト |
| descriptor pool | descriptor lock と再利用固定費を削減 | raw DX12 descriptor heap | transactional failure と recycle テスト |
| shader metadata/PSO key | layout 検査と PSO lookup の確保を除去 | 共通 RHI descriptor 型 | constexpr、状態差分、衝突照合テスト |

公開 API と source 構成の追加は各 `Module.cmake`、利用側 include、単体テスト、
本書、および配布用 `dist/acs.h` へ反映した。CTest の unit/lifecycle と
convention/module/single-header/distribution 監査から再現可能に検証できる。

## 実装

### T47: AABB/frustum バッチ判定

- `EvaluateSpheresBatch` を追加し、x64 では四球を SSE で同時評価する。
- 妥当性検査と world radius 計算は `PrepareSphere` に一本化した。
- 四個未満の端数および非 x64 は `EvaluateSphere` へ戻る。
- エディタの実描画経路 `BuildSceneMeshVisibility` をバッチ API へ接続した。
- 無効入力は従来どおり fail-open とし、部分的な可視マスクを公開しない。

### T48: SoA transform 更新

- `FTransformSoAInput` と `ComposeTransformBatchSoA` を追加した。
- 入出力を同じ配列にでき、AoS 一時 transform を構築せずに位置・回転・
  スケールを連続走査する。
- 結果は全要素について `FTransform3D::Compose` と同じである。
- エディタの scene mesh prepass は明示 stack の反復 DFS で順序を保ち、兄弟を
  16 件ずつ合成する。host が結果と stack の容量を保持するため、warm frame は
  allocation 0 で scene cache、vertex 構築、frustum 判定、material 検査へ同じ
  world transform を渡す。列挙数や確保が不正なら既存の scalar `World()` へ戻る。
- `FHierarchyWorldTransformBatch` はエンジン上限の深さ 512 を持つ chain 8 本、
  合計 4096 node を 64 回評価し、scalar parity、pre-order、保持容量の再利用、
  非再帰動作を固定する。

### T49: ECS sparse prefetch を不採用

128 件以上、16 件先という固定 prefetch を検討したが、代表的な sparse query の
cache miss または latency 改善を再現できず、cache pollution で逆効果になる可能性を
排除できない。prefetch API、production call、parity test をすべて撤去した。
再検討には entity 分布と component 密度を固定した workload、および hardware
counter による miss/latency 指標が必要である。

### T50: バッチ hash を不採用

独立範囲と四レーンの batch hash を検討したが、実 backend で hash 回数や状態遷移を
減らす production consumer を確立できなかった。scene mesh material path を
毎フレーム hash する案も、既存の material loaded state と描画に使う material 値が
cache key に含まれており、追加費用に対する correctness 改善を証明できないため
撤去した。utility、test、Module 登録、本番統合を残していない。

### T59: command inline storage

- `TInlineArray<T, N>` を追加した。
- `FSceneCommandQueue` の先頭 16 command をオブジェクト内に保持する。
- 17 件目で動的配列へ移行し、その容量は `Clear` 後も再利用する。
- priority の安定順序、flush 中 enqueue、one-shot/repeating の契約は維持する。

### T60: descriptor batching/recycle

- `TDescriptorSlotPool<Capacity>` に単体・バッチ確保、単体・バッチ返却、
  二重返却防止、全件成功または全件失敗の契約を実装した。
- raw DX12 の SRV/UAV、DSV、RTV を同じプール実装へ統合した。
- array/cubemap の per-slice RTV は、一スライス一ロックから一バッチ一ロックへ
  変更した。
- RTV 件数の乗算 overflow は GPU リソースを公開する前に拒否する。

### T61/T62: constexpr shader layout と PSO key intern

- `FShaderParameterLayoutMetadata` は graphics/compute の slot 上限を constexpr で
  検査できる。
- raw DX12 と Diligent の graphics/compute pipeline 作成前に metadata を検査し、
  不正な slot layout を backend 呼び出し前に拒否する。
- graphics/compute descriptor から 128 bit `FPipelineStateKey` を生成する。
  primary と verification は別 seed、別 multiplier、別 avalanche で byte 列を
  独立走査する。
- 固定容量・確保不要の `TPipelineStateKeyCache` は open addressing で key を
  intern し、二つの 64 bit 値を照合して primary hash 衝突を同一視しない。
- shader identity、RT/depth、input layout、resource names、sampler、raster/depth/
  stencil/MSAA をキーへ含める。
- RT state は backend が消費する有効状態へ正規化する。MRT 時の legacy
  `rt_format`、有効数より後ろの `rt_formats`、depth-only 時の全 RT field は
  hash しない。legacy 単一 RT と同内容の明示 1-RT も同じ key になる。
- raw DX12 device は cpp-private の遅延 heap owner で最大 512 件の
  PSO/root signature を所有して再利用する。公開 device 本体には owner pointer
  だけを置き、key table と COM 配列による約 20 KiB の ABI 肥大を避ける。
  同内容を別 backing storage に置いた文字列も同じ key となり、同じ native
  pointer が返ることをテストで固定した。
- final queue 完了を証明できない teardown では、heap owner 内の cache 所有参照を
  意図的に解放せず null 化してから owner metadata だけを破棄する。retired GPU
  resource と同じく shutdown 時の use-after-free を
  避ける fail-safe であり、通常終了では全参照を解放する。source 契約テストは
  queue 完了判定後に cache reset が行われる順序、通常終了の `Release`、異常終了の
  PSO/root signature null 化、最後の key table reset を固定する。
- この保持方針は raw DX12 queue fence 完了を証明できない異常終了だけに依存する。
  device-loss 時も安全に待機または fence retire できる backend 契約が整った時は、
  leak-safe 保持を明示的な遅延解放へ置き換えて再検討する。

## 決定的なコスト指標

| 項目 | 従来 | Wave J/L |
|---|---:|---:|
| 16 scene command の backing 確保 | 1 回以上 | 0 回 |
| `N` 個 per-slice RTV の descriptor lock | `N` 回 | 1 回 |
| frustum plane 判定の同時 lane 数 (x64) | 1 | 4 |
| PSO key table の追加確保 | 利用側依存 | 0 回 |

PSO cache owner は最初の native pipeline 登録時に一度だけ確保する。lookup には owner
pointer の acquire read が一段増える一方、device の作成だけで 512 entry table を
構築・保持せず、公開 layout と cold device cost を抑える。cache 内の hot probe と
COM AddRef は outer device lock の内側で実行する。

Win64 Debug/Release 共通の layout 実測は、inline cache 構成 **42,400 byte**、
cpp-private owner 構成 **21,920 byte** で、**20,480 byte (48.3%)** を削減した。
比較値は同じ compiler/ABI で cache table、二つの 512 pointer 配列、lock を
device pointer と置換した byte 数から算出し、22,528 byte の公開 layout budget と
20,000 byte 超の削減 static assert で固定する。

device 側には owner pointer と小さな outer `SRWLOCK` だけを残す。Find/Store は
owner pointer を読む前に outer lock を取得し、Reset は同じ lock の内側で pointer
を切り離し、COM release または意図的 null 化、key reset、owner delete まで完了する。
これにより `load owner → Reset/Delete → freed lock access` の競合窓を作らない。
source 契約テストは三経路の lock-before-owner と delete-before-unlock を固定する。

これらは allocator count、pool high-water/free count、scalar parity の Release テストで
固定しており、壁時計だけに依存する不安定な合否判定は置いていない。

## 検証

- raw DX12 Release `acs_unit_tests`: **1112 passed / 0 failed**
- raw DX12 Release `acs_editor_abi_lifecycle_tests`: **exit 0**
- raw DX12 Debug `acs_unit_tests`: **1116 passed / 0 failed**
- raw DX12 Debug `acs_editor_abi_lifecycle_tests`: **exit 0**
- SIMD frustum: valid/invalid/tail を含む scalar parity
- SoA transform: in-place 更新を含む scalar parity
- descriptor pool: transactional failure、二重返却、LIFO recycle
- inline command storage: 16 件まで allocation 0、spill 後の容量再利用
- pipeline metadata/key: constexpr validation、同値 intern、状態差分
- Debug・Release とも対象 binary を clean build してから検証した。増分 build の
  成否や壁時計、binary size はこの wave の改善判定には使っていない。
- `audit_changed_cpp_rules.py --base-ref origin/main` は、統合側で別途修正済みの
  `ReflectApply.h` だけを除いて違反なし。
- 関連 CTest の unit/lifecycle は Debug・Release とも全件通過し、
  convention/module/single-header/distribution 監査も全件通過した。
- 配布対象ゲームが未登録の構成なので package target は生成されず、実ゲーム package
  の起動確認は対象外である。

## バックエンドと互換性

- raw DX12 の実ビルド・実行を検証した。
- Diligent backend にも pipeline metadata の同じ事前検査を追加した。
  Diligent 構成は初回依存取得に約 10 分を要するため、この wave のローカル検証では
  source 契約の確認までとし、runtime backend は実施していない。
- 新 API は追加であり、既存 public method の署名、描画精度、shader 精度、
  resource lifetime 契約は変更していない。
