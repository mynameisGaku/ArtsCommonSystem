#pragma once

__interface IObject
{
	virtual void Awake() {}
	virtual void Start() {}
	virtual void Update(float deltaTime) {}
	virtual void FixedUpdate(float fixedDeltaTime) {}
	virtual void Draw(float deltaTime) {}
	virtual void Destroy() {}
};