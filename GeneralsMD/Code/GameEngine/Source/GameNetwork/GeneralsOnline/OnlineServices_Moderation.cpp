#include "GameNetwork/GeneralsOnline/OnlineServices_Moderation.h"

#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
#include "GameNetwork/GameSpyOverlay.h"

#include <stdexcept>
#include <windows.h>
#include <shellapi.h>

namespace
{
	enum class EModerationDialogContext
	{
		LOGIN,
		ACTIVE_SESSION
	};

	void LeaveLoginScreen()
	{
		TheShell->pop();
	}

	void OpenDiscord()
	{
		ShellExecuteA(nullptr, "open", "https://discord.playgenerals.online", nullptr, nullptr, SW_SHOWNORMAL);
	}

	void OpenDiscordFromLogin()
	{
		OpenDiscord();
		LeaveLoginScreen();
	}

	std::wstring GetNormalizedReason(const std::string& reason)
	{
		try
		{
			return NormalizeSingleLineText(from_utf8(reason));
		}
		catch (const std::range_error&)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[MODERATION]: Ignoring an invalid UTF-8 reason");
			return std::wstring();
		}
	}

	void AppendReason(std::wstring& message, const std::string& reason)
	{
		const std::wstring normalizedReason = GetNormalizedReason(reason);
		if (!normalizedReason.empty())
		{
			message += L"\n\nReason: ";
			message += normalizedReason;
		}
	}

	void ShowModerationDialog(
		UnicodeString title,
		std::wstring message,
		const std::string& reason,
		EModerationDialogContext context)
	{
		AppendReason(message, reason);
		message += L"\n\nVisit Discord for more information or support.";

		GameWinMsgBoxFunc discordCallback = OpenDiscord;
		GameWinMsgBoxFunc closeCallback = nullptr;
		if (context == EModerationDialogContext::LOGIN)
		{
			discordCallback = OpenDiscordFromLogin;
			closeCallback = LeaveLoginScreen;
		}

		GSMessageBoxOkCancelWithLabels(
			title,
			UnicodeString(message.c_str()),
			UnicodeString(L"Discord"),
			UnicodeString(L"Close"),
			discordCallback,
			closeCallback);
	}

	void ShowBanDialog(const std::string& reason, EModerationDialogContext context)
	{
		ShowModerationDialog(
			UnicodeString(L"Banned"),
			L"You have been banned from Generals Online.",
			reason,
			context);
	}

	void ShowKickDialog(const std::string& reason)
	{
		ShowModerationDialog(
			UnicodeString(L"Kicked"),
			L"You have been kicked from Generals Online.",
			reason,
			EModerationDialogContext::ACTIVE_SESSION);
	}
}

void ShowLoginBanDialog(const std::string& reason)
{
	ShowBanDialog(reason, EModerationDialogContext::LOGIN);
}

void HandleModerationDisconnect(EOnlineModerationAction action, const std::string& reason)
{
	NGMP_OnlineServicesManager* manager = NGMP_OnlineServicesManager::GetInstance();
	if (manager == nullptr || IsModerationTeardownReason(manager->GetTeardownReason()))
	{
		return;
	}

	switch (action)
	{
	case EOnlineModerationAction::BAN:
		manager->SetPendingFullTeardown(EGOTearDownReason::MODERATION_BAN);
		ShowBanDialog(reason, EModerationDialogContext::ACTIVE_SESSION);
		break;

	case EOnlineModerationAction::KICK:
		manager->SetPendingFullTeardown(EGOTearDownReason::MODERATION_KICK);
		ShowKickDialog(reason);
		break;
	}
}
