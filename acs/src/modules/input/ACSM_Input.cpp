#include "ACSM_Input.h"

#include "JoyshockWrapper.h"
#include "KeyWrapper.h"


namespace
{
	// JSON の "S" / "PAD_S" どちらでも拾えるように同義語を入れておく
	bool TryParsePadName(const std::string& name, uint32_t& outMask)
	{
		std::unordered_map<std::string, uint32_t>* kPadMap =
		new std::unordered_map<std::string, uint32_t>{
			{ "S", JSMASK_S }, { "PAD_S", JSMASK_S },
			{ "W", JSMASK_W }, { "PAD_W", JSMASK_W },
			{ "E", JSMASK_E }, { "PAD_E", JSMASK_E },
			{ "N", JSMASK_N }, { "PAD_N", JSMASK_N },
			{ "ZL", JSMASK_ZL }, { "PAD_ZL", JSMASK_ZL },
			{ "ZR", JSMASK_ZR }, { "PAD_ZR", JSMASK_ZR },
			{ "L", JSMASK_L }, { "PAD_L", JSMASK_L },
			{ "R", JSMASK_R }, { "PAD_R", JSMASK_R },
			{ "MINUS", JSMASK_MINUS }, { "PAD_MINUS", JSMASK_MINUS },
			{ "PLUS", JSMASK_PLUS }, { "PAD_PLUS", JSMASK_PLUS },
			{ "LCLICK", JSMASK_LCLICK }, { "PAD_LCLICK", JSMASK_LCLICK },
			{ "RCLICK", JSMASK_RCLICK }, { "PAD_RCLICK", JSMASK_RCLICK },
			{ "HOME", JSMASK_HOME }, { "PAD_HOME", JSMASK_HOME },
			{ "CAPTURE", JSMASK_CAPTURE }, { "PAD_CAPTURE", JSMASK_CAPTURE },
		};

		auto it = kPadMap->find(name);
		if (it == kPadMap->end())
		{
			delete kPadMap;
			return false;
		}
		outMask = it->second;

		delete kPadMap;
		return true;
	}
}

#ifdef ACSM_DXLIB
namespace ACS_Mouse
{
	// deleteできるようにポインタ型にする
	std::unordered_map<MouseButton, bool>* g_pButtons = {};
	std::unordered_map<MouseButton, bool>* g_pButtonsPrev = {};

	void Initialize()
	{
		g_pButtons		= new std::unordered_map<MouseButton, bool>;
		g_pButtonsPrev	= new std::unordered_map<MouseButton, bool>;
	}

	void Finalize()
	{
		delete g_pButtons;
		delete g_pButtonsPrev;
	}

	void Update()
	{
		*g_pButtonsPrev = *g_pButtons;
		int mouseState = GetMouseInput();
		(*g_pButtons)[ACSM_Input::MouseButton::Left]	= (mouseState & MOUSE_INPUT_LEFT) != 0;
		(*g_pButtons)[ACSM_Input::MouseButton::Right]	= (mouseState & MOUSE_INPUT_RIGHT) != 0;
		(*g_pButtons)[ACSM_Input::MouseButton::Middle]	= (mouseState & MOUSE_INPUT_MIDDLE) != 0;
		(*g_pButtons)[ACSM_Input::MouseButton::X1]		= (mouseState & MOUSE_INPUT_4) != 0;
		(*g_pButtons)[ACSM_Input::MouseButton::X2]		= (mouseState & MOUSE_INPUT_5) != 0;
	}
}
#endif

namespace ASC_VirtualKey
{
	enum class DeviceType
	{
		None,
		Keyboard,
		Pad
	};

	struct Binding
	{
		DeviceType type;     // キーボード or パッド
		unsigned int code;   // Keyboard: 0x00..0xFF, Pad: JSL button mask (JSMASK_*)
		int deviceIndex;     // どのパッドか
	};

	static std::unordered_map<std::string, std::vector<Binding>>* s_pBindings; // name -> bindings
	static std::unordered_map<std::string, bool>* s_pCurr;                     // name -> pressed
	static std::unordered_map<std::string, bool>* s_pPrev;                     // name -> pressed(prev)

	void Initialize()
	{
		s_pBindings	= new std::unordered_map<std::string, std::vector<Binding>>;
		s_pCurr		= new std::unordered_map<std::string, bool>;
		s_pPrev		= new std::unordered_map<std::string, bool>;
	}

	void Finalize()
	{
		delete s_pBindings;
		delete s_pCurr;
		delete s_pPrev;
	}

	static bool PollBinding(const Binding& b)
	{
		if (b.type == DeviceType::Keyboard)
		{
			return KeyWrapper::Instance().GetKey(static_cast<unsigned char>(b.code));
		}
		else
		{
			// Pad は指定インデックス（デフォルト0）を参照
			return JoyshockWrapper::Instance().GetRawButton(b.deviceIndex, b.code);
		}
	}

	void Update()
	{
		// 前回状態を保存
		*s_pPrev = *s_pCurr;

		// 各仮想キーの押下を再計算（OR 集計）
		for (auto& kv : (*s_pBindings))
		{
			const std::string& name = kv.first;
			const auto& vec = kv.second;

			bool pressed = false;
			for (const auto& b : vec)
			{
				if (PollBinding(b))
				{
					pressed = true;
					break;
				}
			}
			(*s_pCurr)[name] = pressed;
		}
	}

	bool Subscribe(const std::string& name, const Binding& b)
	{
		if (name.empty())
		{
			return false;
		}
		if (b.type == DeviceType::Keyboard)
		{
			if (b.code > 0xFF) { return false; }
		}
		else
		{
			if (b.code == 0) { return false; } // JSMASK_UNKNOWN は無効
		}

		auto& vec = (*s_pBindings)[name];
		// 重複登録ガード
		auto it = std::find_if(vec.begin(), vec.end(),
			[&](const Binding& x)
			{
				return x.type == b.type && x.code == b.code && x.deviceIndex == b.deviceIndex;
			});
		if (it == vec.end())
		{
			vec.push_back(b);
		}

		// 初期状態エントリ確保
		s_pCurr->emplace(name, false);
		s_pPrev->emplace(name, false);
		return true;
	}

	bool Get(const std::string& name)
	{
		auto it = s_pCurr->find(name);
		return (it != s_pCurr->end()) ? it->second : false;
	}

	bool GetDown(const std::string& name)
	{
		const bool now = Get(name);
		const bool prev = (s_pPrev->find(name) != s_pPrev->end()) ? (*s_pPrev)[name] : false;
		return now && !prev;
	}

	bool GetUp(const std::string& name)
	{
		const bool now = Get(name);
		const bool prev = (s_pPrev->find(name) != s_pPrev->end()) ? (*s_pPrev)[name] : false;
		return (!now) && prev;
	}
}

void ACSM_Input::Initialize()
{
	JoyshockWrapper::Instance().Initialize();
	ACS_Mouse::Initialize();
	ASC_VirtualKey::Initialize();
}
void ACSM_Input::Finalize()
{
	ACS_Mouse::Finalize();
	ASC_VirtualKey::Finalize();
	JoyshockWrapper::Instance().Destroy();
	KeyWrapper::Instance().Destroy();
}
void ACSM_Input::Update()
{
	JoyshockWrapper::Instance().Update();
	KeyWrapper::Instance().Update();
	ACS_Mouse::Update();
	ASC_VirtualKey::Update();
}
bool ACSM_Input::GetKey(int key)
{
	return KeyWrapper::Instance().GetKey(key);
}
bool ACSM_Input::GetKeyDown(int key)
{
	return KeyWrapper::Instance().GetKeyDown(key);
}
bool ACSM_Input::GetKeyUp(int key)
{
	return KeyWrapper::Instance().GetKeyUp(key);
}
bool ACSM_Input::GetKey(KeyCode key)
{
	return KeyWrapper::Instance().GetKey(key);
}
bool ACSM_Input::GetKeyDown(KeyCode key)
{
	return KeyWrapper::Instance().GetKeyDown(key);
}
bool ACSM_Input::GetKeyUp(KeyCode key)
{
	return KeyWrapper::Instance().GetKeyUp(key);
}
bool ACSM_Input::GetButton(int button)
{
	return JoyshockWrapper::Instance().GetRawButton(button);
}
bool ACSM_Input::GetButtonDown(int button)
{
	return JoyshockWrapper::Instance().GetRawButtonDown(button);
}
bool ACSM_Input::GetButtonUp(int button)
{
	return JoyshockWrapper::Instance().GetRawButtonUp(button);
}
void ACSM_Input::GetLeftStick(float& outLX, float& outLY)
{
	JoyshockWrapper::Instance().GetLeftStick(outLX, outLY);
}
void ACSM_Input::GetRightStick(float& outRX, float& outRY)
{
	JoyshockWrapper::Instance().GetRightStick(outRX, outRY);
}
void ACSM_Input::GetTriggers(float& outL, float& outR)
{
	JoyshockWrapper::Instance().GetTriggers(outL, outR);
}
void ACSM_Input::GetAndFlushGyro(float& outX, float& outY, float& outZ)
{
	JoyshockWrapper::Instance().GetAndFlushGyro(outX, outY, outZ);
}

bool ACSM_Input::SubscribeVirtualKey(const std::string& key, int button)
{
	using namespace ASC_VirtualKey;

	if (button <= 0xFF)
	{
		Binding b{};
		b.type = DeviceType::Keyboard;
		b.code = static_cast<unsigned int>(button);
		b.deviceIndex = 0;
		return Subscribe(key, b);
	}
	else
	{
		Binding b{};
		b.type = DeviceType::Pad;
		b.code = static_cast<unsigned int>(button); // JSMASK_*
		b.deviceIndex = 0; // 必要なら将来拡張
		return Subscribe(key, b);
	}
}

bool ACSM_Input::GetVirtualKey(const std::string& key)
{
	return ASC_VirtualKey::Get(key);
}

bool ACSM_Input::GetVirtualKeyDown(const std::string& key)
{
	return ASC_VirtualKey::GetDown(key);
}

bool ACSM_Input::GetVirtualKeyUp(const std::string& key)
{
	return ASC_VirtualKey::GetUp(key);
}

void ACSM_Input::SubscribeVirtualKeyFromJson(const std::string& path)
{
	using namespace ASC_VirtualKey;
	using JSON = nlohmann::json;

	JSON json;
	std::ifstream file(path);

	file >> json;

	auto keys = json["Keys"];

	for (auto& key : keys)
	{
		std::string virtualKeyName = key["VirtualKeyName"];
		auto values = key["Values"];
		for (auto& value : values)
		{
			std::string deviceName = value["Device"];
			std::string keyName = value["KeyName"];

			DeviceType device = ACSM_Enum::ToEnum<DeviceType>(deviceName, DeviceType::None);
			switch (device)
			{
			case DeviceType::Keyboard:
			{
				KeyCode keyCode = ACSM_Enum::ToEnum(keyName, KeyCode::NONE);
				if (keyCode != KeyCode::NONE)
					SubscribeVirtualKey(virtualKeyName, keyCode);
			}
			break;
			case DeviceType::Pad:
			{
				uint32_t mask = 0;
				if (TryParsePadName(keyName, mask))
				{
					SubscribeVirtualKey(virtualKeyName, static_cast<int>(mask));
				}
			}
			break;
			default:
				break;
			}
		}
	}
}

#ifdef ACSM_DXLIB

POINT ACSM_Input::GetMousePoint()
{
	POINT pos{};
	int tmpX, tmpY;
	DxLib::GetMousePoint(&tmpX, &tmpY);
	pos = { (LONG)tmpX, (LONG)tmpY };
	return pos;
}

bool ACSM_Input::GetMouseButton(MouseButton button)
{
	return (*ACS_Mouse::g_pButtons)[button];
}

bool ACSM_Input::GetMouseButtonDown(MouseButton button)
{
	return (*ACS_Mouse::g_pButtons)[button] && !(*ACS_Mouse::g_pButtonsPrev)[button];
}

bool ACSM_Input::GetMouseButtonUp(MouseButton button)
{
	return !(*ACS_Mouse::g_pButtons)[button] && (*ACS_Mouse::g_pButtonsPrev)[button];
}

void ACSM_Input::SetMousePosition(int x, int y)
{
	SetMousePoint(x, y);
}

void ACSM_Input::SetMouseVisible(bool visible)
{
	SetMouseDispFlag(visible ? TRUE : FALSE);
}
#endif