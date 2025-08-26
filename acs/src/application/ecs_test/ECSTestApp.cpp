#include "pch.h"
#include "ECSTestApp.h"

#include "Position.h"
#include "Velocity.h"
#include "MovementSystem.h"

ECSTestApp::ECSTestApp()
{
}

ECSTestApp::~ECSTestApp()
{
}

void ECSTestApp::OnStart()
{
	m_pECS = new seecs::ECS;

	HWND hwnd = GetMainWindowHandle();
	ACSM_Input::Initialize(hwnd);

	ACSM_Input::LoadVirtualKeysFromJson("data/json/config/input/input.json");
}

void ECSTestApp::Update()
{
	ACSM_Input::Update();

	if (ACSM_Input::GetVirtualKey("Jump"))
	{
		auto e = m_pECS->CreateEntity();
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Velocity>(e, { 5, 0 });
		m_Entities.push_back(e);
	}

	if (ACSM_Input::GetVirtualKey("Fire"))
	{
		if (not m_Entities.empty())
		{
			auto e = (*m_Entities.begin());
			m_pECS->DeleteEntity(e);
			m_Entities.erase(m_Entities.begin());
		}
	}

	m_pECS->View<Position, Velocity>().ForEach(
		[](seecs::EntityID id, Position& pos, const Velocity& vel)
		{
			pos.x += vel.x;
			pos.y += vel.y;
		});
}

void ECSTestApp::Draw()
{
	DrawString(10, 10, "ECS Test Running...", GetColor(255, 255, 255));

	if (m_pECS)
	{
		DrawFormatString(10, 30, GetColor(255, 255, 0), "Entities: %d", (int)m_pECS->GetEntityCount());
	}
}

void ECSTestApp::OnDestroy()
{
	delete m_pECS;
}