# スキンメッシュアニメーション

ACS のスキンメッシュアニメーションは、CPU 側の [`ASkinnedMeshAsset`](../../reference/symbols/asset/askinnedmeshasset-13dc3444.html)、再生位置を管理する [`CAnimationPlayer`](../../reference/symbols/asset/canimationplayer-c9accf01.html)、GPU バッファを所有する [`FSkinnedGpuMesh`](../../reference/symbols/render/fskinnedgpumesh-a6d6495e.html)、描画状態を所有する [`CSkinnedShader`](../../reference/symbols/render/cskinnedshader-82687ae1.html) で構成します。

処理順は次のとおりです。

1. 頂点、インデックス、ボーン、アニメーションチャンネルを `ASkinnedMeshAsset` へ格納する。
2. [`ComputeInverseBindMatrices`](../../reference/symbols/asset/askinnedmeshasset-13dc3444/computeinversebindmatrices-1f654cff.html) でバインドポーズの逆行列を作る。
3. [`UploadSkinnedMesh`](../../reference/symbols/render/uploadskinnedmesh-2327b377.html) で頂点とインデックスを GPU へ転送する。
4. `CAnimationPlayer` を更新し、ボーンパレットを作る。
5. `CSkinnedShader` へモデル行列とパレットを設定して描画する。

## 2 ボーンの最小アセットを作る

次の例は、下端をルートボーン、上端を子ボーンが動かす 1 枚の三角形を作ります。配列の確保に失敗した場合は、未完成のアセットを返しません。

```cpp
#include "asset/SkinnedMeshAsset.h"
#include "math/Quat.h"
#include "memory/SharedPtr.h"

acs::TSharedPtr<acs::ASkinnedMeshAsset> MakeTwoBoneTriangle() noexcept
{
    acs::TSharedPtr<acs::ASkinnedMeshAsset> mesh =
        acs::MakeShared<acs::ASkinnedMeshAsset>();
    if (!mesh)
        return {};

    auto& vertices = mesh->Vertices();
    auto& indices = mesh->Indices();
    auto& bones = mesh->Bones();
    auto& animations = mesh->Animations();
    if (!vertices.TryResize(3u) || !indices.TryResize(3u) || !bones.TryResize(2u) || !animations.TryResize(1u))
        return {};

    vertices[0].position = acs::FVec3{-0.5f, 0.0f, 0.0f};
    vertices[1].position = acs::FVec3{0.5f, 0.0f, 0.0f};
    vertices[2].position = acs::FVec3{0.0f, 1.0f, 0.0f};
    for (acs::u32 index = 0u; index < 3u; ++index)
    {
        vertices[index].normal = acs::FVec3{0.0f, 0.0f, 1.0f};
        vertices[index].u = index == 1u ? 1.0f : 0.0f;
        vertices[index].v = index == 2u ? 0.0f : 1.0f;
        vertices[index].weights[0] = 1.0f;
    }
    vertices[0].bones[0] = 0u;
    vertices[1].bones[0] = 0u;
    vertices[2].bones[0] = 1u;
    indices[0] = 0u;
    indices[1] = 1u;
    indices[2] = 2u;

    bones[0].parent = -1;
    bones[1].parent = 0;
    bones[1].bind_translation = acs::FVec3{0.0f, 0.5f, 0.0f};

    acs::FAnimation& animation = animations[0];
    animation.duration = 1.0f;
    if (!animation.channels.TryResize(1u))
        return {};

    acs::FAnimationChannel& channel = animation.channels[0];
    channel.bone_index = 1;
    if (!channel.keys.TryResize(3u))
        return {};

    channel.keys[0].time = 0.0f;
    channel.keys[0].translation = bones[1].bind_translation;
    channel.keys[0].rotation = acs::FQuat::Identity();

    channel.keys[1].time = 0.5f;
    channel.keys[1].translation = bones[1].bind_translation;
    channel.keys[1].rotation = acs::FQuat::AxisAngle(acs::FVec3{0.0f, 0.0f, 1.0f}, 0.65f);

    channel.keys[2].time = 1.0f;
    channel.keys[2].translation = bones[1].bind_translation;
    channel.keys[2].rotation = acs::FQuat::Identity();

    mesh->ComputeInverseBindMatrices();
    return mesh;
}
```

[`FSkinnedVertex`](../../reference/symbols/asset/fskinnedvertex-8a7d8d41.html) は最大 4 つのボーン番号と重みを持ちます。使用する重みの合計は 1 に揃え、未使用の重みは 0 にします。`CSkinnedShader` が参照できるパレットは [`kMaxBones`](../../reference/symbols/render/cskinnedshader-82687ae1/kmaxbones-a6a6c2df.html) の 64 行列なので、頂点のボーン番号は 0 から 63 に収めます。

[`FBone::parent`](../../reference/symbols/asset/fbone-63d3a66d/parent-3ccff958.html) は負数がルートです。子ボーンは親ボーンより後ろへ並べます。`ComputeInverseBindMatrices()` はこの順序で親ボーンのワールド座標におけるバインドポーズを参照します。

[`FAnimationChannel`](../../reference/symbols/asset/fanimationchannel-725677bb.html) は 1 つのボーンを対象とし、[`FAnimationKey`](../../reference/symbols/asset/fanimationkey-eb45cba5.html) は時刻順に並べます。チャンネルがないボーンはバインドポーズを使います。空のキー配列を持つチャンネルは単位 TRS を適用するため、動かさないボーンには空のチャンネルを追加しません。

## 再生して描画する

次の型は、アセットと GPU バッファの準備、パレットの更新、インデックス描画を 1 つの寿命へまとめます。

```cpp
#include "asset/AnimationPlayer.h"
#include "foundation/Log.h"
#include "foundation/Move.h"
#include "render/IRhiCommandList.h"
#include "render/RenderAssets.h"
#include "render/Renderer.h"
#include "render/SkinnedShader.h"

class FSkinnedAnimationRenderer
{
public:
    bool Initialize(acs::CRenderer& renderer) noexcept
    {
        acs::IRhiDevice* device = renderer.Device();
        if (!device)
            return false;

        acs::TSharedPtr<acs::ASkinnedMeshAsset> mesh = MakeTwoBoneTriangle();
        if (!mesh)
            return false;

        acs::FSkinnedGpuMesh staged_mesh{};
        auto upload = acs::UploadSkinnedMesh(*device, *mesh, staged_mesh);
        if (upload.IsErr())
        {
            ACS_LOG_ERROR("スキンメッシュを転送できません: %s", upload.Error().message);
            return false;
        }

        auto shader = m_Shader.Init(*device, renderer.ColorFormat(), renderer.DepthFormat());
        if (shader.IsErr())
        {
            ACS_LOG_ERROR("スキンシェーダーを初期化できません: %s", shader.Error().message);
            m_Shader.Shutdown();
            return false;
        }

        m_Mesh = acs::Move(mesh);
        m_GpuMesh = acs::Move(staged_mesh);
        m_Player.SetMesh(m_Mesh.Get());
        m_Player.Play(0u, true);
        return m_Player.IsPlaying();
    }

    void Update(acs::f32 delta_seconds) noexcept
    {
        m_Player.Update(delta_seconds);
    }

    void Draw(acs::CRenderer& renderer, const acs::FMat4& view_projection, acs::FVec3 camera_position) noexcept
    {
        acs::FMat4 palette[acs::CSkinnedShader::kMaxBones];
        const acs::u32 bone_count = m_Player.WritePalette(palette, acs::CSkinnedShader::kMaxBones);
        if (bone_count == 0u)
            return;

        acs::IRhiCommandList* command = renderer.CommandList();
        if (!command || !m_Shader.BeginFrame(1u))
            return;

        m_Shader.SetFrame(view_projection, camera_position, acs::FVec3{0.0f, -1.0f, 0.0f}, acs::FVec3{1.0f, 1.0f, 1.0f}, acs::FVec3{0.08f, 0.08f, 0.10f});
        if (!m_Shader.SetObject(acs::FMat4::Identity(), acs::FVec3{0.85f, 0.35f, 0.20f}) || !m_Shader.SetBonePalette(palette, bone_count))
            return;

        acs::IRhiPipeline* pipeline = m_Shader.Pipeline();
        acs::IRhiBuffer* frame_cb = m_Shader.PerFrameCB();
        acs::IRhiBuffer* object_cb = m_Shader.PerObjectCB();
        acs::IRhiBuffer* bones_cb = m_Shader.BonesCB();
        acs::IRhiTexture* white = m_Shader.DefaultWhiteTexture();
        if (!pipeline || !frame_cb || !object_cb || !bones_cb || !white || !m_GpuMesh.vertex_buffer || !m_GpuMesh.index_buffer)
            return;

        command->SetPipeline(*pipeline);
        command->SetConstantBuffer(0u, *frame_cb);
        command->SetConstantBuffer(1u, *object_cb);
        command->SetConstantBuffer(2u, *bones_cb);
        command->SetTexture(0u, *white);
        command->SetVertexBuffer(*m_GpuMesh.vertex_buffer, m_GpuMesh.vertex_stride);
        command->SetIndexBuffer(*m_GpuMesh.index_buffer);
        command->DrawIndexed(m_GpuMesh.index_count);
    }

    void Shutdown() noexcept
    {
        m_Player.Stop();
        m_Player.SetMesh(nullptr);
        m_Mesh.Reset();
        m_GpuMesh = acs::FSkinnedGpuMesh{};
        m_Shader.Shutdown();
    }

private:
    acs::TSharedPtr<acs::ASkinnedMeshAsset> m_Mesh;
    acs::FSkinnedGpuMesh m_GpuMesh;
    acs::CAnimationPlayer m_Player;
    acs::CSkinnedShader m_Shader;
};
```

`Draw` は `CRenderer::BeginFrame` 後の描画区間で呼びます。`Update` と `Draw` の間も `m_Mesh` を保持し、再生器の非所有参照を有効に保ちます。

[`CAnimationPlayer::SetMesh`](../../reference/symbols/asset/canimationplayer-c9accf01/setmesh-b5467803.html) はメッシュを所有しません。例では `m_Mesh` が再生器より長くアセットを共有所有します。[`Play`](../../reference/symbols/asset/canimationplayer-c9accf01/play-c47b34b6.html) はメッシュが未設定、またはアニメーション番号が範囲外の場合は再生を開始しません。

[`Update`](../../reference/symbols/asset/canimationplayer-c9accf01/update-f60cfd5e.html) は再生時刻を進め、繰り返し再生では継続時間の範囲へ戻します。[`WritePalette`](../../reference/symbols/asset/canimationplayer-c9accf01/writepalette-ad9e3ea1.html) は現在姿勢を補間し、書き込んだ行列数を返します。メッシュまたは出力先がない場合は 0 です。

描画ごとに [`SetObject`](../../reference/symbols/render/cskinnedshader-82687ae1/setobject-3a728e18.html) を先に呼び、続けて [`SetBonePalette`](../../reference/symbols/render/cskinnedshader-82687ae1/setbonepalette-36e1f919.html) を呼びます。この 2 つは同じ描画専用定数バッファーの組を使います。[`BeginFrame`](../../reference/symbols/render/cskinnedshader-82687ae1/beginframe-682af4eb.html) には、そのフレームで予定するスキンメッシュ描画数を渡します。

## 失敗条件と寿命

- `UploadSkinnedMesh` は頂点が空の場合、または GPU バッファ作成に失敗した場合にエラーを返します。インデックスが空でも転送は成功しますが、例のインデックス描画にはインデックスバッファが必要です。
- `UploadSkinnedMesh` の出力は途中まで更新される可能性があるため、一時 `FSkinnedGpuMesh` を成功後だけ公開します。
- `SetObject` と `SetBonePalette` が `false` の描画は発行しません。定数バッファーの確保失敗や、対応する描画スロットがない状態を表します。
- `FSkinnedGpuMesh` と `CSkinnedShader` は GPU 資源を所有します。作成元の `IRhiDevice` より先に破棄します。
- `CAnimationPlayer` は CPU 側のメッシュを非所有参照します。再生器の参照を外してから最後の `TSharedPtr<ASkinnedMeshAsset>` を解放します。
