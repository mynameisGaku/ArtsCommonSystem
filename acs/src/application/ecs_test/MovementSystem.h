#pragma once
#include "modules/ecs/ISystem.h"
#include "modules/ecs/ECSWorld.h"
#include "Position.h"
#include "Velocity.h"

class MovementSystem : public ISystem
{
public:
    MovementSystem(ECSWorld* world) : m_pWorld(world) {}

    void Update() override
    {
        auto ids = m_pWorld->QueryEntities({
            ComponentTypeID::Get<Position>(),
            ComponentTypeID::Get<Velocity>()
            });

        for (auto id : ids)
        {
            auto* p = m_pWorld->GetComponent<Position>(id);
            auto* v = m_pWorld->GetComponent<Velocity>(id);
            if (p && v)
            {
                p->x += v->x;
                p->y += v->y;
            }
        }
    }

private:
    ECSWorld* m_pWorld;
};
