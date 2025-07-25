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
    m_pECSWorld = new ACSM_ECS_ECSWorld;
}

void ECSTestApp::Update()
{
    if (CheckHitKey(KEY_INPUT_Z))
    {
        if (not m_IsPush)
        {
            m_IsPush = true;

            ACSM_ECS_EntityID e1 = m_pECSWorld->CreateEntity();
            m_pECSWorld->AddComponents(e1, Position{ 0, 0 }, Velocity{ 1, 2 });

            // MovementSystem のインスタンスを登録
            m_pECSWorld->RegisterSystem(std::make_shared<MovementSystem>(m_pECSWorld));
        }
    }
    else
    {
        m_IsPush = false;
    }

    m_pECSWorld->Update();
}

void ECSTestApp::Draw()
{
    ClearDrawScreen();

    DrawString(10, 10, "ECS Test Running...", GetColor(255, 255, 255));

    if (m_pECSWorld)
    {
        int entityCount = m_pECSWorld->GetEntityCount();  // 仮メソッド
        DrawFormatString(10, 30, GetColor(255, 255, 0), "Entities: %d", entityCount);
    }
}

void ECSTestApp::OnDestroy()
{
    delete m_pECSWorld;
}