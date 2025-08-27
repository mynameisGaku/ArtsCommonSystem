#pragma once
#include "Pch.h"
#include "Position.h"
#include "Scale.h"
#include "Color.h"
#include "modules/ecs/seecs.h"

class RenderingSystem
{
public:
	RenderingSystem(seecs::ECS* world) : m_pWorld(world) {}

	void Update(std::function<void(Position, unsigned int, int)> drawfunc)
	{
		m_pWorld->View<Position, Color, Scale>().ForEach(
			[&](seecs::EntityID id, Position& pos, Color& col, Scale& scale)
			{
				drawfunc(pos, col.color, scale.radius);
			});
	}

private:
	seecs::ECS* m_pWorld;
};