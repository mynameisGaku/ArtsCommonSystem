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
}

void ECSTestApp::Update()
{
	if (CheckHitKey(KEY_INPUT_Z))
	{
		auto e = m_pECS->CreateEntity();
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Position>(e, { 5, 0 });
		m_pECS->Add<Velocity>(e, { 5, 0 });
		m_pECS->Add<Velocity>(e, { 5, 0 });
		m_pECS->Add<Velocity>(e, { 5, 0 });
		m_pECS->Add<Velocity>(e, { 5, 0 });
		m_pECS->Add<Velocity>(e, { 5, 0 });
		m_pECS->Add<Velocity>(e, { 5, 0 });
		m_pECS->Add<Velocity>(e, { 5, 0 });
		m_pECS->Add<Velocity>(e, { 5, 0 });
		m_pECS->Add<Velocity>(e, { 5, 0 });
		m_pECS->Add<Velocity>(e, { 5, 0 });
		m_pECS->Add<Velocity>(e, { 5, 0 });
		m_pECS->Add<Velocity>(e, { 5, 0 });
		m_Entities.push_back(e);
		if (not m_IsPush)
		{
			m_IsPush = true;
		}
	}
	else
	{
		m_IsPush = false;
	}

	if (CheckHitKey(KEY_INPUT_X))
	{
		if (not m_Entities.empty())
		{
			auto e = (*m_Entities.begin());
			m_pECS->DeleteEntity(e);
			m_Entities.erase(m_Entities.begin());
		}
		if (not m_IsPush2)
		{
			m_IsPush2 = true;
		}
	}
	else
	{
		m_IsPush2 = false;
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
	ClearDrawScreen();

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