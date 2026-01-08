#include "SpriteComponent.h"

static float ExtractZRadiansFromQuat(const ACSU_Math::Quat& qIn)
{
    ACSU_Math::Quat q = qIn.Normalized();

    float siny_cosp = 2.0f * (q.w * q.z + q.x * q.y);
    float cosy_cosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);

    return std::atan2(siny_cosp, cosy_cosp);
}

void SpriteComponent::Setup(const SetupParam& p)
{
    m_Param = p;
}

void SpriteComponent::EnsureLoaded()
{
    if (m_Handle != -1)
    {
        return;
    }

    if (m_Param.Path == nullptr)
    {
        return;
    }

    int total = m_Param.DivX * m_Param.DivY;
    if (total <= 0)
    {
        return;
    }

    // DxLib の LoadDivGraph は配列が必要
    // ここでは total が小さい想定で動的確保（スマポ無し）
    int* handles = new int[total];

    int ok = LoadDivGraph(
        m_Param.Path,
        total,
        m_Param.DivX,
        m_Param.DivY,
        m_Param.CellW,
        m_Param.CellH,
        handles
    );

    if (ok == 0)
    {
        // 先頭を基準に連番として扱う実装にしている場合があるので、
        // ここは「配列を保持する」方が安全。
        // いったん最小のため、先頭だけ保持し、配列は破棄する。
        m_Handle = handles[0];
    }

    delete[] handles;
}

void SpriteComponent::Update(float dt)
{
    int total = m_Param.DivX * m_Param.DivY;
    if (total <= 1)
    {
        return;
    }

    m_Timer += dt;
    if (m_Timer >= m_Param.FrameTime)
    {
        m_Timer = 0.0f;
        ++m_Frame;

        if (m_Frame >= total)
        {
            m_Frame = m_Param.Loop ? 0 : total - 1;
        }
    }
}

void SpriteComponent::Draw(float)
{
    EnsureLoaded();

    if (m_Handle == -1)
    {
        return;
    }

    GameObject* go = GetGameObject();
    if (go == nullptr)
    {
        return;
    }

    TransformTRS trs;
    if (!go->GetBlackboard().Get(BlackboardKeys::TransformWorldTRS, trs))
    {
        return;
    }

    float x = trs.position.x;
    float y = trs.position.y;

    float angle = ExtractZRadiansFromQuat(trs.rotation);

    float cx = static_cast<float>(m_Param.CellW) * 0.5f;
    float cy = static_cast<float>(m_Param.CellH) * 0.5f;

    float sx = trs.scale.x;
    float sy = trs.scale.y;

    // 非等方スケール反映（使えない環境ならここがコンパイルで落ちるので、2Fに差し替え）
    DrawRotaGraph3F(
        x,
        y,
        cx,
        cy,
        sx,
        sy,
        angle,
        m_Handle + m_Frame,
        TRUE,
        FALSE
    );
}

void SpriteComponent::Destroy()
{
    if (m_Handle != -1)
    {
        DeleteGraph(m_Handle);
        m_Handle = -1;
    }
}
