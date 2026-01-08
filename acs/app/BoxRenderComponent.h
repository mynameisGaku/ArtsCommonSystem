#pragma once

#include <Pch.h>

class BoxRenderComponent : public Component
{
public:
	BoxRenderComponent(const ACSU_Math::Vec3& position, float size);
	void Draw(float deltaTime) override;
private:
	ACSU_Math::Vec3 m_position;
	float m_size;
};