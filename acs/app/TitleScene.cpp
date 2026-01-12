#include "TitleScene.h"

#include "BoxRenderComponent.h"
#include "SkyComponent.h"
#include "SpriteComponent.h"

namespace
{
	static float g_SunTime = 0.0f;

	GameObject* cameraObj = nullptr;
	GameObject* spriteObj = nullptr;
	GameObject* skyObj = nullptr;

	static float g_DayTime = 0.2f;
	static const float g_DaySpeed = 0.05f;
	static float mySunSize = 0.001f;
	static float myConvergence = 150.0f;
	static float myCloudDensity = 0.5f;

	float Lerp(float a, float b, float t)
	{
		return a + (b - a) * t;
	}

	ACSU_Math::Vector3 LerpColor(const ACSU_Math::Vector3& a, const ACSU_Math::Vector3& b, float t)
	{
		return ACSU_Math::Vector3(
			Lerp(a.x, b.x, t),
			Lerp(a.y, b.y, t),
			Lerp(a.z, b.z, t)
		);
	}
}

void TitleScene::OnInitialize()
{
	ACSU_Time::SetTimeScale(0.5f);

	cameraObj = CreateGameObject<GameObject>();

	auto* cam = cameraObj->AddComponent<CameraComponent>();
	cam->SetNearFar(0.1f, 10000000.0f);
	cam->SetFov(75.0f);

	skyObj = CreateGameObject<GameObject>();
	auto* sky = skyObj->AddComponent<SkyComponent>();

	SkyComponent::SetupParam sp;
	sp.Radius = 50000.0f;
	sp.Slices = 128;
	sp.Stacks = 64;
	sp.DisableLighting = true;
	sp.DisableZBuffer = true;

	sp.PixelShaderPath = "assets/shader/SkyPS.pso";

	sky->Setup(sp);

	skyObj->GetBlackboard().Set(BlackboardKeys::SunDirection, ACSU_Math::Vector3(0.0f, 1.0f, 0.0f));

	spriteObj = CreateGameObject<GameObject>();
	auto* spr = spriteObj->AddComponent<SpriteComponent>();
	{
		SpriteComponent::SetupParam param{};
		param.Path = "assets/sprites/cursor.png";
		param.DivX = 5;
		param.DivY = 1;
		param.CellW = 48;
		param.CellH = 48;
		param.Loop = true;
		param.FrameTime = 0.05f;
		spr->Setup(param);
	}
}

void TitleScene::OnPreUpdate(float deltaTime)
{
	auto mousePos = ACSM_Input::GetMousePoint();
	spriteObj->GetComponent<Transform>()->SetLocalPosition(ACSU_Math::Vector3(mousePos.x, mousePos.y, 0.0f));
}

void TitleScene::OnPostUpdate(float deltaTime)
{
	if (ACSM_Input::GetKey(UP))
	{
		g_DayTime += g_DaySpeed * deltaTime;
		if (g_DayTime > 1.0f) g_DayTime -= 1.0f;
	}
	if (ACSM_Input::GetKey(DOWN))
	{
		g_DayTime -= g_DaySpeed * deltaTime;
		if (g_DayTime < 0.0f) g_DayTime += 1.0f;
	}
	if (ACSM_Input::GetKey(A)) myCloudDensity += 1.0f;
	if (ACSM_Input::GetKey(S)) myCloudDensity -= 1.0f;
	if (ACSM_Input::GetKey(RIGHT)) myConvergence += 5.0f * deltaTime;
	if (ACSM_Input::GetKey(LEFT))  myConvergence -= 5.0f * deltaTime;
	if (mySunSize < 0.001f) mySunSize = 0.001f;
	if (myConvergence < 0.1f) myConvergence = 0.1f;


	float theta = (g_DayTime - 0.25f) * (2.0f * 3.14159265f);
	float sunY = std::sin(theta);
	float sunH = std::cos(theta);

	ACSU_Math::Vector3 sunDir(sunH, sunY, 0.0f);


	ACSU_Math::Vector3 kDaySky(0.10f, 0.40f, 0.80f);
	ACSU_Math::Vector3 kDayHorz(0.60f, 0.80f, 0.95f);
	ACSU_Math::Vector3 kDayGnd(0.37f, 0.35f, 0.34f);

	ACSU_Math::Vector3 kSunsetSky(0.10f, 0.05f, 0.20f);
	ACSU_Math::Vector3 kSunsetHorz(1.0f, 0.30f, 0.05f);
	ACSU_Math::Vector3 kSunsetGnd(0.25f, 0.20f, 0.20f);

	ACSU_Math::Vector3 kNightSky(0.01f, 0.01f, 0.05f);
	ACSU_Math::Vector3 kNightHorz(0.02f, 0.02f, 0.10f);
	ACSU_Math::Vector3 kNightGnd(0.05f, 0.05f, 0.05f);

	ACSU_Math::Vector3 cSky, cHorz, cGnd;
	float atmosphere = 1.0f;
	float exposure = 1.0f;

	if (sunY >= 0.0f)
	{

		float t = std::clamp(sunY / 0.2f, 0.0f, 1.0f);
		t = std::pow(t, 0.5f);

		cSky = LerpColor(kSunsetSky, kDaySky, t);
		cHorz = LerpColor(kSunsetHorz, kDayHorz, t);
		cGnd = LerpColor(kSunsetGnd, kDayGnd, t);
		atmosphere = Lerp(2.0f, 1.0f, t);
		exposure = Lerp(1.0f, 1.3f, t);
	}
	else
	{
		float t = std::clamp(std::abs(sunY) / 0.2f, 0.0f, 1.0f);

		cSky = LerpColor(kSunsetSky, kNightSky, t);
		cHorz = LerpColor(kSunsetHorz, kNightHorz, t);
		cGnd = LerpColor(kSunsetGnd, kNightGnd, t);
		atmosphere = 1.0f;
		exposure = Lerp(1.5f, 0.5f, t);
	}

	Blackboard& bb = skyObj->GetBlackboard();
	bb.Set(BlackboardKeys::SunDirection, sunDir);
	bb.Set(BlackboardKeys::SkyTint, cSky);
	bb.Set(BlackboardKeys::HorizonColor, cHorz);
	bb.Set(BlackboardKeys::GroundColor, cGnd);
	bb.Set(BlackboardKeys::AtmosphereThick, atmosphere);
	bb.Set(BlackboardKeys::Exposure, exposure);
	bb.Set(BlackboardKeys::SunSize, mySunSize);
	bb.Set(BlackboardKeys::SunConvergence, myConvergence);


	auto* trs = cameraObj->GetComponent<Transform>();

	//trs->RotateEulerLocal(ACSU_Math::Vector3(-15.0f * deltaTime, 0.0f, 0.0f));
	trs->SetLocalRotation(ACSU_Math::Quaternion::Euler(ACSU_Math::Vector3(0.0f, 90.0f, 0.0f)));

	cameraObj->GetBlackboard().Set(
		BlackboardKeys::TransformWorldTRS,
		TransformTRS{
			trs->GetWorldPosition(),
			trs->GetWorldRotation(),
			trs->GetWorldScale(),
			trs->GetForwardVector(),
			trs->GetUpVector()
		}
	);

	if (skyObj)
	{
		auto* skyTrs = skyObj->GetComponent<Transform>();

		skyTrs->SetLocalPosition(trs->GetWorldPosition());
	}
}

void TitleScene::OnPreFixedUpdate(float fixedDeltaTime) {}
void TitleScene::OnPostFixedUpdate(float fixedDeltaTime) {}
void TitleScene::OnPreRender(float deltaTime) {}
void TitleScene::OnPostRender(float deltaTime)
{
	DrawFormatString(10, 50, 0xffffff, "Sun Convergence: %.1f (LEFT/RIGHT)", myConvergence);
	DrawFormatString(10, 70, 0xffffff, "Time : %.4f (UP/DOWN)", g_DayTime);
}
void TitleScene::OnDestroy() {}