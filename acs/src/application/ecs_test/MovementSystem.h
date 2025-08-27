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
			[](seecs::EntityID id, Position& pos, Velocity& vel)
			{
				pos.x += vel.x;
				pos.y += vel.y;
				if (pos.x > 800 || pos.y > 600) 
				{
					pos = { 0, 0 };
					vel = { 5 + GetRand(5), 5 + GetRand(5) };
				}
			});
	}

private:
	seecs::ECS* m_pWorld;
};