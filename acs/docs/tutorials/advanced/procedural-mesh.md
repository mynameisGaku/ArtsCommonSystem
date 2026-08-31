# 手続き生成メッシュ

ACSはファイルを読み込まずに [`AMeshAsset`](../../reference/symbols/asset/ameshasset-c3a8777c.html) を生成できます。`Primitive` 名前空間の関数はCPU側の頂点、法線、UV、インデックスを作り、[`UploadMesh`](../../reference/symbols/render/uploadmesh-6726a6de.html) が描画用の [`FGpuMesh`](../../reference/symbols/render/fgpumesh-8d5c5148.html) へ転送します。

## 形状を生成して描画する

次の型は XZ 平面を作り、[`CStandardShader`](../../reference/symbols/render/cstandardshader-8477aafd.html) で描画します。

```cpp
#include "asset/MeshPrimitive.h"
#include "foundation/Log.h"
#include "foundation/Move.h"
#include "render/RenderAssets.h"
#include "render/Renderer.h"
#include "render/StandardShader.h"

class FProceduralMeshRenderer
{
public:
    bool Initialize(acs::CRenderer& renderer) noexcept
    {
        acs::IRhiDevice* device = renderer.Device();
        if (!device)
            return false;

        acs::TSharedPtr<acs::AMeshAsset> cpu_mesh = acs::Primitive::MakePlane(20.0f, 20.0f);
        if (!cpu_mesh)
            return false;

        acs::FGpuMesh staged_mesh{};
        auto upload = acs::UploadMesh(*device, *cpu_mesh, staged_mesh);
        if (upload.IsErr())
        {
            ACS_LOG_ERROR("procedural mesh を転送できません: %s", upload.Error().message);
            return false;
        }

        auto shader = m_Shader.Init(*device, renderer.ColorFormat(), renderer.DepthFormat());
        if (shader.IsErr())
        {
            ACS_LOG_ERROR("標準 mesh shader を初期化できません: %s", shader.Error().message);
            m_Shader.Shutdown();
            return false;
        }

        m_Mesh = acs::Move(staged_mesh);
        return true;
    }

    void Draw(acs::CRenderer& renderer, const acs::FMat4& view_projection, acs::FVec3 camera_position) noexcept
    {
        acs::IRhiCommandList* command = renderer.CommandList();
        if (!command || !m_Shader.BeginFrame(1u))
            return;

        m_Shader.SetFrame(view_projection, camera_position, acs::Normalize(acs::FVec3{-0.4f, -1.0f, 0.2f}), acs::FVec3{1.0f, 0.95f, 0.85f}, acs::FVec3{0.08f, 0.10f, 0.14f});
        m_Shader.DrawMesh(*command, m_Mesh, acs::FMat4::Identity(), acs::FVec3{0.22f, 0.65f, 0.30f});
    }

    void Shutdown() noexcept
    {
        m_Mesh = acs::FGpuMesh{};
        m_Shader.Shutdown();
    }

private:
    acs::FGpuMesh m_Mesh;
    acs::CStandardShader m_Shader;
};
```

`Draw` は `CRenderer::BeginFrame` 後の描画区間で呼びます。[`CStandardShader::BeginFrame`](../../reference/symbols/render/cstandardshader-8477aafd/beginframe-331cfd2c.html) には、そのフレームで予定するメッシュ描画数を渡します。[`DrawMesh`](../../reference/symbols/render/cstandardshader-8477aafd/drawmesh-1ab1b568.html) はインデックス付きメッシュを描くため、`vertex_buffer` と `index_buffer` の両方が必要です。

## 利用できる生成関数

| 関数 | 生成内容 | 入力の扱い |
|---|---|---|
| [`MakeCube`](../../reference/symbols/asset/makecube-d3ec2240.html) | 原点中心の立方体。24頂点、36インデックス | `size` は一辺の長さ |
| [`MakeSphere`](../../reference/symbols/asset/makesphere-ae5de654.html) | UV球 | `segments` は最小3、`rings` は最小2へ補正 |
| [`MakePlane`](../../reference/symbols/asset/makeplane-1321368f.html) | Y=0、法線 +Y の XZ 平面 | `width` と `depth` を指定 |

各関数は生成したCPUメッシュを `TSharedPtr<AMeshAsset>` で返します。空ポインターは生成失敗です。入力がNaNまたは無限大の場合、要素数を表現できない場合、または領域確保に失敗した場合は空を返します。

## 所有権

- `AMeshAsset` はCPU側の頂点、インデックス、サブメッシュを所有します。
- `UploadMesh` は内容をGPUバッファへ複製します。転送成功後、CPUメッシュが不要なら `TSharedPtr` を解放できます。
- `FGpuMesh` の [`vertex_buffer`](../../reference/symbols/render/fgpumesh-8d5c5148/vertex-buffer-26478c4a.html) と [`index_buffer`](../../reference/symbols/render/fgpumesh-8d5c5148/index-buffer-ac3cfb96.html) は `TUniquePtr` であり、`FGpuMesh` が単独所有します。
- GPUバッファとシェーダーは、それらを作成した `IRhiDevice` より先に破棄します。`CApplication` を再実行する構成では、`OnShutdown` から `Shutdown()` を呼びます。

## 更新と失敗時の公開

`UploadMesh` は空の頂点配列をエラーにします。頂点バッファの作成後にインデックスバッファの作成が失敗した場合、出力先には頂点バッファだけが入る可能性があります。公開中のメッシュを直接出力先にせず、例のように一時 `FGpuMesh` へ転送し、成功した場合だけ `Move` します。

CPUメッシュの頂点やインデックスを変更して再転送する場合も、新しい `FGpuMesh` を一時領域で完成させてから交換します。[`AMeshAsset::Vertices`](../../reference/symbols/asset/ameshasset-c3a8777c/vertices-9ef917d8.html)、[`Indices`](../../reference/symbols/asset/ameshasset-c3a8777c/indices-1a8f0721.html)、[`SubMeshes`](../../reference/symbols/asset/ameshasset-c3a8777c/submeshes-df7ba6c3.html) の可変アクセサーは形状リビジョンを更新しますが、既存GPUバッファを自動更新しません。
