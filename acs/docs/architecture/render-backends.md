# レンダリングバックエンド

ACS の `Render` モジュールは Raw DX12 と Diligent の 2 系統の RHI 実装を持ちます。使用する実装はビルド時に決まり、`CreateRhiDevice` が実行時に両者から選択する仕組みではありません。

## ビルド時の選択

| 構成 | `CreateRhiDevice` の実装 |
|---|---|
| `ACS_RENDER_DILIGENT=OFF`、`ACS_RENDER_DX12_RAW=ON` | Raw DX12 |
| `ACS_RENDER_DILIGENT=ON` | Diligent |

`ACS_RENDER_DILIGENT=ON` では `WITH_RENDER_DILIGENT` が有効になり、Diligent 側の生成処理が組み込まれます。`ACS_RENDER_DX12_RAW` も同時に有効な構成では Raw DX12 のソースを含められますが、RHIデバイスの生成処理は Diligent 側です。

Diligent 構成では `FDeviceConfig::backend` により D3D12 または Vulkan を選択します。Vulkan を使う構成では `ACS_DILIGENT_VULKAN=ON` も必要です。

```powershell
# Raw DX12バックエンド
cmake --preset dx12-debug -S .\engine
cmake --build .\engine\cmake-build-debug

# Diligentバックエンド
cmake --preset diligent-debug -S .\engine
cmake --build .\engine\cmake-build-diligent-debug
```

コマンドは ACS リポジトリルートで実行します。両プリセットは `ACS_BUILD_SAMPLES=OFF` を設定し、学習用サンプルを構成対象から除外します。Releaseを構成する場合は、それぞれ `dx12-release` または `diligent-release` を使用します。

## 機能境界

| 機能 | Raw DX12 | Diligent | 補足 |
|---|---:|---:|---|
| 基本的な 2D・3D 描画 | 対応 | 対応 | 共通のRHIインターフェースを使用します。 |
| 複数描画先への同時描画 | 対応 | 対応 | `BeginRenderToTextureMrt` と `BeginRenderToTextureMrtLoad` を実装しています。 |
| テクスチャの配列層・ミップへの描画 | 対応 | 対応 | `BeginRenderToTextureSlice` を実装しています。 |
| `CImageBasedLighting` による IBL 構築 | 非対応 | 対応 | IBL の実装内で Diligentバックエンドを明示的に要求します。 |

IBL の制限は、Raw DX12 の MRT や配列層描画が未実装であることを意味しません。`CImageBasedLighting` 自体が現在 Diligent 専用として実装されています。

## 非対応時の結果

Raw DX12 で `CImageBasedLighting` の構築 API を呼ぶと、`BuildBrdfLut`、`BuildEnvCubemap`、`LoadEquirectHdrFromMemory`、`EnsureIrradiance`、`BuildIrradiance`、`EnsurePrefilter`、`BuildPrefilter` は `ACS_ERR(Render, 88)` を返します。成功値は返しません。

`DrawSkybox` は戻り値を持たないため、Raw DX12 では描画命令を発行せずに終了します。呼び出し側は IBL を必要とする構成で Diligentバックエンドを選択してください。
