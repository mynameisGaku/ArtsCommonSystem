# Foundation Optimization Wave C

## Scope

Wave C は描画品質を一切変更せず、アセット、ECS、reflection、RHI/DX12 の
CPU/driver hot path と安全契約を改善する。対象は T11–T15 と T31–T35。

## 実装

| ID | 変更 | 保持する契約 |
|---|---|---|
| T11 | `FAssetRegistry::LoadAsync` に in-flight table を追加。同一 `FAssetId` の同時要求は同じ `FAsyncLoadState` と job を共有する。成功・失敗・OOM の全経路で table から除去する。 | path validation、同一結果 identity、失敗分類、Shutdown の active-operation fence |
| T12 | Query snapshot を dense index ではなく世代付き `FEntityId` に変更し、required component を entity/type ごとに 1 回だけ取得する。 | iteration order、destroy/reuse 時の stale-generation 拒否、mutation snapshot |
| T13 | ECB は 16 bytes 以下の trivially-copyable value を command 内へ保持し、それ以外は従来の heap fallback を使う。逐次/parallel buffer に batch reserve API を追加。 | command order、move-only fallback、OOM sticky flag、parallel slot order |
| T14 | scene/component authored-value 適用で使う reflection dispatch を built-in kind の immutable descriptor table に統一。 | field order、schema-only skip、未知 kind の fail-closed fallback |
| T15 | raw DX12 command list は同一 graphics/compute pipeline の連続 bind を省略する。resource transition と UAV barrier は変更しない。 | backend state safety、draw/dispatch quality、barrier ordering |
| T31 | `TComponentTypeTraits<T>` と compile-time `ComponentSignatureId` を追加。dense World slot は従来の dynamic runtime ID fallback を維持する。 | plugin/dynamic component registration、既存 ABI の runtime indexing |
| T32 | required/excluded/optional query pack を template specialization で解決し、optional component は nullable pointer で渡す `EachOptional` を追加。 | required filtering、excluded semantics、optional absence、mutation safety |
| T33 | `EFieldKind` の apply/read function と対応可否を constexpr descriptor table 化し、table coverage を static assert する。 | unsupported String/Enum、out-of-range kind、schema validation |
| T34 | 全 `EFormat` の bytes/block、block dimensions、color/depth/stencil aspect、compression flag を constexpr traits table 化。DX12/Diligent の texture validation/readback/layout が同じ表を使用する。 | invalid format fail-closed、depth/color usage legality、backend parity |
| T35 | `TRhiPipelineBindPolicy<Graphics/Compute>` を compile-time domain policy として追加し、pointer identity による collision-free bind cache を raw DX12 と Diligent に使用する。 | compute/graphics domain separation、dynamic pipeline fallback、backend parity |

## Deterministic operation counts

以下は wall-clock に依存しない hot-path operation count。

| Path | Before | After |
|---|---:|---:|
| 同じ未完了 path への `N` async load | loader/I/O/job `N` 回 | loader/I/O/job `1` 回 |
| `R` required component を持つ query match | sparse lookup `2R` 回 | sparse lookup `R` 回 |
| reserve 後の `128 x Add<FHealth>` | command storage 1 + value heap 128 allocations | command storage 1 allocation |
| 同一 pipeline の連続 `N` bind | native PSO/root bind `N` 回 | native PSO/root bind `1` 回 |
| format bytes/aspect validation | backend ごとの switch/table | 共通 constexpr table 1 lookup |

ECB の inline 化で `FCommand` は大きくなる。particle/spawn の典型的な小型 component
では個別 allocation を消す利益を優先し、16 bytes を超える型と非 trivial 型は従来経路へ
退避する。

## Release evidence

環境: Visual Studio 18 2026, MSVC 19.51.36252, Windows SDK 10.0.28000.0,
raw DX12, `ACS_BUILD_TESTS=ON`, x64 Release, parallelism 8。

| Metric | Before | After | Difference |
|---|---:|---:|---:|
| clean Release `acs_unit_tests` build (single sample) | 116.9 s | 130.4 s | +13.5 s |
| `acs_unit_tests.exe` | 5,616,128 bytes | 5,625,344 bytes | +9,216 bytes (+0.164%) |
| unit tests | 1,098 pass | 1,102 pass | +4 regression tests |

Clean build time は同一マシンで各 1 sample のため、OS cache、並列 scheduling、header fan-out
を含む参考値である。runtime hot-path の改善量は上の deterministic count を gate とする。
binary 増加は constexpr descriptor tables、追加 diagnostics/test instantiation を含み
0.2% 未満に収まる。

検証コマンド:

```powershell
cmake --build .\Intermediate\vs --config Release --target acs_unit_tests --parallel 8 --clean-first
.\Binaries\Release\acs_unit_tests.exe
```

結果: `passed=1102 failed=0`。

## Regression coverage

- 同一 path の async future 2 本が loader 1 回と同一 asset identity を共有する。
- Query 中に未訪問 entity を destroy/recreate しても新世代を同じ snapshot で訪問しない。
- required + optional specialization が present/null の両方を正しく渡す。
- ECB reserve 後の小型 component 128 件が追加 allocation を行わず、Flush 後の値を保持する。
- component signature、全 format traits、reflection dispatch coverage、typed pipeline domains を
  compile-time assertion で検証する。
