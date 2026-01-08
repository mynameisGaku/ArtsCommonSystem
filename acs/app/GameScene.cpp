#include "GameScene.h"


void GameScene::OnInitialize()
{
}

void GameScene::OnPreUpdate(float deltaTime)
{
}

void GameScene::OnPostUpdate(float deltaTime)
{
	if (ACSM_Input::GetKeyDown(KeyCode::T))
	{
		SceneManager::GetInstance()->ChangeScene("Title");
	}
}

void GameScene::OnPreFixedUpdate(float fixedDeltaTime)
{
}

void GameScene::OnPostFixedUpdate(float fixedDeltaTime)
{
}

void GameScene::OnPreRender(float deltaTime)
{
}

void GameScene::OnPostRender(float deltaTime)
{
	DrawString(50, 100, "GameScene", 0xff00ff);
}

void GameScene::OnDestroy()
{
}
