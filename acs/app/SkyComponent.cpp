#include "SkyComponent.h"

bool SkyComponent::Setup(const SetupParam& p)
{
    m_Param = p;
    if (m_Param.Slices < 8) m_Param.Slices = 8;
    if (m_Param.Stacks < 4) m_Param.Stacks = 4;
    return true;
}

void SkyComponent::Start()
{
    EnsureCreated();
}

void SkyComponent::EnsureCreated()
{
    if (m_Created) return;
    if (m_Param.PixelShaderPath == nullptr) return;

    // ピクセルシェーダー読み込み
    m_PS = LoadPixelShader(const_cast<char*>(m_Param.PixelShaderPath));
    if (m_PS == -1) return;

    // 定数バッファ作成
    m_PSCBuffer = CreateShaderConstantBuffer(sizeof(SkyParamsCB));

    RebuildMesh();
    m_Created = true;
}

void SkyComponent::RebuildMesh()
{
    m_UnitVertices.clear();
    m_Indices.clear();

    int slices = m_Param.Slices;
    int stacks = m_Param.Stacks;

    // 球体メッシュ作成（UVが重要です）
    for (int j = 0; j <= stacks; ++j)
    {
        float v = (float)j / (float)stacks;
        float phi = v * 3.14159265f;

        for (int i = 0; i <= slices; ++i)
        {
            float u = (float)i / (float)slices;
            float theta = u * 2.0f * 3.14159265f;

            float y = std::cos(phi);
            float r = std::sin(phi);
            float x = r * std::cos(theta);
            float z = r * std::sin(theta);

            VERTEX3DSHADER vert;
            vert.pos = VGet(x, y, z);
            vert.norm = VGet(x, y, z);
            vert.dif = GetColorU8(255, 255, 255, 255);
            vert.spc = GetColorU8(0, 0, 0, 0);

            // ★このUV(u,v)を使ってシェーダーで座標復元します
            vert.u = u;
            vert.v = v;
            vert.su = 0; vert.sv = 0;

            m_UnitVertices.push_back(vert);
        }
    }

    // インデックス生成
    for (int j = 0; j < stacks; ++j)
    {
        for (int i = 0; i < slices; ++i)
        {
            int p0 = j * (slices + 1) + i;
            int p1 = p0 + 1;
            int p2 = (j + 1) * (slices + 1) + i;
            int p3 = p2 + 1;

            // 逆面カリング対策のため、順序に注意（ここではカリングOFFで描くのでどちらでも映ります）
            m_Indices.push_back(static_cast<unsigned short>(p0));
            m_Indices.push_back(static_cast<unsigned short>(p1));
            m_Indices.push_back(static_cast<unsigned short>(p2));

            m_Indices.push_back(static_cast<unsigned short>(p2));
            m_Indices.push_back(static_cast<unsigned short>(p1));
            m_Indices.push_back(static_cast<unsigned short>(p3));
        }
    }
    m_DrawVertices = m_UnitVertices;
}

void SkyComponent::Update(float deltaTime)
{
    if (!m_Created || m_PSCBuffer == -1) return;
    m_Time += deltaTime;;
    GameObject* go = GetGameObject();
    if (!go) return;
    Blackboard& bb = go->GetBlackboard();

    // パラメータ取得
    ACSU_Math::Vector3 sunDir(0, 1, 0);
    ACSU_Math::Vector3 skyTint(0.5f, 0.5f, 0.5f);
    ACSU_Math::Vector3 groundColor(0.3f, 0.3f, 0.3f);
    ACSU_Math::Vector3 horizonColor(0.8f, 0.8f, 0.8f);
    float sunSize = 0.04f;
    float sunConvergence = 5.0f;
    float atmosphereThick = 1.0f;
    float exposure = 1.0f;
    float cloudDensity = 0.5f;
    float cloudSharpness = 0.1f;

    bb.Get(BlackboardKeys::SunDirection, sunDir);
    bb.Get(BlackboardKeys::SkyTint, skyTint);
    bb.Get(BlackboardKeys::GroundColor, groundColor);
    bb.Get(BlackboardKeys::HorizonColor, horizonColor);
    bb.Get(BlackboardKeys::SunSize, sunSize);
    bb.Get(BlackboardKeys::SunConvergence, sunConvergence);
    bb.Get(BlackboardKeys::AtmosphereThick, atmosphereThick);
    bb.Get(BlackboardKeys::Exposure, exposure);
    bb.Get(BlackboardKeys::Turbidity, cloudDensity);

    // バッファ転送
    void* p = GetBufferShaderConstantBuffer(m_PSCBuffer);
    if (p != nullptr)
    {
        SkyParamsCB* cb = static_cast<SkyParamsCB*>(p);

        // ベクトル正規化などの安全策
        float len = std::sqrt(sunDir.x * sunDir.x + sunDir.y * sunDir.y + sunDir.z * sunDir.z);
        if (len > 0.0001f) { sunDir.x /= len; sunDir.y /= len; sunDir.z /= len; }

        cb->SunDirX = sunDir.x; cb->SunDirY = sunDir.y; cb->SunDirZ = sunDir.z;
        cb->SunSize = sunSize;
        cb->SkyTintR = skyTint.x; cb->SkyTintG = skyTint.y; cb->SkyTintB = skyTint.z;
        cb->AtmosphereThick = atmosphereThick;
        cb->GroundColorR = groundColor.x; cb->GroundColorG = groundColor.y; cb->GroundColorB = groundColor.z;
        cb->Exposure = exposure;
        cb->HorizonColorR = horizonColor.x; cb->HorizonColorG = horizonColor.y; cb->HorizonColorB = horizonColor.z;
        cb->SunConvergence = sunConvergence;
        cb->Time = m_Time;
        cb->CloudDensity = cloudDensity;
        cb->CloudSharpness = cloudSharpness;

        UpdateShaderConstantBuffer(m_PSCBuffer);
    }
}

void SkyComponent::Draw(float dt)
{
    if (!m_Created || m_PS == -1 || m_DrawVertices.empty()) return;

    GameObject* go = GetGameObject();
    if (!go) return;

    // カメラ位置へ移動（回転はさせない＝空は常にワールド軸に固定）
    TransformTRS camTrs;
    ACSU_Math::Vector3 camPos(0, 0, 0);
    if (go->GetBlackboard().Get(BlackboardKeys::TransformWorldTRS, camTrs))
    {
        camPos = camTrs.position;
    }

    float rad = m_Param.Radius;
    size_t count = m_UnitVertices.size();
    for (size_t i = 0; i < count; ++i)
    {
        m_DrawVertices[i].pos.x = m_UnitVertices[i].pos.x * rad + camPos.x;
        m_DrawVertices[i].pos.y = m_UnitVertices[i].pos.y * rad + camPos.y;
        m_DrawVertices[i].pos.z = m_UnitVertices[i].pos.z * rad + camPos.z;
    }

    int oldCull = GetUseBackCulling();
    SetUseBackCulling(FALSE);

    if (m_Param.DisableLighting) SetUseLighting(FALSE);
    if (m_Param.DisableZBuffer) { SetUseZBuffer3D(FALSE); SetWriteZBuffer3D(FALSE); }

    SetUsePixelShader(m_PS);
    SetShaderConstantBuffer(m_PSCBuffer, DX_SHADERTYPE_PIXEL, 0);

    // ★重要: ここで描画するとき、DxLibはデフォルトの頂点シェーダーを使います
    // デフォルトVSは UV座標(texcoord) をそのままPSに渡してくれるので、
    // SkyPS側でそれを受け取って計算に使います。
    DrawPolygonIndexed3DToShader(
        m_DrawVertices.data(),
        static_cast<int>(m_DrawVertices.size()),
        m_Indices.data(),
        static_cast<int>(m_Indices.size() / 3)
    );

    SetShaderConstantBuffer(-1, DX_SHADERTYPE_PIXEL, 0);
    SetUsePixelShader(-1);

    if (m_Param.DisableZBuffer) { SetUseZBuffer3D(TRUE); SetWriteZBuffer3D(TRUE); }
    if (m_Param.DisableLighting) SetUseLighting(TRUE);
    SetUseBackCulling(oldCull);
}

void SkyComponent::Destroy()
{
    if (m_PS != -1) { DeleteShader(m_PS); m_PS = -1; }
    if (m_PSCBuffer != -1) { DeleteShaderConstantBuffer(m_PSCBuffer); m_PSCBuffer = -1; }
    m_UnitVertices.clear();
    m_DrawVertices.clear();
    m_Indices.clear();
    m_Created = false;
}