#pragma once

#include <Pch.h>

class GameScene : public SceneBase
{
public:
	void OnAwake() override;
	void OnStart() override;
	void OnPreUpdate(float deltaTime) override;
	void OnPostUpdate(float deltaTime) override;
	void OnPreFixedUpdate(float fixedDeltaTime) override;
	void OnPostFixedUpdate(float fixedDeltaTime) override;
	void OnPreRender(float deltaTime) override;
	void OnPostRender(float deltaTime) override;
	void OnDestroy() override;
};