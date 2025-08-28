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
		int posX, posY;
		
		GetMousePoint(&posX, &posY);
		auto e = ecs->CreateEntity();
		ecs->Add<Position>(e, { posX, posY });
		ecs->Add<Velocity>(e, { 5 + GetRand(5), 5 + GetRand(5) });
		ecs->Add<Color>(e, { GetColor(GetRand(255), GetRand(255), GetRand(255)) });
		ecs->Add<Scale>(e, { 4 + abs(GetRand(10)) });
	}
}

void ECSTestApp::OnStart()
{
	m_pECS = new seecs::ECS;

	ACSM_Input::Initialize();

	ACSM_Input::SubscribeVirtualKeyFromJson("data/json/config/input/input.json");

	m_pMovementSystem = new MovementSystem(m_pECS);
	m_pRenderingSystem = new RenderingSystem(m_pECS);
}

void ECSTestApp::Update()
{
	ACSM_Input::Update();
	m_pMovementSystem->Update();

	if (ACSM_Input::GetVirtualKeyDown("Jump") || 
		ACSM_Input::GetVirtualKeyDown("Attack"))
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

	if (ACSM_Input::GetVirtualKey("Jump"))
	{
		DrawString(10, 50, "Pressing Jump key", GetColor(255, 255, 255));
	}

	if (ACSM_Input::GetVirtualKey("Attack"))
	{
		DrawString(10, 80, "Pressing Attack key", GetColor(255, 255, 255));
	}

	DrawString(10, 10, "ECS Test Running...", GetColor(255, 255, 255));

	int posX, posY;

	GetMousePoint(&posX, &posY);
	DrawFormatString(10, 100, GetColor(255, 255, 0), "Mouse x:%d y:%d", posX, posY);
}

void ECSTestApp::OnDestroy()
{
	delete m_pMovementSystem;
	delete m_pRenderingSystem;
	m_pECS->Reset();
	delete m_pECS;
}