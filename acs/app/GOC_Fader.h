#pragma once
#include <Pch.h>

class GOC_Fader : public GO_Component
{
public:
	using Action = std::function<void()>;

	struct SetupParameter
	{
		unsigned int Color;
		float Duration;
		Action FadeinCallback;
		Action FadeoutCallback;
	};
	GOC_Fader() : m_Color(0xffffff), m_Duration(1.0f), m_Progress(0.0f), m_Alpha(0.0f), m_TargetAlpha(0.0f), m_IsProcessing(false){}
	void Setup(const SetupParameter& param);

	void Update(float dt) override;
	void Draw(float dt) override;

	void Fadein(bool isInvokeAction = true);
	void Fadeout(bool isInvokeAction = true);

private:

	enum Type
	{
		in,
		out
	};

	Action m_FadeinAction{};
	Action m_FadeoutAction{};
	unsigned int m_Color{};
	float m_TargetAlpha{};
	float m_Alpha{};
	float m_Duration{};
	float m_Progress{};
	bool m_IsProcessing{};
	bool m_IsInvokeAction{};
	int m_Type{};
};