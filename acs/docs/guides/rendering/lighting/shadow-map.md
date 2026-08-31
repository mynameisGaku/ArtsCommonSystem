# シャドウマップ

[`CShadowMap`](../../../../src/render/ShadowMap.h) は、有向光源用の深度画像、キャスター描画パイプライン、光源用と物体用の定数バッファを単独所有します。一分割のシャドウマップと、最大4分割の <abbr title="視錐台を距離で分割して影の解像度を配分する方式">CSM</abbr> に対応します。

## 一分割の初期化

```cpp
#include "render/ShadowMap.h"

acs::CShadowMap shadow;
if (shadow.Init(device, 2048u, 1u).IsErr()) {
    return;
}
```

`size == 0` は 2048、`cascade_count == 0` は1として扱います。分割数は `CShadowMap::kMaxCascades` の4までに制限します。初期化に失敗した場合は全資源を解放して空状態へ戻ります。

## 深度パス

`FCasterDraw` はこの例だけで使う局所型です。ACS の `FGpuMesh` とモデル行列をまとめます。

```cpp
#include "render/GpuMesh.h"

struct FCasterDraw
{
    const acs::FGpuMesh* mesh;
    acs::FMat4 model;
};

shadow.SetDirectionalLight(light_direction, scene_center, scene_radius);

if (!shadow.BeginFrame(caster_count)) {
    standard_shader.SetShadowMap(nullptr, shadow.LightViewProjection());
    return;
}

acs::IRhiTexture* depth = shadow.DepthTexture();
acs::IRhiPipeline* pipeline = shadow.CasterPipeline();
if (!depth || !pipeline) {
    standard_shader.SetShadowMap(nullptr, shadow.LightViewProjection());
    return;
}

command_list.BeginShadowPass(*depth, 1.0f);
command_list.SetViewport(shadow.CascadeViewport(0u));
command_list.SetScissor(shadow.CascadeScissor(0u));
command_list.SetPipeline(*pipeline);
command_list.SetConstantBuffer(0u, *shadow.LightCB());

bool complete = true;
for (acs::u32 i = 0; i < caster_count; ++i) {
    const FCasterDraw& caster = casters[i];
    if (!caster.mesh || !caster.mesh->vertex_buffer || !caster.mesh->index_buffer || !shadow.TrySetCaster(caster.model)) {
        complete = false;
        continue;
    }

    acs::IRhiBuffer* object_cb = shadow.CasterObjectCB();
    if (!object_cb) {
        complete = false;
        continue;
    }

    command_list.SetConstantBuffer(1u, *object_cb);
    command_list.SetVertexBuffer(*caster.mesh->vertex_buffer, caster.mesh->vertex_stride);
    command_list.SetIndexBuffer(*caster.mesh->index_buffer);
    command_list.DrawIndexed(caster.mesh->index_count);
}

command_list.EndShadowPass(*depth);

complete = complete && !shadow.CasterOverflowed() && shadow.CasterDrawCount() == caster_count;
standard_shader.SetShadowMap(complete ? depth : nullptr, shadow.LightViewProjection(), 0.001f, 1.0f);
```

`BeginShadowPass` を呼んだ後は、途中のキャスター設定に失敗しても `EndShadowPass` を必ず呼びます。`EndShadowPass` は深度画像をシェーダから読める状態へ遷移します。不完全な深度画像は受信側へ公開せず、そのフレームの影を無効にします。

`TrySetCaster` はモデル行列が有限でない場合、物体用定数バッファを確保できない場合、またはフレームの準備に失敗している場合に `false` を返します。戻り値を確認し、失敗した描画を後段の有効な影として数えません。

## 影を受ける描画

`CStandardShader::SetShadowMap` は深度画像、光源の表示・投影行列、バイアス、ぼかし範囲を受け取ります。深度画像は所有しないため、主描画と GPU 実行が終わるまで `CShadowMap` を生存させます。

現在の `CStandardShader` は、遮蔽物探索と半影の評価を行う <abbr title="遮蔽物と受光面の距離から半影幅を求めるソフトシャドウ方式">PCSS</abbr> を使い、最初の有向光源だけへ影を適用します。`filter_radius == 0` は最小幅の硬い影、`1` は標準の半影、より大きい値は広い半影です。バイアスが小さすぎると自己遮蔽が出やすく、大きすぎると影が物体から離れます。

## 複数分割

複数分割では `SetDirectionalLightCascades` でカメラの `view`、`projection`、`near_z`、`far_z` を渡します。深度パスでは各分割について `SetCurrentCascade`、`CascadeViewport`、`CascadeScissor` を設定し、同じキャスター群を描きます。`BeginFrame` の引数は一分割あたりの予定キャスター数です。

`CStandardShader` は一分割の行列だけを受け取ります。CSM の深度アトラス、各行列、分割距離を使う描画には `CPbrShader::SetShadowMapCascades` を使います。

キャスター用パイプラインは表面自身の影を減らすため前面を除外します。厚みのない片面メッシュでは影が欠ける場合があるため、形状とカリング条件を確認します。
