#include "GOC_Light.h"

static DxLib::COLOR_F ToDxColorF(const ACSU_Math::Vector3& c, float intensity)
{
	DxLib::COLOR_F col;
	col.r = c.x * intensity;
	col.g = c.y * intensity;
	col.b = c.z * intensity;
	col.a = 1.0f;
	return col;
}

GOC_Light::~GOC_Light()
{
	Destroy();
}

void GOC_Light::Destroy()
{
	if (m_LightHandle != -1)
	{
		DxLib::DeleteLightHandle(m_LightHandle);
		m_LightHandle = -1;
	}
}

void GOC_Light::Start()
{
	ApplyParameters();
}

void GOC_Light::SetupDirectional(const ACSU_Math::Vector3& color, float intensity)
{
	m_Type = LightType::Directional;
	m_Param.Color = color;
	m_Param.Intensity = intensity;
	m_IsMainLight = true;

	DxLib::SetUseLighting(TRUE);
}

void GOC_Light::SetupPoint(float range, const ACSU_Math::Vector3& color, float intensity)
{
	m_Type = LightType::Point;
	m_Param.Range = range;
	m_Param.Color = color;
	m_Param.Intensity = intensity;
	m_IsMainLight = false;

	// ハンドル作成
	if (m_LightHandle == -1)
	{
		// CreatePointLightHandle( Position, Range, Atten0, Atten1, Atten2 )
		m_LightHandle = DxLib::CreatePointLightHandle(
			DxLib::VGet(0, 0, 0),
			range,
			m_Param.Atten0,
			m_Param.Atten1,
			m_Param.Atten2
		);
	}
	DxLib::SetUseLighting(TRUE);
}

void GOC_Light::SetupSpot(float range, float angle, const ACSU_Math::Vector3& color, float intensity)
{
	m_Type = LightType::Spot;
	m_Param.Range = range;
	m_Param.SpotOuterAngle = angle;
	m_Param.SpotInnerAngle = angle * 0.8f;
	m_Param.Color = color;
	m_Param.Intensity = intensity;
	m_IsMainLight = false;

	// ハンドル作成
	if (m_LightHandle == -1)
	{
		// CreateSpotLightHandle( Position, Direction, Range, Atten0, Atten1, Atten2, InnerAngle, OuterAngle )
		m_LightHandle = DxLib::CreateSpotLightHandle(
			DxLib::VGet(0, 0, 0),
			DxLib::VGet(0, -1, 0),
			range,
			m_Param.Atten0,
			m_Param.Atten1,
			m_Param.Atten2,
			m_Param.SpotInnerAngle * ACSU_Math::Mathf::Deg2Rad,
			m_Param.SpotOuterAngle * ACSU_Math::Mathf::Deg2Rad
		);
	}
	DxLib::SetUseLighting(TRUE);
}

void GOC_Light::Update(float deltaTime)
{
	auto* go = GetGameObject();
	if (!go) return;

	auto* transform = go->GetGOC<Transform>();
	if (!transform) return;

	ACSU_Math::Vector3 forward = transform->Forward();
	ACSU_Math::Vector3 pos = transform->GetWorldPosition();

	// 色情報の作成 (COLOR_F)
	DxLib::COLOR_F colorF = ToDxColorF(m_Param.Color, m_Param.Intensity);

	// --- メインライト (標準ライト) の場合 ---
	if (m_IsMainLight && m_Type == LightType::Directional)
	{
		// 標準ライト設定
		DxLib::SetLightDirection(forward);

		// Color系関数は存在しないため、Dif(拡散)とSpc(鏡面)を設定する
		DxLib::SetLightDifColor(colorF);
		DxLib::SetLightSpcColor(colorF);
		// Amb(環境光)はシーン全体の設定によるが、必要ならここでも設定可能
		// DxLib::SetLightAmbColor( ... );
	}
	// --- 追加ライト (ハンドル) の場合 ---
	else if (m_LightHandle != -1)
	{
		// 1. 位置の設定
		DxLib::SetLightPositionHandle(m_LightHandle, pos);

		// 2. 色の設定 (DifとSpcを設定)
		DxLib::SetLightDifColorHandle(m_LightHandle, colorF);
		DxLib::SetLightSpcColorHandle(m_LightHandle, colorF);

		// 3. タイプ別設定
		if (m_Type == LightType::Directional)
		{
			DxLib::SetLightDirectionHandle(m_LightHandle, forward);
		}
		else if (m_Type == LightType::Point)
		{
			DxLib::SetLightRangeAttenHandle(
				m_LightHandle,
				m_Param.Range,
				m_Param.Atten0, m_Param.Atten1, m_Param.Atten2
			);
		}
		else if (m_Type == LightType::Spot)
		{
			DxLib::SetLightDirectionHandle(m_LightHandle, forward);

			DxLib::SetLightRangeAttenHandle(
				m_LightHandle,
				m_Param.Range,
				m_Param.Atten0, m_Param.Atten1, m_Param.Atten2
			);

			// スポットライトの角度設定
			// リストにある通り SetLightAngleHandle を使用
			DxLib::SetLightAngleHandle(
				m_LightHandle,
				m_Param.SpotOuterAngle * ACSU_Math::Mathf::Deg2Rad,
				m_Param.SpotInnerAngle * ACSU_Math::Mathf::Deg2Rad
			);
		}
	}
}

void GOC_Light::SetColor(const ACSU_Math::Vector3& color)
{
	m_Param.Color = color;
}
void GOC_Light::SetIntensity(float intensity)
{
	m_Param.Intensity = intensity;
}
void GOC_Light::SetRange(float range)
{
	m_Param.Range = range;
}
void GOC_Light::SetDirection(const ACSU_Math::Vector3& dir)
{
	// 1. Transformがあればそちらも回しておく（可視化デバッグ用などで便利）
	if (auto* trs = GetGameObject()->GetGOC<Transform>())
	{
		// Vector3::up() は (0,1,0) と仮定
		trs->LookAt(trs->GetWorldPosition() + dir, ACSU_Math::Vector3(0, 1, 0));
	}

	// 2. DxLibのハンドルへ反映
	if (m_LightHandle != -1 && (m_Type == LightType::Directional || m_Type == LightType::Spot))
	{
		VECTOR dxDir{};
		dxDir.x = dir.x;
		dxDir.y = dir.y;
		dxDir.z = dir.z;

		DxLib::SetLightDirectionHandle(m_LightHandle, dxDir);
	}
}
void GOC_Light::ApplyParameters()
{
	Update(0.0f);
}