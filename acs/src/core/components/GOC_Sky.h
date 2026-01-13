#pragma once

#include <Pch.h>

class GOC_Sky : public GO_Component
{
public:
	// 静的な初期化パラメータ（メッシュやシェーダーパスなど）
	struct SetupParam
	{
		float Radius = 50000.0f;
		int Slices = 64;
		int Stacks = 32;
		bool DisableLighting = true;
		bool DisableZBuffer = true;
		const char* PixelShaderPath = nullptr;
	};

	// 動的な環境設定（色、時間、速度など）
	struct EnvironmentSettings
	{
		// 時間制御
		float CurrentTime = 0.2f;       // 0.0 ~ 1.0 (0.0=真夜中, 0.25=日の出, 0.5=正午, 0.75=日没)
		float TimeSpeed = 0.05f;        // 1秒あたりに進む時間の割合 (0.0で停止)
		bool AutoProgress = true;       // Updateで自動的に時間を進めるか

		// 太陽・雲
		float SunSize = 0.04f;
		float SunConvergence = 150.0f;
		float CloudDensity = 0.5f;
		float CloudSharpness = 0.1f;

		// 昼の色設定
		ACSU_Math::Vector3 DaySkyColor = { 0.10f, 0.40f, 0.80f };
		ACSU_Math::Vector3 DayHorizonColor = { 0.60f, 0.80f, 0.95f };
		ACSU_Math::Vector3 DayGroundColor = { 0.37f, 0.35f, 0.34f };

		// 夕方の色設定
		ACSU_Math::Vector3 SunsetSkyColor = { 0.10f, 0.05f, 0.20f };
		ACSU_Math::Vector3 SunsetHorizonColor = { 1.0f, 0.30f, 0.05f };
		ACSU_Math::Vector3 SunsetGroundColor = { 0.25f, 0.20f, 0.20f };

		// 夜の色設定
		ACSU_Math::Vector3 NightSkyColor = { 0.01f, 0.01f, 0.05f };
		ACSU_Math::Vector3 NightHorizonColor = { 0.02f, 0.02f, 0.10f };
		ACSU_Math::Vector3 NightGroundColor = { 0.05f, 0.05f, 0.05f };
	};

private:
	// シェーダー定数バッファ用構造体（内部利用）
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
	void UpdateEnvironmentLogic(float dt); // 計算ロジック

	// ヘルパー関数
	static float Lerp(float a, float b, float t);
	static ACSU_Math::Vector3 LerpColor(const ACSU_Math::Vector3& a, const ACSU_Math::Vector3& b, float t);

private:
	float m_TotalTime = 0.0f; // シェーダーに渡す累積時間

	SetupParam m_Param;
	EnvironmentSettings m_EnvSettings; // 現在の設定

	int m_PS = -1;
	int m_PSCBuffer = -1;

	std::vector<VERTEX3DSHADER> m_UnitVertices;
	std::vector<VERTEX3DSHADER> m_DrawVertices;
	std::vector<unsigned short> m_Indices;

	bool m_Created = false;
};