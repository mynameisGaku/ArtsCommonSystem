#include "SkyComponent.h"

static float Clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static float Length3(const ACSU_Math::Vector3& v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

static ACSU_Math::Vector3 NormalizeSafe(const ACSU_Math::Vector3& v)
{
    float len = Length3(v);
    if (len < 1e-6f)
    {
        return ACSU_Math::Vector3(0.0f, 1.0f, 0.0f);
    }
    return ACSU_Math::Vector3(v.x / len, v.y / len, v.z / len);
}

unsigned char SkyComponent::ToUNormByte(float v01)
{
    float t = Clamp01(v01);
    int b = static_cast<int>(t * 255.0f + 0.5f);
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return static_cast<unsigned char>(b);
}

unsigned char SkyComponent::ToSNormByte(float vNeg1To1)
{
    float t = vNeg1To1 * 0.5f + 0.5f;
    return ToUNormByte(t);
}

bool SkyComponent::Setup(const SetupParam& p)
{
    m_Param = p;

    if (m_Param.Slices < 8) m_Param.Slices = 8;
    if (m_Param.Stacks < 4) m_Param.Stacks = 4;
    if (m_Param.Radius < 10.0f) m_Param.Radius = 10.0f;

    return true;
}

void SkyComponent::Start()
{
    EnsureCreated();

    if (m_PSCBuffer == -1)
    {
        m_PSCBuffer = CreateShaderConstantBuffer(sizeof(SkyParamsCB));
        m_Time = 0.0f;

        void* p = GetBufferShaderConstantBuffer(m_PSCBuffer);
        if (p != nullptr)
        {
            std::memset(p, 0, sizeof(SkyParamsCB));
            UpdateShaderConstantBuffer(m_PSCBuffer);
        }
    }
}

void SkyComponent::EnsureCreated()
{
    if (m_Created)
    {
        return;
    }

    if (m_Param.PixelShaderPath == nullptr)
    {
        return;
    }

    m_PS = LoadPixelShader(const_cast<char*>(m_Param.PixelShaderPath));
    if (m_PS == -1)
    {
        return;
    }

    m_ParamSoftImage = MakeARGB8ColorSoftImage(4, 1);
    if (m_ParamSoftImage == -1)
    {
        return;
    }

    m_ParamGraph = CreateGraphFromSoftImage(m_ParamSoftImage);
    if (m_ParamGraph == -1)
    {
        return;
    }

    RebuildMesh();
    UpdateParamTextureIfNeeded();

    m_Created = true;
}

void SkyComponent::RebuildMesh()
{
    const int slices = m_Param.Slices;
    const int stacks = m_Param.Stacks;

    const int vtxW = slices + 1;
    const int vtxH = stacks + 1;

    m_UnitVertices.clear();
    m_DrawVertices.clear();
    m_Indices.clear();

    m_UnitVertices.resize(static_cast<size_t>(vtxW) * static_cast<size_t>(vtxH));
    m_DrawVertices.resize(m_UnitVertices.size());

    for (int y = 0; y < vtxH; ++y)
    {
        float v = static_cast<float>(y) / static_cast<float>(stacks);
        float theta = v * 3.1415926535f;

        float sinT = std::sin(theta);
        float cosT = std::cos(theta);

        for (int x = 0; x < vtxW; ++x)
        {
            float u = static_cast<float>(x) / static_cast<float>(slices);
            float phi = (u * 2.0f - 1.0f) * 3.1415926535f;

            float sinP = std::sin(phi);
            float cosP = std::cos(phi);

            float dx = cosP * sinT;
            float dy = cosT;
            float dz = sinP * sinT;

            VERTEX3DSHADER vv{};
            vv.pos = VGet(dx * m_Param.Radius, dy * m_Param.Radius, dz * m_Param.Radius);

            vv.norm = VGet(-dx, -dy, -dz);

            // viewDir を dif.rgb に詰める（snorm）
            unsigned char r = ToSNormByte(dx);
            unsigned char g = ToSNormByte(dy);
            unsigned char b = ToSNormByte(dz);
            vv.dif = GetColorU8(r, g, b, 255);

            vv.spc = GetColorU8(0, 0, 0, 0);

            // UV は今回使わない（内蔵VSが流さない可能性が高いので）
            vv.u = u;
            vv.v = v;
            vv.su = u;
            vv.sv = v;

            m_UnitVertices[static_cast<size_t>(y) * static_cast<size_t>(vtxW) + static_cast<size_t>(x)] = vv;
        }
    }

    for (int y = 0; y < stacks; ++y)
    {
        for (int x = 0; x < slices; ++x)
        {
            unsigned short i0 = static_cast<unsigned short>(y * vtxW + x);
            unsigned short i1 = static_cast<unsigned short>(y * vtxW + x + 1);
            unsigned short i2 = static_cast<unsigned short>((y + 1) * vtxW + x);
            unsigned short i3 = static_cast<unsigned short>((y + 1) * vtxW + x + 1);

            m_Indices.push_back(i0);
            m_Indices.push_back(i2);
            m_Indices.push_back(i1);

            m_Indices.push_back(i1);
            m_Indices.push_back(i2);
            m_Indices.push_back(i3);
        }
    }
}

void SkyComponent::Update(float deltaTime)
{
    if (!m_Created || m_PSCBuffer == -1)
    {
        return;
    }

    m_Time += deltaTime;

    GameObject* go = GetGameObject();
    if (go == nullptr)
    {
        return;
    }

    ACSU_Math::Vector3 sunDir(0.0f, 1.0f, 0.0f);
    float intensity = 1.0f;
    float turbidity = 0.4f;
    float exposure = 0.6f;

    go->GetBlackboard().Get(BlackboardKeys::SunDirection, sunDir);
    go->GetBlackboard().Get(BlackboardKeys::SunIntensity, intensity);
    go->GetBlackboard().Get(BlackboardKeys::Turbidity, turbidity);
    go->GetBlackboard().Get(BlackboardKeys::Exposure, exposure);

    sunDir = NormalizeSafe(sunDir);

    void* p = GetBufferShaderConstantBuffer(m_PSCBuffer);
    if (p == nullptr)
    {
        return;
    }

    SkyParamsCB* cb = static_cast<SkyParamsCB*>(p);
    cb->SunDirX = sunDir.x;
    cb->SunDirY = sunDir.y;
    cb->SunDirZ = sunDir.z;
    cb->Intensity = intensity;

    cb->Turbidity = turbidity;
    cb->Exposure = exposure;
    cb->Time = m_Time;
    cb->Padding = 0.0f;

    UpdateShaderConstantBuffer(m_PSCBuffer);
}

void SkyComponent::UpdateParamTextureIfNeeded()
{
    GameObject* go = GetGameObject();
    if (go == nullptr)
    {
        return;
    }

    Blackboard& bb = go->GetBlackboard();

    std::uint32_t vSun = bb.Version(BlackboardKeys::SunDirection);
    std::uint32_t vInt = bb.Version(BlackboardKeys::SunIntensity);
    std::uint32_t vTur = bb.Version(BlackboardKeys::Turbidity);
    std::uint32_t vExp = bb.Version(BlackboardKeys::Exposure);

    if (vSun == m_LastSunVer && vInt == m_LastIntVer && vTur == m_LastTurbVer && vExp == m_LastExpoVer)
    {
        return;
    }

    m_LastSunVer = vSun;
    m_LastIntVer = vInt;
    m_LastTurbVer = vTur;
    m_LastExpoVer = vExp;

    ACSU_Math::Vector3 sunDir(0.0f, 1.0f, 0.0f);
    float intensity = 1.0f;
    float turbidity01 = 0.5f;
    float exposure01 = 0.5f;

    bb.Get(BlackboardKeys::SunDirection, sunDir);
    bb.Get(BlackboardKeys::SunIntensity, intensity);
    bb.Get(BlackboardKeys::Turbidity, turbidity01);
    bb.Get(BlackboardKeys::Exposure, exposure01);

    sunDir = NormalizeSafe(sunDir);

    // ParamTex layout (4x1):
    // (0,0) sunDir.xyz in snorm, a unused
    // (1,0) intensity(0..1), turbidity(0..1), exposure(0..1), reserved
    // (2,0) reserved
    // (3,0) reserved

    DrawPixelSoftImage(m_ParamSoftImage, 0, 0,
        ToSNormByte(sunDir.x),
        ToSNormByte(sunDir.y),
        ToSNormByte(sunDir.z),
        255
    );

    DrawPixelSoftImage(m_ParamSoftImage, 1, 0,
        ToUNormByte(Clamp01(intensity)),
        ToUNormByte(Clamp01(turbidity01)),
        ToUNormByte(Clamp01(exposure01)),
        255
    );

    DrawPixelSoftImage(m_ParamSoftImage, 2, 0, 0, 0, 0, 255);
    DrawPixelSoftImage(m_ParamSoftImage, 3, 0, 0, 0, 0, 255);

    if (m_ParamGraph != -1)
    {
        DeleteGraph(m_ParamGraph);
        m_ParamGraph = -1;
    }

    m_ParamGraph = CreateGraphFromSoftImage(m_ParamSoftImage);
}

void SkyComponent::Draw(float)
{
    if (!m_Created || m_PS == -1 || m_PSCBuffer == -1)
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
        camTrs.position = ACSU_Math::Vector3::zero();
    }

    VECTOR camPos;
    camPos.x = camTrs.position.x;
    camPos.y = camTrs.position.y;
    camPos.z = camTrs.position.z;

    const size_t n = m_UnitVertices.size();
    for (size_t i = 0; i < n; ++i)
    {
        m_DrawVertices[i] = m_UnitVertices[i];
        m_DrawVertices[i].pos.x += camPos.x;
        m_DrawVertices[i].pos.y += camPos.y;
        m_DrawVertices[i].pos.z += camPos.z;
    }

    UpdateShaderConstantBuffer(m_PSCBuffer);

    // ここで m_DrawVertices をカメラ位置に追従させる処理は今まで通り

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

    SetUsePixelShader(m_PS);

    // PS の slot0 に定数バッファを入れる
    SetShaderConstantBuffer(m_PSCBuffer, DX_SHADERTYPE_PIXEL, 0);

    DrawPolygonIndexed3DToShader(
        m_DrawVertices.data(),
        static_cast<int>(m_DrawVertices.size()),
        m_Indices.data(),
        static_cast<int>(m_Indices.size() / 3)
    );

    // 後片付け
    SetShaderConstantBuffer(-1, DX_SHADERTYPE_PIXEL, 0);
    SetUsePixelShader(-1);

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

void SkyComponent::Destroy()
{
    if (m_PS != -1)
    {
        DeleteShader(m_PS);
        m_PS = -1;
    }

    if (m_ParamGraph != -1)
    {
        DeleteGraph(m_ParamGraph);
        m_ParamGraph = -1;
    }

    if (m_ParamSoftImage != -1)
    {
        DeleteSoftImage(m_ParamSoftImage);
        m_ParamSoftImage = -1;
    }

    if (m_PSCBuffer != -1)
    {
        DeleteShaderConstantBuffer(m_PSCBuffer);
        m_PSCBuffer = -1;
    }

    m_UnitVertices.clear();
    m_DrawVertices.clear();
    m_Indices.clear();

    m_Created = false;
}
