#include "GOC_Sprite.h"

static float ExtractZRadiansFromQuat(const ACSU_Math::Quaternion& qIn)
{
    ACSU_Math::Quaternion q = qIn.normalized();

    float siny_cosp = 2.0f * (q.w * q.z + q.x * q.y);
    float cosy_cosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);

    return std::atan2(siny_cosp, cosy_cosp);
}

void GOC_Sprite::Setup(const SetupParam& p)
{
    m_Param = p;
}

void GOC_Sprite::EnsureLoaded()
{
    if (m_Param.Path == nullptr)
    {
        return;
    }

    if (m_Handles.size() > 0)
    {
        return;
    }

    int total = m_Param.DivX * m_Param.DivY;
    if (total <= 0)
    {
        return;
    }

    std::vector<int> temp(total, 0);

    int ok = ACS_LoadDivGraph(
        m_Param.Path,
        total,
        m_Param.DivX,
        m_Param.DivY,
        m_Param.CellW,
        m_Param.CellH,
        temp.data()
    );

    if (ok == 0)
    {
		m_Handles = std::move(temp);
    }
}

void GOC_Sprite::Update(float dt)
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

void GOC_Sprite::Draw(float)
{
    EnsureLoaded();

    if (m_Handles.empty())
    {
        return;
    }

    GameObject* go = GetGameObject();
    if (go == nullptr)
    {
        return;
    }

    Transform* transform = go->GetGOC<Transform>();
    if (transform == nullptr) return;

    float x = transform->GetWorldPosition().x;
    float y = transform->GetWorldPosition().y;

    float angle = ExtractZRadiansFromQuat(transform->GetLocalRotation());

    float cx = static_cast<float>(m_Param.CellW) * 0.5f;
    float cy = static_cast<float>(m_Param.CellH) * 0.5f;

    float sx = transform->GetLocalScale().x;
    float sy = transform->GetLocalScale().y;

    DrawRotaGraph3F(
        x,
        y,
        cx,
        cy,
        sx,
        sy,
        angle,
        m_Handles[m_Frame],
        TRUE,
        FALSE
    );
}

void GOC_Sprite::Destroy()
{
}
