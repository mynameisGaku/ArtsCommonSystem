#include "SkyMeshComponent.h"

static float Clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

bool SkyMeshComponent::Setup(const SetupParam& p)
{
    m_Param = p;

    if (m_Param.TexW < 8) m_Param.TexW = 8;
    if (m_Param.TexH < 8) m_Param.TexH = 8;

    if (m_Param.Slices < 8) m_Param.Slices = 8;
    if (m_Param.Stacks < 4) m_Param.Stacks = 4;

    return true;
}

void SkyMeshComponent::Start()
{
    EnsureCreated();
}

void SkyMeshComponent::EnsureCreated()
{
    if (m_Created)
    {
        return;
    }

    m_SkySoftImage = MakeARGB8ColorSoftImage(m_Param.TexW, m_Param.TexH);
    if (m_SkySoftImage == -1)
    {
        return;
    }

    m_SkyTexGraph = CreateGraphFromSoftImage(m_SkySoftImage);
    if (m_SkyTexGraph == -1)
    {
        return;
    }

    m_Gen = new SkyTextureGenerator(m_Param.TexW, m_Param.TexH);

    RebuildMesh();
    UpdateSkyTexture();

    m_Created = true;
}

void SkyMeshComponent::RebuildMesh()
{
    const int slices = m_Param.Slices;
    const int stacks = m_Param.Stacks;

    const int vtxW = slices + 1;
    const int vtxH = stacks + 1;

    m_UnitVertices.clear();
    m_Indices.clear();

    m_UnitVertices.resize(static_cast<size_t>(vtxW) * static_cast<size_t>(vtxH));
    m_DrawVertices.resize(m_UnitVertices.size());

    // unit sphere vertices
    for (int y = 0; y < vtxH; ++y)
    {
        float v = static_cast<float>(y) / static_cast<float>(stacks); // 0..1
        float theta = v * 3.1415926535f; // 0..pi

        float sinT = std::sin(theta);
        float cosT = std::cos(theta);

        for (int x = 0; x < vtxW; ++x)
        {
            float u = static_cast<float>(x) / static_cast<float>(slices); // 0..1
            float phi = (u * 2.0f - 1.0f) * 3.1415926535f; // -pi..pi

            float sinP = std::sin(phi);
            float cosP = std::cos(phi);

            VECTOR dir;
            dir.x = cosP * sinT;
            dir.y = cosT;
            dir.z = sinP * sinT;

            VERTEX3D vv{};
            vv.pos = VGet(dir.x * m_Param.Radius, dir.y * m_Param.Radius, dir.z * m_Param.Radius);

            // 内側を描くので法線は内向きにしておく（ライティングOFFでも害はない）
            vv.norm = VGet(-dir.x, -dir.y, -dir.z);

            vv.dif = GetColorU8(255, 255, 255, 255);
            vv.spc = GetColorU8(0, 0, 0, 0);

            vv.u = u;
            vv.v = v;

            vv.su = 0.0f;
            vv.sv = 0.0f;

            m_UnitVertices[static_cast<size_t>(y) * static_cast<size_t>(vtxW) + static_cast<size_t>(x)] = vv;
        }
    }

    // indices (2 triangles per quad)
    for (int y = 0; y < stacks; ++y)
    {
        for (int x = 0; x < slices; ++x)
        {
            unsigned short i0 = static_cast<unsigned short>(y * vtxW + x);
            unsigned short i1 = static_cast<unsigned short>(y * vtxW + x + 1);
            unsigned short i2 = static_cast<unsigned short>((y + 1) * vtxW + x);
            unsigned short i3 = static_cast<unsigned short>((y + 1) * vtxW + x + 1);

            // windingはカリングOFF前提で気にしない
            m_Indices.push_back(i0);
            m_Indices.push_back(i2);
            m_Indices.push_back(i1);

            m_Indices.push_back(i1);
            m_Indices.push_back(i2);
            m_Indices.push_back(i3);
        }
    }
}

void SkyMeshComponent::Update(float)
{
    if (!m_Created)
    {
        return;
    }

    GameObject* go = GetGameObject();
    if (go == nullptr)
    {
        return;
    }

    std::uint32_t ver = go->GetBlackboard().Version(BlackboardKeys::SunDirection);
    if (ver != m_LastSunVersion)
    {
        UpdateSkyTexture();
        m_LastSunVersion = ver;
    }
}

void SkyMeshComponent::UpdateSkyTexture()
{
    if (m_Gen == nullptr || m_SkySoftImage == -1)
    {
        return;
    }

    GameObject* go = GetGameObject();
    if (go == nullptr)
    {
        return;
    }

    ACSU_Math::Vec3 sunDir(0.0f, 1.0f, 0.0f);
    float intensity = 1.0f;

    go->GetBlackboard().Get(BlackboardKeys::SunDirection, sunDir);
    go->GetBlackboard().Get(BlackboardKeys::SunIntensity, intensity);

    m_Gen->Render(sunDir, intensity, m_GenParams);

    const std::uint8_t* px = m_Gen->PixelsRGBA();
    int w = m_Gen->Width();
    int h = m_Gen->Height();

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;

            int r = px[idx + 0];
            int g = px[idx + 1];
            int b = px[idx + 2];
            int a = px[idx + 3];

            DrawPixelSoftImage(m_SkySoftImage, x, y, r, g, b, a);
        }
    }

    if (m_SkyTexGraph != -1)
    {
        DeleteGraph(m_SkyTexGraph);
        m_SkyTexGraph = -1;
    }

    m_SkyTexGraph = CreateGraphFromSoftImage(m_SkySoftImage);
}

void SkyMeshComponent::Draw(float)
{
    if (!m_Created || m_SkyTexGraph == -1)
    {
        return;
    }

    GameObject* go = GetGameObject();
    if (go == nullptr)
    {
        return;
    }

    TransformTRS camTrs;
    if (!go->GetBlackboard().Get(BlackboardKeys::TransformWorldTRS, camTrs))
    {
        camTrs.position = ACSU_Math::Vec3::Zero();
    }

    VECTOR camPos;
    camPos.x = camTrs.position.x;
    camPos.y = camTrs.position.y;
    camPos.z = camTrs.position.z;

    // カメラ位置へ追従（頂点のposだけ加算）
    const size_t n = m_UnitVertices.size();
    for (size_t i = 0; i < n; ++i)
    {
        m_DrawVertices[i] = m_UnitVertices[i];
        m_DrawVertices[i].pos.x += camPos.x;
        m_DrawVertices[i].pos.y += camPos.y;
        m_DrawVertices[i].pos.z += camPos.z;
    }

    const int oldCull = GetUseBackCulling();
    SetUseBackCulling(FALSE);

    if (m_Param.DisableLighting)
    {
        SetUseLighting(FALSE);
    }

    if (m_Param.DisableZBuffer)
    {
        SetUseZBuffer3D(FALSE);
        SetWriteZBuffer3D(FALSE);
    }

    // DrawPolygonIndexed3D は VERTEX3D とテクスチャで描ける :contentReference[oaicite:2]{index=2}
    // テクスチャには制限あり（2のn乗など） :contentReference[oaicite:3]{index=3}
    DrawPolygonIndexed3D(
        m_DrawVertices.data(),
        static_cast<int>(m_DrawVertices.size()),
        m_Indices.data(),
        static_cast<int>(m_Indices.size() / 3),
        m_SkyTexGraph,
        FALSE
    );

    if (m_Param.DisableZBuffer)
    {
        SetUseZBuffer3D(TRUE);
        SetWriteZBuffer3D(TRUE);
    }

    if (m_Param.DisableLighting)
    {
        SetUseLighting(TRUE);
    }

    SetUseBackCulling(oldCull);
}

void SkyMeshComponent::Destroy()
{
    if (m_SkyTexGraph != -1)
    {
        DeleteGraph(m_SkyTexGraph);
        m_SkyTexGraph = -1;
    }

    if (m_SkySoftImage != -1)
    {
        DeleteSoftImage(m_SkySoftImage);
        m_SkySoftImage = -1;
    }

    if (m_Gen != nullptr)
    {
        delete m_Gen;
        m_Gen = nullptr;
    }

    m_UnitVertices.clear();
    m_DrawVertices.clear();
    m_Indices.clear();

    m_Created = false;
}
