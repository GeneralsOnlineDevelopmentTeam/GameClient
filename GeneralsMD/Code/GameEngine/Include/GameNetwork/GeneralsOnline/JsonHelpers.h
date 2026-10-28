#pragma once

#include "GameNetwork/GeneralsOnline/json.hpp"

// Optional field: dest keeps its default when the key is missing or null
template <typename T>
inline void JsonGetOptional(const nlohmann::json& j, const char* szKey, T& dest)
{
	auto it = j.find(szKey);
	if (it != j.end() && !it->is_null())
	{
		it->get_to(dest);
	}
}

// Required field: throws when the key is missing or the wrong type
template <typename T>
inline void JsonGetRequired(const nlohmann::json& j, const char* szKey, T& dest)
{
	j.at(szKey).get_to(dest);
}
