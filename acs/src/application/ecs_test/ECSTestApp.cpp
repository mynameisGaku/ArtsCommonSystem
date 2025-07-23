#include "ECSTestApp.h"
#include <DxLib.h>

#include <modules/ecs/ECSWorld.h>
#include "Position.h"
#include "Velocity.h"
#include "MovementSystem.h"
#include <memory>

ECSTestApp::ECSTestApp()
{
}

ECSTestApp::~ECSTestApp()
{
}

void ECSTestApp::Start()
{
    m_pECSWorld = new ECSWorld;

    EntityID e1 = m_pECSWorld->CreateEntity();
    m_pECSWorld->AddComponents(e1, Position{ 0, 0 }, Velocity{ 1, 2 });

    // MovementSystem のインスタンスを登録
    m_pECSWorld->RegisterSystem(std::make_shared<MovementSystem>(m_pECSWorld));
}

void ECSTestApp::Update()
{
    if (CheckHitKey(KEY_INPUT_Z))
    {
        if (not m_IsPush)
        {
            m_IsPush = true;

            EntityID e1 = m_pECSWorld->CreateEntity();
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

void ECSTestApp::Destroy()
{
    delete m_pECSWorld;
}