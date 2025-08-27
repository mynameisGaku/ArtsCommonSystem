#include "pch.h"
#include "ECSTestApp.h"

#include "Position.h"
#include "Velocity.h"
#include "Color.h"
#include "Scale.h"
#include "MovementSystem.h"
#include "RenderingSystem.h"

ECSTestApp::ECSTestApp()
{
	m_pECS = nullptr;
	m_pMovementSystem = nullptr;
	m_pRenderingSystem = nullptr;
}

ECSTestApp::~ECSTestApp()
{
	ACSM_Input::Finalize();
}

void GenerateBall(seecs::ECS* ecs, int amount)
{
	for (int i = 0; i < amount; i++)
	{
		auto e = ecs->CreateEntity();
		ecs->Add<Position>(e, { 5 + GetRand(5), 5 + GetRand(5) });
		ecs->Add<Velocity>(e, { 5 + GetRand(5), 5 + GetRand(5) });
		ecs->Add<Color>(e, { GetColor(GetRand(255), GetRand(255), GetRand(255)) });
		ecs->Add<Scale>(e, { 4 + abs(GetRand(10)) });
	}
}

void ECSTestApp::OnStart()
{
	m_pECS = new seecs::ECS;

	ACSM_Input::Initialize();

	for (int i = 0; i < 4; i++)
	{
	}

	m_pMovementSystem = new MovementSystem(m_pECS);
	m_pRenderingSystem = new RenderingSystem(m_pECS);
}

void ECSTestApp::Update()
{
	ACSM_Input::Update();
	m_pMovementSystem->Update();

	if (ACSM_Input::GetKey(KeyCode::Z))
	{
		GenerateBall(m_pECS, 1);
	}
}

void ECSTestApp::Draw()
{
	if (m_pECS)
	{
		auto drawfunc = [](Position pos, unsigned int color, int radius) {
				DrawCircle((int)pos.x, (int)pos.y, radius, color, TRUE);
			};

		m_pRenderingSystem->Update(drawfunc);

		DrawFormatString(10, 30, GetColor(255, 255, 0), "Entities: %d", (int)m_pECS->GetEntityCount());
	}

	if (ACSM_Input::GetKey(KeyCode::Z))
	{
		DrawString(10, 50, "Pressing Z key", GetColor(255, 255, 255));
	}

	DrawString(10, 10, "ECS Test Running...", GetColor(255, 255, 255));
}

void ECSTestApp::OnDestroy()
{
	delete m_pECS;
}