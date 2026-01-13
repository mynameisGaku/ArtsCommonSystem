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
	if (m_EnvSettings.AutoProgress)
	{
		m_EnvSettings.CurrentTime += m_EnvSettings.TimeSpeed * dt;
		if (m_EnvSettings.CurrentTime > 1.0f) m_EnvSettings.CurrentTime -= 1.0f;
		if (m_EnvSettings.CurrentTime < 0.0f) m_EnvSettings.CurrentTime += 1.0f;
	}

	float theta = (m_EnvSettings.CurrentTime - 0.25f) * (2.0f * 3.14159265f);
	float sunY = std::sin(theta);
	float sunH = std::cos(theta);
	ACSU_Math::Vector3 sunDir(sunH, sunY, 0.0f);

	ACSU_Math::Vector3 cSky, cHorz, cGnd;
	float atmosphere = 1.0f;
	float exposure = 1.0f;

	if (sunY >= 0.0f) // ’‹ŠÔ`“ú–v
	{
		float t = std::clamp(sunY / 0.2f, 0.0f, 1.0f);
		t = std::pow(t, 0.5f);

		cSky = LerpColor(m_EnvSettings.SunsetSkyColor, m_EnvSettings.DaySkyColor, t);
		cHorz = LerpColor(m_EnvSettings.SunsetHorizonColor, m_EnvSettings.DayHorizonColor, t);
		cGnd = LerpColor(m_EnvSettings.SunsetGroundColor, m_EnvSettings.DayGroundColor, t);
		atmosphere = Lerp(2.0f, 1.0f, t);
		exposure = Lerp(1.0f, 1.3f, t);
	}
	else // –éŠÔ
	{
		float t = std::clamp(std::abs(sunY) / 0.2f, 0.0f, 1.0f);

		cSky = LerpColor(m_EnvSettings.SunsetSkyColor, m_EnvSettings.NightSkyColor, t);
		cHorz = LerpColor(m_EnvSettings.SunsetHorizonColor, m_EnvSettings.NightHorizonColor, t);
		cGnd = LerpColor(m_EnvSettings.SunsetGroundColor, m_EnvSettings.NightGroundColor, t);
		atmosphere = 1.0f;
		exposure = Lerp(1.5f, 0.5f, t);
	}

	GameObject* go = GetGameObject();
	if (go)
	{
		Blackboard& bb = go->GetBlackboard();
		bb.Set(BlackboardKeys::SunDirection, sunDir);
		bb.Set(BlackboardKeys::SkyTint, cSky);
		bb.Set(BlackboardKeys::HorizonColor, cHorz);
		bb.Set(BlackboardKeys::GroundColor, cGnd);
		bb.Set(BlackboardKeys::AtmosphereThick, atmosphere);
		bb.Set(BlackboardKeys::Exposure, exposure);
		bb.Set(BlackboardKeys::SunSize, m_EnvSettings.SunSize);
		bb.Set(BlackboardKeys::SunConvergence, m_EnvSettings.SunConvergence);
		bb.Set(BlackboardKeys::Turbidity, m_EnvSettings.CloudDensity);
	}

	void* p = GetBufferShaderConstantBuffer(m_PSCBuffer);
	if (p != nullptr)
	{
		SkyParamsCB* cb = static_cast<SkyParamsCB*>(p);

		cb->SunDirX = sunDir.x; cb->SunDirY = sunDir.y; cb->SunDirZ = sunDir.z;
		cb->SunSize = m_EnvSettings.SunSize;
		cb->SkyTintR = cSky.x; cb->SkyTintG = cSky.y; cb->SkyTintB = cSky.z;
		cb->AtmosphereThick = atmosphere;
		cb->GroundColorR = cGnd.x; cb->GroundColorG = cGnd.y; cb->GroundColorB = cGnd.z;
		cb->Exposure = exposure;
		cb->HorizonColorR = cHorz.x; cb->HorizonColorG = cHorz.y; cb->HorizonColorB = cHorz.z;
		cb->SunConvergence = m_EnvSettings.SunConvergence;

		cb->Time = m_TotalTime;
		cb->CloudDensity = m_EnvSettings.CloudDensity;
		cb->CloudSharpness = m_EnvSettings.CloudSharpness;

		UpdateShaderConstantBuffer(m_PSCBuffer);
	}
}

void GOC_Sky::Draw(float dt)
{
	if (!m_Created || m_PS == -1 || m_DrawVertices.empty()) return;

	GameObject* go = GetGameObject();
	if (!go) return;

	TransformTRS camTrs;
	ACSU_Math::Vector3 camPos(0, 0, 0);
	if (go->GetBlackboard().Get(BlackboardKeys::TransformWorldTRS, camTrs))
	{
		camPos = camTrs.position;
	}

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