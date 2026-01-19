#pragma once
#include <Pch.h>

class GOC_Mesh : public GO_Component
{
public:
	void Setup(const std::string& meshPath);
	void Draw(float dt) override;
private:
	int m_hModel = -1;
};