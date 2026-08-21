#pragma once

#include <string>

enum class EOnlineModerationAction
{
	BAN,
	KICK
};

void ShowLoginBanDialog(const std::string& reason);
void HandleModerationDisconnect(EOnlineModerationAction action, const std::string& reason);
