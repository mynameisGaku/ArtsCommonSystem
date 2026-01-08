#pragma once

__interface IScene
{
	virtual void Initialize() = 0;
	virtual void Update(float deltaTime) = 0;
	virtual void FixedUpdate(float fixedDeltaTime) = 0;
	virtual void Render(float deltaTime) = 0;
	virtual void Destroy() = 0;
};