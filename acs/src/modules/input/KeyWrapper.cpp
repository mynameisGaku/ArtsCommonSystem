#include "KeyWrapper.h"

#include <Windows.h>
#include <cstring>

KeyWrapper* KeyWrapper::m_pInstance = nullptr;

KeyWrapper& KeyWrapper::Instance()
{
	if (not m_pInstance)
	{
		m_pInstance = new KeyWrapper;
	}
	return *m_pInstance;
}

KeyWrapper::KeyWrapper()
{
	std::memset(m_KeyState, 0, sizeof(m_KeyState));
	std::memset(m_KeyStatePrev, 0, sizeof(m_KeyStatePrev));

	BYTE kb[256]{};
	if (::GetKeyboardState(kb))
	{
		std::memcpy(m_KeyState, kb, sizeof(m_KeyState));
	}
}

KeyWrapper::~KeyWrapper()
{
}

void KeyWrapper::Destroy()
{
	if (m_pInstance)
	{
		delete m_pInstance;
	}
	m_pInstance = nullptr;
}

void KeyWrapper::Uninitialize()
{
	std::memset(m_KeyState, 0, sizeof(m_KeyState));
	std::memset(m_KeyStatePrev, 0, sizeof(m_KeyStatePrev));
}

void KeyWrapper::Update()
{
	std::memcpy(m_KeyStatePrev, m_KeyState, sizeof(m_KeyStatePrev));

	BYTE kb[256]{};
	if (::GetKeyboardState(kb))
	{
		std::memcpy(m_KeyState, kb, sizeof(m_KeyState));
	}
}

bool KeyWrapper::GetKey(unsigned char mask)
{
	return (m_KeyState[mask] & 0x80u) != 0u;
}

bool KeyWrapper::GetKeyDown(unsigned char mask)
{
	const bool now = (m_KeyState[mask] & 0x80u) != 0u;
	const bool prev = (m_KeyStatePrev[mask] & 0x80u) != 0u;
	return now && !prev;
}

bool KeyWrapper::GetKeyUp(unsigned char mask)
{
	const bool now = (m_KeyState[mask] & 0x80u) != 0u;
	const bool prev = (m_KeyStatePrev[mask] & 0x80u) != 0u;
	return !now && prev;
}

bool KeyWrapper::GetKey(KeyCode code)
{
	return GetKey(static_cast<unsigned char>(code));
}

bool KeyWrapper::GetKeyDown(KeyCode code)
{
	return GetKeyDown(static_cast<unsigned char>(code));
}

bool KeyWrapper::GetKeyUp(KeyCode code)
{
	return GetKeyUp(static_cast<unsigned char>(code));
}