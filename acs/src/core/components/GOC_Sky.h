#pragma once

#include <Pch.h>

class GOC_Sky : public GO_Component
{
public:
	struct SetupParam
	{
		float Radius = 50000.0f;
		int Slices = 64;
		int Stacks = 32;
		bool DisableLighting = true;
		bool DisableZBuffer = true;
		const char* PixelShaderPath = nullptr;
	};

	struct EnvironmentSettings
	{
		float CurrentTime = 0.2f;
		float TimeSpeed = 0.05f;
		bool AutoProgress = true;

		float SunSize = 0.04f;
		float SunConvergence = 150.0f;
		float CloudDensity = 0.5f;
		float CloudSharpness = 0.1f;

		ACSU_Math::Vector3 DaySkyColor = { 0.10f, 0.40f, 0.80f };
		ACSU_Math::Vector3 DayHorizonColor = { 0.60f, 0.80f, 0.95f };
		ACSU_Math::Vector3 DayGroundColor = { 0.37f, 0.35f, 0.34f };

		ACSU_Math::Vector3 SunsetSkyColor = { 0.10f, 0.05f, 0.20f };
		ACSU_Math::Vector3 SunsetHorizonColor = { 1.0f, 0.30f, 0.05f };
		ACSU_Math::Vector3 SunsetGroundColor = { 0.25f, 0.20f, 0.20f };

		ACSU_Math::Vector3 NightSkyColor = { 0.01f, 0.01f, 0.05f };
		ACSU_Math::Vector3 NightHorizonColor = { 0.02f, 0.02f, 0.10f };
		ACSU_Math::Vector3 NightGroundColor = { 0.05f, 0.05f, 0.05f };
	};

private:
	struct SkyParamsCB
	{
		float SunDirX, SunDirY, SunDirZ, SunSize;
		float SkyTintR, SkyTintG, SkyTintB, AtmosphereThick;
		float GroundColorR, GroundColorG, GroundColorB, Exposure;
		float HorizonColorR, HorizonColorG, HorizonColorB, SunConvergence;
		float Time, CloudDensity, CloudSharpness, Padding;
	};

public:
	~GOC_Sky()
	{
	}
	bool Setup(const SetupParam& p);

	EnvironmentSettings& GetSettings() { return m_EnvSettings; }
	const EnvironmentSettings& GetSettings() const { return m_EnvSettings; }

	void SetTime(float time01) { m_EnvSettings.CurrentTime = time01; }
	float GetTime() const { return m_EnvSettings.CurrentTime; }
	void SetTimeSpeed(float speed) { m_EnvSettings.TimeSpeed = speed; }

	void Start() override;
	void Update(float dt) override;
	void Draw(float dt) override;
	void Destroy() override;

private:
	void EnsureCreated();
	void RebuildMesh();
	void UpdateEnvironmentLogic(float dt);

	static float Lerp(float a, float b, float t);
	static ACSU_Math::Vector3 LerpColor(const ACSU_Math::Vector3& a, const ACSU_Math::Vector3& b, float t);

private:
	float m_TotalTime = 0.0f;

	SetupParam m_Param;
	EnvironmentSettings m_EnvSettings;

	int m_PS = -1;
	int m_PSCBuffer = -1;

	std::vector<VERTEX3DSHADER> m_UnitVertices;
	std::vector<VERTEX3DSHADER> m_DrawVertices;
	std::vector<unsigned short> m_Indices;

	bool m_Created = false;
};