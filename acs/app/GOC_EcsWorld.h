#pragma once
#include <Pch.h>

class GOC_EcsWorld : public GO_Component
{
public:

	void Awake() override;
	void Start() override;
	void Update(float dt) override;
	void Draw(float dt) override;

private:
	entt::registry m_Registry;
	int image = -1;
};