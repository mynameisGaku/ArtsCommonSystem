#pragma once
#include <Pch.h>

class GOC_Rotator : public GO_Component
{
public:
	struct SetupParameter
	{
		float RotateSpeed;        // ‰ñ“]‘¬“x
		float RotateRange;        // ‰ñ“]‚ÌU•
	};

	GOC_Rotator();

	void Setup(const SetupParameter& param);

	void Update(float dt) override;

private:
	float m_RotateSpeed;
	float m_RotateRange;

	float m_Timer;
};