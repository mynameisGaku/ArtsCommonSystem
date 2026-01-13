#include "GOC_Fader.h"

void GOC_Fader::Setup(const SetupParameter& param)
{
	m_Color = param.Color;
	m_Duration = param.Duration;
	m_FadeinAction = param.FadeinCallback;
	m_FadeoutAction = param.FadeoutCallback;
}

void GOC_Fader::Update(float dt)
{
	if (not m_IsProcessing)
		return;

	if (m_Progress > 0.0f)
	{
		m_Progress -= dt;

		float rate = m_Duration - m_Progress;
		float rate01 = rate / m_Duration;
		m_Alpha = std::lerp(m_Alpha, m_TargetAlpha, rate01);
	}
	else
	{
		if (m_IsProcessing)
		{
			if (m_IsInvokeAction)
			{
				switch (m_Type)
				{
				case in:
					if (m_FadeinAction) 
						m_FadeinAction();
					break;
				case out:
					if (m_FadeoutAction)
						m_FadeoutAction();
					break;
				}
			}

			m_Alpha = m_TargetAlpha;
			m_IsProcessing = false;
		}
	}
}

void GOC_Fader::Draw(float dt)
{
	SetDrawBlendMode(DX_BLENDMODE_ALPHA, (int)m_Alpha);

	DrawBoxAA(0.0f, 0.0f, 1280.0f, 720.0f, m_Color, true);

	SetDrawBlendMode(DX_BLENDMODE_NOBLEND, 0);
}

void GOC_Fader::Fadein(bool isInvokeAction)
{
	if (m_IsProcessing)
		return;

	m_TargetAlpha = 0.0f;
	m_Alpha = 255.0f;
	m_Progress = m_Duration;
	m_IsProcessing = true;
	m_IsInvokeAction = isInvokeAction;
	m_Type = in;
}

void GOC_Fader::Fadeout(bool isInvokeAction)
{
	if (m_IsProcessing)
		return;

	m_TargetAlpha = 255.0f;
	m_Alpha = 0.0f;
	m_Progress = m_Duration;
	m_IsProcessing = true;
	m_IsInvokeAction = isInvokeAction;
	m_Type = out;
}