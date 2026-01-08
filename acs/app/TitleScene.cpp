#include "TitleScene.h"

#include "BoxRenderComponent.h"
#include "SkyComponent.h"
#include "SpriteComponent.h"

namespace
{
	static float g_SunTime = 0.0f;

    GameObject* cameraObj = nullptr;
	GameObject* spriteObj = nullptr;

	// 1日のサイクル制御用
	// 0.0(夜明け) -> 0.25(昼) -> 0.5(夕方) -> 0.75(夜) -> 1.0
	static float g_DayTime = 0.2f;
	static const float g_DaySpeed = 0.05f; // 1秒あたりの進む量(調整してください)
}

void TitleScene::OnInitialize()
{
	ACSU_Time::SetTimeScale(0.5f);

	// Scene初期化時
	cameraObj = CreateGameObject<GameObject>();

	// CameraComponent は既にある想定
	auto* cam = cameraObj->AddComponent<CameraComponent>();
	cam->SetNearFar(0.1f, 10000000.0f);
	cam->SetFovY(110.0f * 3.1415926535f / 180.0f);
//	cameraObj->GetComponent<Transform>()->SetLocalRotation(ACSU_Math::Quaternion::Euler(ACSU_Math::Vector3::left()));

	auto* sky = cameraObj->AddComponent<SkyComponent>();

	SkyComponent::SetupParam sp;
	sp.Radius = 50000.0f;
	sp.Slices = 128;
	sp.Stacks = 64;
	sp.DisableLighting = true;
	sp.DisableZBuffer = true;
	sp.PixelShaderPath = "assets/shader/SkyPS.pso";

	sky->Setup(sp);

	// 初期値（0..1正規化前提のキーも入れる）
	cameraObj->GetBlackboard().Set(BlackboardKeys::SunDirection, ACSU_Math::Vector3(0.2f, 0.9f, 0.3f));
	cameraObj->GetBlackboard().Set(BlackboardKeys::SunIntensity, 1.0f);
	cameraObj->GetBlackboard().Set(BlackboardKeys::Turbidity, 2.5f);
	cameraObj->GetBlackboard().Set(BlackboardKeys::Exposure, 1.15f);

	spriteObj = CreateGameObject<GameObject>();
	auto* sprite = spriteObj->AddComponent<SpriteComponent>();
	{
		SpriteComponent::SetupParam param;
		param.Path = "assets/sprites/cursor.png";
		param.DivX = 5;
		param.DivY = 1;
		param.CellW = 48;
		param.CellH = 48;
		param.Loop = true;
		param.FrameTime = .1f;
		sprite->Setup(param);
	}
}

void TitleScene::OnPreUpdate(float deltaTime)
{
	if (ACSM_Input::GetKeyDown(KeyCode::G))
	{
		SceneManager::GetInstance()->ChangeScene("Game");
	}

	const POINT p = ACSM_Input::GetMousePoint();
	spriteObj->GetComponent<Transform>()->SetLocalPosition(
		ACSU_Math::Vector3(
			static_cast<float>(p.x),
			static_cast<float>(p.y),
			0.0f
		)
	);
}

bool add = true;

void TitleScene::OnPostUpdate(float deltaTime)
{
	if (ACSM_Input::GetKeyDown(Z))
	{
		add = !add;
	}
	// 1. 時間を進める
	if(add)
	{
		g_DayTime += g_DaySpeed * deltaTime;
		if (g_DayTime > 1.0f) g_DayTime -= 1.0f;
	}
	else
	{
		g_DayTime -= g_DaySpeed * deltaTime;
		if (g_DayTime <= 0.0f) g_DayTime += 1.0f;
	}

	if (cameraObj)
	{
		Blackboard& bb = cameraObj->GetBlackboard();

		// 2. 太陽の方向を計算
		// 0.0(朝) -> 0.25(昼) -> 0.5(夕) -> 0.75(夜)
		float angle = (g_DayTime - 0.25f) * (2.0f * 3.14159265f);

		ACSU_Math::Vector3 sunDir;
		sunDir.x = angle; // 東から西へ
		sunDir.y = std::cos(angle);  // 上下運動
		sunDir.z = std::sin(angle);             // 奥行きは固定
		bb.Set(BlackboardKeys::SunDirection, sunDir);

		// 3. パラメータ制御 (ここが重要！)
		float sunHeight = sunDir.y; // -1.0(真夜中) ～ 1.0(正午)

		// --- Exposure (露出) ---
		// 昼間(height=1.0)は明るすぎるので露出を絞る (0.4) -> 青が濃くなる
		// 夜間(height<=0.0)は暗いので露出を開ける (2.0) -> 夜空が見える
		float targetExposure = std::lerp(2.0f, 0.5f, std::max(0.0f, sunHeight));
		bb.Set(BlackboardKeys::Exposure, targetExposure);

		// --- Turbidity (濁り) ---
		// 夕方(height=0付近)だけ値を上げて、夕焼けを赤く強調する
		float sunsetFactor = 1.0f - std::abs(sunHeight);
		sunsetFactor = std::clamp(sunsetFactor, 0.0f, 1.0f);
		sunsetFactor = std::pow(sunsetFactor, 5.0f); // ピークを鋭くする

		// 昼間は 1.0 (極めてクリアな青空)
		// 夕方は 20.0 (強い散乱で赤い空)
		float targetTurbidity = std::lerp(1.0f, 3.0f, sunsetFactor);
		bb.Set(BlackboardKeys::Turbidity, targetTurbidity);

		// --- Sun Intensity (太陽強度) ---
		// シェーダー側で強度を増幅しているので、基本は 1.0 でOK
		bb.Set(BlackboardKeys::SunIntensity, 1.0f);
	}
}

void TitleScene::OnPreFixedUpdate(float fixedDeltaTime)
{
}

void TitleScene::OnPostFixedUpdate(float fixedDeltaTime)
{
}

void TitleScene::OnPreRender(float deltaTime)
{
}

void TitleScene::OnPostRender(float deltaTime)
{
	DrawString(50, 100, "TitleScene", 0xff00ff);
}

void TitleScene::OnDestroy()
{
}
