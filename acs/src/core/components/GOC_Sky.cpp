#include "GOC_Sky.h"
#include <algorithm>
#include <cmath>

bool GOC_Sky::Setup(const SetupParam& p)
{
	m_Param = p;
	if (m_Param.Slices < 8) m_Param.Slices = 8;
	if (m_Param.Stacks < 4) m_Param.Stacks = 4;
	return true;
}

void GOC_Sky::Start()
{
	EnsureCreated();
}

void GOC_Sky::EnsureCreated()
{
	if (m_Created) return;
	if (m_Param.PixelShaderPath == nullptr) return;

	m_PS = LoadPixelShader(const_cast<char*>(m_Param.PixelShaderPath));
	if (m_PS == -1) return;

	m_PSCBuffer = CreateShaderConstantBuffer(sizeof(SkyParamsCB));
	RebuildMesh();
	m_Created = true;
}

void GOC_Sky::RebuildMesh()
{
	m_UnitVertices.clear();
	m_Indices.clear();

	int slices = m_Param.Slices;
	int stacks = m_Param.Stacks;

	for (int j = 0; j <= stacks; ++j)
	{
		float v = (float)j / (float)stacks;
		float phi = v * 3.14159265f;

		for (int i = 0; i <= slices; ++i)
		{
			float u = (float)i / (float)slices;
			float theta = u * 2.0f * 3.14159265f;
			float y = std::cos(phi);
			float r = std::sin(phi);
			float x = r * std::cos(theta);
			float z = r * std::sin(theta);

			VERTEX3DSHADER vert;
			vert.pos = VGet(x, y, z);
			vert.norm = VGet(x, y, z);
			vert.dif = GetColorU8(255, 255, 255, 255);
			vert.spc = GetColorU8(0, 0, 0, 0);
			vert.u = u; vert.v = v; vert.su = 0; vert.sv = 0;
			m_UnitVertices.push_back(vert);
		}
	}
	for (int j = 0; j < stacks; ++j)
	{
		for (int i = 0; i < slices; ++i)
		{
			int p0 = j * (slices + 1) + i;
			int p1 = p0 + 1;
			int p2 = (j + 1) * (slices + 1) + i;
			int p3 = p2 + 1;
			m_Indices.push_back(static_cast<unsigned short>(p0));
			m_Indices.push_back(static_cast<unsigned short>(p1));
			m_Indices.push_back(static_cast<unsigned short>(p2));
			m_Indices.push_back(static_cast<unsigned short>(p2));
			m_Indices.push_back(static_cast<unsigned short>(p1));
			m_Indices.push_back(static_cast<unsigned short>(p3));
		}
	}
	m_DrawVertices = m_UnitVertices;
}

float GOC_Sky::Lerp(float a, float b, float t) { return a + (b - a) * t; }
ACSU_Math::Vector3 GOC_Sky::LerpColor(const ACSU_Math::Vector3& a, const ACSU_Math::Vector3& b, float t)
{
	return ACSU_Math::Vector3(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t));
}

void GOC_Sky::Update(float deltaTime)
{
	if (!m_Created || m_PSCBuffer == -1) return;

	m_TotalTime += deltaTime;
	UpdateEnvironmentLogic(deltaTime);
}

void GOC_Sky::UpdateEnvironmentLogic(float dt)
{
	// -------------------------------------------------------------
	// 1. 時間の進行処理
	// -------------------------------------------------------------
	if (m_EnvSettings.AutoProgress)
	{
		m_EnvSettings.CurrentTime += m_EnvSettings.TimeSpeed * dt;
		// 時間を 0.0 ～ 1.0 の範囲に正規化
		if (m_EnvSettings.CurrentTime > 1.0f) m_EnvSettings.CurrentTime -= 1.0f;
		if (m_EnvSettings.CurrentTime < 0.0f) m_EnvSettings.CurrentTime += 1.0f;
	}

	// -------------------------------------------------------------
	// 2. 太陽の位置計算（提示されたロジック）
	// -------------------------------------------------------------
	// 0.25f (6:00) を基準に回転させる
	float theta = (m_EnvSettings.CurrentTime - 0.25f) * (2.0f * 3.14159265f);
	float sunY = std::sin(theta); // 高さ
	float sunH = std::cos(theta); // 水平成分
	ACSU_Math::Vector3 sunDir(sunH, sunY, 0.0f);
	sunDir.Normalize(); // 念のため正規化

	// メンバ変数にも保持しておく（必要であれば）
	m_CurrentSunDirection = sunDir;

	// -------------------------------------------------------------
	// 3. 空の色・大気パラメータの計算（昼・夕方・夜の分岐）
	// -------------------------------------------------------------
	ACSU_Math::Vector3 cSky, cHorz, cGnd;
	float atmosphere = 1.0f;
	float exposure = 1.0f;

	if (sunY >= 0.0f) // 昼間 ～ 日没
	{
		// 太陽の高さ(0.0~0.2)に応じて夕方具合を計算
		float t = std::clamp(sunY / 0.2f, 0.0f, 1.0f);
		t = std::pow(t, 0.5f); // ガンマ補正的なカーブで変化を滑らかに

		// 夕焼け色 と 昼間の色 をブレンド
		cSky = LerpColor(m_EnvSettings.SunsetSkyColor, m_EnvSettings.DaySkyColor, t);
		cHorz = LerpColor(m_EnvSettings.SunsetHorizonColor, m_EnvSettings.DayHorizonColor, t);
		cGnd = LerpColor(m_EnvSettings.SunsetGroundColor, m_EnvSettings.DayGroundColor, t);

		atmosphere = Lerp(2.0f, 1.0f, t);   // 夕方は大気が厚く見える演出
		exposure = Lerp(1.0f, 1.3f, t);     // 昼間は露出を上げる
	}
	else // 夜間
	{
		// 日没直後(0.0) ～ 完全な夜(0.2以上)
		float t = std::clamp(std::abs(sunY) / 0.2f, 0.0f, 1.0f);

		// 夕焼け色 と 夜の色 をブレンド
		cSky = LerpColor(m_EnvSettings.SunsetSkyColor, m_EnvSettings.NightSkyColor, t);
		cHorz = LerpColor(m_EnvSettings.SunsetHorizonColor, m_EnvSettings.NightHorizonColor, t);
		cGnd = LerpColor(m_EnvSettings.SunsetGroundColor, m_EnvSettings.NightGroundColor, t);

		atmosphere = 1.0f;
		exposure = Lerp(1.5f, 0.5f, t);     // 夜は少し暗く
	}

	// -------------------------------------------------------------
	// 4. SceneContext への反映（ゲーム全体での共有）
	// -------------------------------------------------------------
	// SceneBase.h/cpp の設計に合わせて、BlackboardではなくContextを使います
	SceneContext& ctx = GetGameObject()->GetSceneContext();

	ctx.SunDirection = sunDir;

	ctx.SkyTint = cSky;
	ctx.HorizonColor = cHorz;
	ctx.GroundColor = cGnd;
	ctx.SunSize = m_EnvSettings.SunSize;
	ctx.SunConvergence = m_EnvSettings.SunConvergence;
	ctx.Turbidity = m_EnvSettings.CloudDensity;
	ctx.Exposure = exposure;

	// もしBlackboardも必要であれば、ここで以前のようにSetしてください。
	// GameObject* go = GetGameObject();
	// if(go) { ... }

	// -------------------------------------------------------------
	// 5. シェーダー定数バッファへの転送（描画への反映）
	// -------------------------------------------------------------

	void* p = GetBufferShaderConstantBuffer(m_PSCBuffer);
	if (p != nullptr)
	{
		SkyParamsCB* cb = static_cast<SkyParamsCB*>(p);

		// 構造体の中身を埋める
		cb->SunDirX = m_CurrentSunDirection.x;
		cb->SunDirY = m_CurrentSunDirection.y;
		cb->SunDirZ = m_CurrentSunDirection.z;
		cb->SunSize = m_EnvSettings.SunSize;

		cb->SkyTintR = cSky.x;
		cb->SkyTintG = cSky.y;
		cb->SkyTintB = cSky.z;
		cb->AtmosphereThick = 1.0f; // 大気の厚み（デフォルト1.0）

		cb->GroundColorR = cGnd.x;
		cb->GroundColorG = cGnd.y;
		cb->GroundColorB = cGnd.z;
		cb->Exposure = 1.0f; // 露出（デフォルト1.0）

		cb->HorizonColorR = cHorz.x;
		cb->HorizonColorG = cHorz.y;
		cb->HorizonColorB = cHorz.z;
		cb->SunConvergence = m_EnvSettings.SunConvergence;

		cb->Time = m_EnvSettings.CurrentTime;
		cb->CloudDensity = m_EnvSettings.CloudDensity;
		cb->CloudSharpness = m_EnvSettings.CloudSharpness;
		cb->Padding = 0.0f;

		UpdateShaderConstantBuffer(m_PSCBuffer);
	}
}

void GOC_Sky::Draw(float dt)
{
	if (!m_Created || m_PS == -1 || m_DrawVertices.empty()) return;

	GameObject* go = GetGameObject();
	if (!go) return;

	ACSU_Math::Vector3 camPos(0, 0, 0);

	Transform* transform = go->GetGOC<Transform>();
	if (transform == nullptr) return;
	
	camPos = transform->GetLocalPosition();

	float rad = m_Param.Radius;
	size_t count = m_UnitVertices.size();
	for (size_t i = 0; i < count; ++i)
	{
		m_DrawVertices[i].pos.x = m_UnitVertices[i].pos.x * rad + camPos.x;
		m_DrawVertices[i].pos.y = m_UnitVertices[i].pos.y * rad + camPos.y;
		m_DrawVertices[i].pos.z = m_UnitVertices[i].pos.z * rad + camPos.z;
	}

	int oldCull = GetUseBackCulling();
	SetUseBackCulling(FALSE);

	if (m_Param.DisableLighting) SetUseLighting(FALSE);
	if (m_Param.DisableZBuffer) { SetUseZBuffer3D(FALSE); SetWriteZBuffer3D(FALSE); }

	SetUsePixelShader(m_PS);
	SetShaderConstantBuffer(m_PSCBuffer, DX_SHADERTYPE_PIXEL, 0);

	DrawPolygonIndexed3DToShader(
		m_DrawVertices.data(),
		static_cast<int>(m_DrawVertices.size()),
		m_Indices.data(),
		static_cast<int>(m_Indices.size() / 3)
	);

	SetShaderConstantBuffer(-1, DX_SHADERTYPE_PIXEL, 0);
	SetUsePixelShader(-1);

	if (m_Param.DisableZBuffer) { SetUseZBuffer3D(TRUE); SetWriteZBuffer3D(TRUE); }
	if (m_Param.DisableLighting) SetUseLighting(TRUE);
	SetUseBackCulling(oldCull);
}

void GOC_Sky::Destroy()
{
	if (m_PS != -1) { DeleteShader(m_PS); m_PS = -1; }
	if (m_PSCBuffer != -1) { DeleteShaderConstantBuffer(m_PSCBuffer); m_PSCBuffer = -1; }
	m_UnitVertices.clear();
	m_DrawVertices.clear();
	m_Indices.clear();
	m_Created = false;
}