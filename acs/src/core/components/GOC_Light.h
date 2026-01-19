#pragma once
#include <Pch.h>

enum class LightType
{
	Directional, // 平行光源 (太陽など)
	Point,       // 点光源 (電球、炎など)
	Spot         // スポットライト (懐中電灯など)
};

struct LightParam
{
	ACSU_Math::Vector3 Color = { 1.0f, 1.0f, 1.0f }; // 光の色 (0.0 ~ 1.0)
	float Intensity = 1.0f;      // 光の強さ (色に乗算されます)
	float Range = 1000.0f;       // 点光源・スポットライトの届く距離

	// スポットライト用
	float SpotOuterAngle = 45.0f;
	float SpotInnerAngle = 30.0f;

	// 減衰率 (点光源用: 1.0, 0.0, 0.0 は減衰なしに近い設定)
	float Atten0 = 1.0f;
	float Atten1 = 0.0f;
	float Atten2 = 0.0f;
};

class GOC_Light : public GO_Component
{
public:
	GOC_Light() = default;
	virtual ~GOC_Light();

	void Start() override;
	void Update(float deltaTime) override; // 位置や向きの更新
	void Destroy() override;

	// --- セットアップ関数 ---

	// メインの平行光源として設定 (DxLib標準ライトを使用)
	void SetupDirectional(const ACSU_Math::Vector3& color, float intensity = 1.0f);

	// 点光源として設定 (追加ライトハンドルを作成)
	void SetupPoint(float range, const ACSU_Math::Vector3& color, float intensity = 1.0f);

	// スポットライトとして設定
	void SetupSpot(float range, float angle, const ACSU_Math::Vector3& color, float intensity = 1.0f);

	// --- 制御 ---
	void SetColor(const ACSU_Math::Vector3& color);
	void SetIntensity(float intensity);
	void SetRange(float range);
	void SetDirection(const ACSU_Math::Vector3& dir);

private:
	void ApplyParameters();

private:
	LightType m_Type = LightType::Directional;
	LightParam m_Param;

	int m_LightHandle = -1;     // 追加ライト用のハンドル
	bool m_IsMainLight = false; // DxLibの標準ライト(SetLight...)を使うかどうか
};