#include "GOC_Rotator.h"

GOC_Rotator::GOC_Rotator()
	: m_RotateSpeed(0.0f)
	, m_RotateRange(0.0f)
	, m_Timer(0.0f)
{
}

void GOC_Rotator::Setup(const SetupParameter& param)
{
	m_RotateSpeed = param.RotateSpeed;
	m_RotateRange = param.RotateRange;
	
	m_Timer = 0.0f;
}

void GOC_Rotator::Update(float dt)
{
	m_Timer += dt;

	float currentAngle = (m_RotateRange * std::sin(m_Timer * m_RotateSpeed));
	if (std::abs(m_RotateRange) > 0.001f)
	{
		currentAngle = std::sin(m_Timer * m_RotateSpeed) * m_RotateRange;
	}
	else
	{
		currentAngle = m_Timer * m_RotateSpeed;
	}
	auto* owner = GetGameObject();

	Transform* transform = owner->GetGOC<Transform>();
	ACSU_Math::Quaternion rotation = ACSU_Math::Quaternion::AngleAxis(currentAngle, ACSU_Math::Vector3::forward());
	transform->SetLocalRotation(rotation);
}