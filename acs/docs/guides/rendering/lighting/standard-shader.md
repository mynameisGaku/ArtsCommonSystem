# 標準ライティングシェーダ

[`CStandardShader`](../../../../src/render/StandardShader.h) は、`FMeshVertex` 形式の静的メッシュを、有向光源、点光源、環境光、鏡面反射、アルベド画像、シャドウマップで描画します。シェーダ、パイプライン、定数バッファ、代替の白画像を単独所有します。

## 初期化

`Init` は頂点シェーダとピクセルシェーダのコンパイル、描画パイプライン、定数バッファ、白画像の作成を行います。どれか一つでも失敗した場合は描画へ進みません。

```cpp
#include "render/StandardShader.h"

acs::CStandardShader shader;
if (shader.Init(device, color_format, depth_format).IsErr()) {
    return;
}
```

`depth_format == EFormat::Unknown` では深度テストを無効にします。通常の 3D 描画では、描画先と一致する色形式と深度形式を指定します。

## フレームごとの描画

`BeginFrame` へ、そのフレームで予定するメッシュ描画数を渡します。必要な物体用定数バッファは増加し、以後のフレームでも再利用されます。

```cpp
const acs::FDirLight lights[] = {
    {acs::FVec3{-0.5f, 0.8f, 0.3f}, acs::FVec3{1.0f, 0.95f, 0.85f}},
};

if (!shader.BeginFrame(visible_count)) {
    return;
}

shader.SetLights(view_projection, camera_position, lights, 1u, acs::FVec3{0.08f, 0.10f, 0.14f});
shader.SetPointLights(nullptr, 0u);

for (acs::u32 i = 0; i < visible_count; ++i) {
    shader.DrawMesh(command_list, *visible[i].mesh, visible[i].model, visible[i].base_color, visible[i].specular_strength, visible[i].shininess, visible[i].albedo);
}

const bool complete = shader.ObjectDrawCount() == visible_count;
```

`DrawMesh` は物体用定数バッファの設定、パイプラインと資源の結び付け、`DrawIndexed` をまとめて行います。アルベド画像が `nullptr` の場合は内部の白画像を使います。

`DrawMesh` は戻り値を持たず、未初期化、無効な `FGpuMesh`、物体用定数バッファの確保失敗では描画を省略します。全描画が記録されたことを確認する場合は、最後に `ObjectDrawCount` を予定数と比較します。個別に失敗を扱う場合は `SetObject` の戻り値を確認し、`Pipeline`、`PerFrameCB`、`PerObjectCB` を使って結び付けと描画を行います。

## 光源と材質

`SetFrame` は有向光源一灯用の簡易 API です。複数の有向光源には `SetLights` を使います。有向光源と点光源はそれぞれ最大4灯で、超過分は使いません。`count > 0` の場合、配列ポインターは有効でなければなりません。

`base_color` はアルベド画像へ乗算します。`specular_strength` は鏡面反射の強さ、`shininess` はハイライトの鋭さです。現在の実装は Lambert 拡散反射と Blinn-Phong 鏡面反射を使用します。

## 所有権と失敗条件

- `CStandardShader` はコピーできません。GPU 装置を破棄する前に `Shutdown` するか、デストラクターを実行します。
- `FGpuMesh` は有効な頂点バッファ、インデックスバッファ、頂点幅、インデックス数を持つ必要があります。
- `SetPointLights` は値を内部へコピーします。`SetShadowMap` が受け取る深度画像は非所有参照です。
- `DrawMesh` が受け取るアルベド画像は所有しません。GPU が描画命令を実行し終えるまで生存させます。
- `BeginFrame` が `false` を返したフレームでは、このシェーダによる描画を行いません。
- `u32` の最大値は内部の無効値として使うため、予定描画数に指定できません。
