# レンダリングバックエンドと機能境界

ACS は 2 つの RHI バックエンドを持つ。**どちらをビルドするかで使える描画機能が変わる**ため、
本ドキュメントで能力境界を明文化する。

| バックエンド | CMake フラグ | 位置づけ |
|---|---|---|
| **raw-DX12** | `-DACS_RENDER_DX12_RAW=ON` (既定 ON) | 軽量・依存ゼロ。2D + 基本 3D 用。`generate.ps1` の既定構成。 |
| **Diligent** | `-DACS_RENDER_DILIGENT=ON` (既定 OFF) | 高度 3D の**正式バックエンド**。IBL/MRT 等を本実装。`generate.ps1 -Diligent`。 |

両方 ON でビルドしてもよい (実行時に `CreateRhiDevice` が振り分ける)。

## Diligent統合のABI・フレーム境界

- Diligent static libraryは内部でC++例外を使う。MSVCでは同じ最終binaryに入るACS targetも
  `/EHsc`と`_HAS_EXCEPTIONS=1`へ揃え、`std::runtime_error`などのSTL ABIを一致させる。
  ACS source側は引き続き例外を投げず、`TResult`で失敗を返す。
- `IDeviceContext::Flush()`はnative pipeline stateを失効させ、
  `FinishFrame()`はdynamic descriptorを回収対象へ移す。全`FDiligentCommandList`は
  1個のimmediate contextを共有するため、PSOの冗長bind判定はDiligentの
  context-global cacheだけに委ねる。ACS側はpipelineをresource lookup用に保持するが、
  `SetPipeline()`のnative通知をcommand-list-local cacheで省略しない。
- dynamic descriptor回収は
  `FDiligentDevice.OffscreenSubmissionsRecycleDynamicDescriptors`、off-screen / primary
  Present境界と複数command list間のPSO整合は
  `FDiligentDevice.SharedContextPipelineStateRemainsCoherent`が回帰確認する。

## 機能サポート表

| 機能 | raw-DX12 | Diligent | 備考 |
|---|---|---|---|
| 2D スプライト / FSpriteBatch / AScene | ✅ | ✅ | 2D ゲームは raw-DX12 だけで完結する |
| 2D ライト・影・コライダー・水/炎エフェクト | ✅ | ✅ | |
| 基本 3D メッシュ / PBR / シャドウマップ | ✅ | ✅ | |
| Bloom / ACES トーンマップ / FXAA 等の単純 postfx | ✅ | ✅ | |
| **IBL (BRDF LUT / env cubemap / irradiance / prefilter)** | ❌ | ✅ | `CImageBasedLighting` の現在の公開境界 |
| **MRT (G-buffer / motion vector)** | ✅ | ✅ | `BeginRenderToTextureMrt` |
| **cubemap/array slice 描画** | ✅ | ✅ | `BeginRenderToTextureSlice` |

## 「fake-success しない」方針

raw-DX12 で高度 3D 機能 (IBL 等) を呼んだ場合、**黙って `Ok()` を返さない**。

- `TResult<void>` を返す経路 (`CImageBasedLighting::BuildBrdfLut` / `BuildEnvCubemap` /
  `LoadEquirectHdrFromMemory` / `EnsureIrradiance` / `BuildIrradiance` /
  `EnsurePrefilter` / `BuildPrefilter`) は **`ACS_ERR(Render, 88)`** を返す
  (「Diligent backend が必要」)。機能ごとに 1 回だけ警告ログを出す。
- `CImageBasedLighting::DrawSkybox` はエラーを返せないため、対象外 backend では
  no-op と 1 回限りの警告ログで能力境界を伝える。raw-DX12 の MRT と slice 描画は
  `CDx12CommandList` が実装しており、この IBL 制限とは独立して利用できる。

これは「未実装の偽装」ではない。**機能自体は Diligent backend に本実装**されており、
raw-DX12 は副次バックエンドとして当該機能を持たない、という**能力境界**を正直に表明する
ものである。高度 3D を使う target は Diligent を有効にしてビルドする:

```powershell
.\generate.ps1 -Diligent -Tests
# または cmake 直接:
cmake -S acs/engine -B <build> -DACS_RENDER_DILIGENT=ON -DACS_BUILD_TESTS=ON
```

## どちらを使うべきか

- **2D ゲーム / 軽量 3D** → raw-DX12 (既定)。依存ゼロで配布が軽い。
- **IBL/TAA/SSR/SSGI/MRT G-buffer を使う高度な 3D** → Diligent。
