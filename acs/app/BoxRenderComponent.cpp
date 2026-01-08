#include "Pch.h"
#include "BoxRenderComponent.h"

BoxRenderComponent::BoxRenderComponent(const ACSU_Math::Vector3& position, float size)
{
	m_position = position;
	m_size = size;
}

void BoxRenderComponent::Draw(float deltaTime)
{
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

	DxLib::DrawBoxAA(
		x,
		y,
		x + m_size,
		y + m_size,
		DxLib::GetColor(255, 0, 0),
		DxLib::GetColor(0, 0, 255),
		true
	);
}
