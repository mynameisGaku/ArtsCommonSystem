#pragma once

#include <Pch.h>

class SkyComponent : public Component
{
public:
	struct SetupParam
	{
		float Radius = 5000.0f;
		int Slices = 64;
		int Stacks = 32;

		bool DisableLighting = true;
		bool DisableZBuffer = true;

		// Precompiled PS binary path (.pso)
		const char* PixelShaderPath = nullptr;
	};

	// Unity Skybox/Procedural に準拠したパラメータ構造体
	struct SkyParamsCB
	{
		float SunDirX;
		float SunDirY;
		float SunDirZ;
		float SunSize;          // 0.0 ~ 1.0

		float SkyTintR;         // Sky Tint
		float SkyTintG;
		float SkyTintB;
		float AtmosphereThick;  // Atmosphere Thickness

		float GroundColorR;     // Ground Color
		float GroundColorG;
		float GroundColorB;
		float Exposure;         // Exposure

		float HorizonColorR;    // Horizon Color (計算または指定)
		float HorizonColorG;
		float HorizonColorB;
		float SunConvergence;   // Sun Convergence

		float Time;
		float CloudDensity;
		float CloudSharpness;
		float Padding;
	};

public:
	bool Setup(const SetupParam& p);

	void Start() override;
	void Update(float dt) override;
	void Draw(float dt) override;
	void Destroy() override;

private:
	void EnsureCreated();
	void RebuildMesh();

	static unsigned char ToUNormByte(float v01);
	static unsigned char ToSNormByte(float vNeg1To1);

private:
	float m_Time = 0.0f;

	SetupParam m_Param;

	int m_PS = -1;
	int m_PSCBuffer = -1;

	std::vector<VERTEX3DSHADER> m_UnitVertices;
	std::vector<VERTEX3DSHADER> m_DrawVertices;
	std::vector<unsigned short> m_Indices;

	bool m_Created = false;
};