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
	DrawString(250, 100, "GameScene", 0xffffff);
}

void GameScene::OnPostRender(float deltaTime)
{
	DrawString(50, 100, "GameScene", 0xffffff);
}

void GameScene::OnDestroy()
{
}
