#pragma once
#include "Pch.h"

namespace ACSU_Enum
{
	/// <summary>
	/// string -> enum
	/// 変換失敗時のデフォルト値を指定できる
	/// </summary>
	template<typename C>
	C ToEnum(const std::string& codeStr, C defaultValue)
	{
		auto key = magic_enum::enum_cast<C>(codeStr);
		if (key.has_value())
		{
			return key.value();
		}
		return defaultValue;
	}

	template<typename C>
	std::enable_if_t<std::is_enum_v<C>, std::string_view>
		ToStringView(C value)
	{
		return magic_enum::enum_name(value);
	}

	template<typename C>
	std::enable_if_t<std::is_enum_v<C>, std::string>
		ToString(C value)
	{
		auto strv = ToStringView(value);
		return std::string(strv.begin(), strv.end());
	}
}