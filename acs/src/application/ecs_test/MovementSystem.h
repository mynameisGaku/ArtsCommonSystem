#pragma once
#include "Pch.h"
#include "Position.h"
#include "Velocity.h"

class MovementSystem : public IACSM_ECS_System
{
public:
    MovementSystem(ACSM_ECS_ECSWorld* world) : m_pWorld(world) {}

    void Update() override
    {
        auto ids = m_pWorld->QueryEntities({
            ACSM_ECS_ComponentTypeID::Get<Position>(),
            ACSM_ECS_ComponentTypeID::Get<Velocity>()
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
    ACSM_ECS_ECSWorld* m_pWorld;
};
