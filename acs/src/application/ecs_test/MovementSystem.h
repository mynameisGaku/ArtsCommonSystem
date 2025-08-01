#pragma once
#include "Pch.h"
#include "Position.h"
#include "Velocity.h"
#include "modules/ecs/seecs.h"

class MovementSystem
{
public:
	MovementSystem(seecs::ECS* world) : m_pWorld(world) {}

	void Update()
	{
		m_pWorld->View<Position, Velocity>().ForEach(
			[](seecs::EntityID id, Position& pos, const Velocity& vel)
			{
				pos.x += vel.x;
				pos.y += vel.y;
			});
	}

private:
	seecs::ECS* m_pWorld;
};