#include "GOC_EcsWorld.h"

#include "ecs/components/ECS_Components.h"

float rotation = .0f;

void GOC_EcsWorld::Awake()
{
    for (size_t i = 0; i < 1000; i++)
    {
        for (size_t j = 0; j < 100; j++)
        {
            entt::entity entity = m_Registry.create();
            m_Registry.emplace<Position>(entity, cosf(i * ACSU_Math::Mathf::Deg2Rad) * 1280.0f, sinf(j * ACSU_Math::Mathf::Deg2Rad) * 720.0f, 0.0f);
            m_Registry.emplace<Velocity>(entity, 0.5f, 0.5f, 0.5f);
        }
    }
}

void GOC_EcsWorld::Start()
{
    image = LoadGraph("assets/sprites/IMG_3337.jpg");
}

void GOC_EcsWorld::Update(float dt)
{
    //for (auto [entity, transform, velocity] : m_Registry.view<Position, Velocity>().each()) {
    //    transform.x += velocity.x * dt;
    //    transform.y += velocity.y * dt;
    //    transform.z += velocity.z * dt;
    //}
    //rotation += 45.0f * dt;
}

void GOC_EcsWorld::Draw(float dt)
{
    int i = 0;
    for (auto [entity, pos] : m_Registry.view<Position>().each()) {
        i++;
        //DrawGraph(pos.x, pos.y, image, true);
        //DrawRectRotaGraphF(pos.x, pos.y, 0, 0, 646, 583, 0.1, (rotation - i) * ACSU_Math::Mathf::Deg2Rad, image, true);
    }
    DrawFormatString(50, 200, 0xffffff, "i:%d", i);
}
