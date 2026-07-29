# Foundation Optimization Wave J/L

## 目的

Wave J/L は、描画品質・数値精度・公開 API の互換性を下げず、CPU 側の反復、
コマンド保持、ディスクリプタ管理、PSO 検索の固定費を減らすための基盤更新である。
既存の単体経路はフォールバックおよび結果比較の基準として維持する。

## 期待効果・依存関係・検証可能性

| 対象 | 期待効果 | 依存関係 | 検証方法 |
|---|---|---|---|
| frustum/transform/hash batch | 同じ結果を保ったまま反復固定費を削減 | x64 SSE は任意。非 x64 は scalar fallback | scalar parity と端数・無効入力テスト |
| ECS sparse prefetch | 大規模 query の sparse 対応表待ちを隠蔽 | compiler prefetch intrinsic。128 件未満は従来経路 | query の要素・世代・構造変更契約テスト |
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

### T49: ECS sparse prefetch

- 128 件以上の `Query::Each` だけ、16 件先の required component sparse 対応表へ
  prefetch hint を発行する。
- required set のポインターは走査前に一度だけ解決する。
- 小規模クエリ、スナップショット、世代検査、構造変更安全性は変更しない。

### T50: バッチ hash

- 独立範囲向け `HashBytesBatch` と、四レーンの依存鎖を交互実行する
  `HashMix64Batch4` を追加した。
- 各出力は既存 `HashBytes` / `HashMix64` とビット単位で一致する。

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
- graphics/compute descriptor から 128 bit `FPipelineStateKey` を生成する。
- 固定容量・確保不要の `TPipelineStateKeyCache` は open addressing で key を
  intern し、二つの 64 bit 値を照合して primary hash 衝突を同一視しない。
- shader identity、RT/depth、input layout、resource names、sampler、raster/depth/
  stencil/MSAA をキーへ含める。

## 決定的なコスト指標

| 項目 | 従来 | Wave J/L |
|---|---:|---:|
| 16 scene command の backing 確保 | 1 回以上 | 0 回 |
| `N` 個 per-slice RTV の descriptor lock | `N` 回 | 1 回 |
| frustum plane 判定の同時 lane 数 (x64) | 1 | 4 |
| PSO key table の追加確保 | 利用側依存 | 0 回 |
| ECS prefetch が動く最小件数 | なし | 128 件 |

これらは allocator count、pool high-water/free count、scalar parity の Release テストで
固定しており、壁時計だけに依存する不安定な合否判定は置いていない。

## 検証

- raw DX12 Release `acs_unit_tests`: **1109 passed / 0 failed**
- raw DX12 Release `acs_editor_abi_lifecycle_tests`: **exit 0**
- raw DX12 Debug `acs_unit_tests`: **1113 passed / 0 failed**
- raw DX12 Debug `acs_editor_abi_lifecycle_tests`: **exit 0**
- SIMD frustum: valid/invalid/tail を含む scalar parity
- SoA transform: in-place 更新を含む scalar parity
- hash: range batch と四整数 lane の scalar parity
- descriptor pool: transactional failure、二重返却、LIFO recycle
- inline command storage: 16 件まで allocation 0、spill 後の容量再利用
- pipeline metadata/key: constexpr validation、同値 intern、状態差分
- raw DX12 Release の clean build は **約 126 秒**、最終差分の incremental build は
  **19.2 秒**だった。
- Release binary size は `acs_unit_tests.exe` **5,661,696 bytes**、
  `acs_editor_abi.dll` **2,591,232 bytes**、
  `acs_editor_abi_lifecycle_tests.exe` **76,800 bytes**だった。
- `audit_changed_cpp_rules.py --base-ref origin/main` は、統合側で別途修正済みの
  `ReflectApply.h` だけを除いて違反なし。
- 関連 CTest の unit/lifecycle は Debug・Release とも全件通過し、
  convention/module/single-header/distribution 監査も全件通過した。
- 配布対象ゲームが未登録の構成なので package target は生成されず、実ゲーム package
  の起動確認は対象外である。

## バックエンドと互換性

- raw DX12 の実ビルド・実行を検証した。
- Diligent backend の動作ロジックは変更せず、共通型分割に伴う include のみ更新した。
  Diligent 構成は初回依存取得に約 10 分を要するため、この wave のローカル検証では
  実施していない。
- 新 API は追加であり、既存 public method の署名、描画精度、shader 精度、
  resource lifetime 契約は変更していない。
