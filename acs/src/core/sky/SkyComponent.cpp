#include "SkyComponent.h"

bool SkyComponent::Setup(const SetupParam& p)
{
    m_Param = p;
    return true;
}

void SkyComponent::Start()
{
    EnsureLoaded();
}

void SkyComponent::EnsureLoaded()
{
    if (m_Loaded)
    {
        return;
    }

    if (m_Param.SkyDomeModelPath == nullptr)
    {
        return;
    }

    m_Model = MV1LoadModel(m_Param.SkyDomeModelPath);
    if (m_Model == -1)
    {
        return;
    }

    // ソフトウェアイメージ作成（RGBA8）
    m_SkySoftImage = MakeARGB8ColorSoftImage(m_Param.TexW, m_Param.TexH);
    if (m_SkySoftImage == -1)
    {
        return;
    }

    // 初回グラフ作成（まだ中身は後で入れる）
    m_SkyTexGraph = CreateGraphFromSoftImage(m_SkySoftImage);
    if (m_SkyTexGraph == -1)
    {
        return;
    }

    // モデル側の差し替え先テクスチャ番号を探す
    m_TexIndex = -1;
    int matNum = MV1GetMaterialNum(m_Model);
    for (int mi = 0; mi < matNum; ++mi)
    {
        int t = MV1GetMaterialDifMapTexture(m_Model, mi);
        if (t >= 0)
        {
            m_TexIndex = t;
            break;
        }
    }

    // Diffuseが無いモデルへのフォールバック（テクスチャ0番）
    if (m_TexIndex < 0)
    {
        int texNum = MV1GetTextureNum(m_Model);
        if (texNum > 0)
        {
            m_TexIndex = 0;
        }
    }

    if (m_TexIndex >= 0)
    {
        MV1SetTextureGraphHandle(m_Model, m_TexIndex, m_SkyTexGraph, FALSE);
    }

    UpdateSkyTexture();


    m_Loaded = true;
}

void SkyComponent::Update(float)
{
    if (!m_Loaded)
    {
        return;
    }

    // 太陽の値が更新されたらテクスチャ更新
    auto* go = GetGameObject();
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

static VECTOR ToDxVec(const ACSU_Math::Vec3& v)
{
    VECTOR r;
    r.x = v.x;
    r.y = v.y;
    r.z = v.z;
    return r;
}

void SkyComponent::UpdateSkyTexture()
{
    if (m_Gen == nullptr || m_SkySoftImage == -1)
    {
        return;
    }

    auto* go = GetGameObject();
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

    // グラフを作り直す（小さいテクスチャ前提なら許容）
    if (m_SkyTexGraph != -1)
    {
        DeleteGraph(m_SkyTexGraph);
        m_SkyTexGraph = -1;
    }

    m_SkyTexGraph = CreateGraphFromSoftImage(m_SkySoftImage);

    if (m_TexIndex >= 0 && m_SkyTexGraph != -1)
    {
        MV1SetTextureGraphHandle(m_Model, m_TexIndex, m_SkyTexGraph, FALSE);
    }
}

void SkyComponent::Draw(float)
{
    if (!m_Loaded)
    {
        return;
    }

    auto* go = GetGameObject();
    if (go == nullptr)
    {
        return;
    }

    // カメラ位置に追従させる（CameraObjectに付ける想定）
    TransformTRS camTrs;
    if (!go->GetBlackboard().Get(BlackboardKeys::TransformWorldTRS, camTrs))
    {
        camTrs.position = ACSU_Math::Vec3::Zero();
    }

    VECTOR camPos = ToDxVec(camTrs.position);

    MV1SetPosition(m_Model, camPos);
    MV1SetScale(m_Model, VGet(m_Param.DomeScale, m_Param.DomeScale, m_Param.DomeScale));

    // 内側を描く必要がある場合はカリングを切る（モデルの面向き次第）
    SetUseBackCulling(FALSE);

    MV1DrawModel(m_Model);

    SetUseBackCulling(TRUE);
}

void SkyComponent::Destroy()
{
    if (m_Model != -1)
    {
        MV1DeleteModel(m_Model);
        m_Model = -1;
    }

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

    m_Loaded = false;
}
