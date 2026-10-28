#include "PreRTS.h"

#include "GameNetwork/GeneralsOnline/ProxyLogin.h"
#include "GameNetwork/GeneralsOnline/ServiceConnectionPolicy.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/KeyDefs.h"
#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// One dialog for the login of a proxy and the connection test, laid out in Menus/ProxyLogin.wnd in the frame of the game's message box.
// States: testing, form, checking, result. The form offers the Windows login where the proxy can use it (ticked when the silent one was not accepted) or typed credentials.
namespace ServiceConnection
{
namespace
{
std::mutex mainThreadMutex;
std::deque<std::function<void()>> mainThreadTasks;

enum class State { Testing, Form, Checking, Result };

struct Dialog
{
	WindowLayout *layout = nullptr;
	GameWindow *parent = nullptr;
	GameWindow *title = nullptr;
	GameWindow *message = nullptr;
	GameWindow *proxyLabel = nullptr;
	GameWindow *proxy = nullptr;
	GameWindow *userName = nullptr;
	GameWindow *userLabel = nullptr;
	GameWindow *password = nullptr;
	GameWindow *passwordLabel = nullptr;
	GameWindow *windows = nullptr;
	GameWindow *remember = nullptr;
	GameWindow *ok = nullptr;
	GameWindow *save = nullptr;
	GameWindow *cancel = nullptr;
	GameWindow *previousFocus = nullptr;
	std::vector<GameWindow *> coveredBoxes;
	State state = State::Testing;
	bool testPath = false;
	bool acceptMode = false;
	bool failed = false;
	bool windowsLoginPossible = true;
	unsigned generation = 0;
	std::string proxyURL;
	ServiceConnectionSettings settings;
	std::string pendingUser;
	std::string pendingPassword;
	bool pendingRemember = false;
	std::function<void(bool)> done;
	std::function<void(AcceptChoice)> decided;
};

Dialog dialog;

const char *environmentName()
{
	return NGMP_OnlineServicesManager::g_Environment == NGMP_OnlineServicesManager::EEnvironment::DEV ? "dev" :
		NGMP_OnlineServicesManager::g_Environment == NGMP_OnlineServicesManager::EEnvironment::TEST ? "test" : "prod";
}

GameWindow *control(const char *name)
{
	return TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(name));
}

UnicodeString wideText(const std::string &text)
{
	UnicodeString result;
	result.translate(text.c_str());
	return result;
}

std::string narrowText(const UnicodeString &text)
{
	AsciiString ascii;
	ascii.translate(text);
	return ascii.str();
}

void wipeString(std::string &value)
{
	if (!value.empty())
	{
		SecureZeroMemory(value.data(), value.size());
	}
	value.clear();
}

std::string upperText(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)toupper(c); });
	return text;
}

Int measure(GameFont *font, const std::string &text)
{
	DisplayString *string = TheDisplayStringManager->newDisplayString();
	string->setFont(font);
	string->setText(wideText(text));
	const Int width = string->getWidth();
	TheDisplayStringManager->freeDisplayString(string);
	return width;
}

void updateFormEnabled()
{
	const bool editable = dialog.state == State::Form;
	// with the Windows login the typed credentials are not used, so their fields are greyed
	const bool typed = editable && GadgetCheckBoxIsChecked(dialog.windows) == FALSE;
	dialog.windows->winEnable(editable && dialog.windowsLoginPossible);
	dialog.userName->winEnable(typed);
	dialog.password->winEnable(typed);
	dialog.remember->winEnable(typed);
	dialog.ok->winEnable(dialog.state == State::Form || dialog.state == State::Result);
	// after a plain result OK is the only way out
	dialog.cancel->winEnable(dialog.state != State::Result || (dialog.acceptMode && dialog.failed));
}

// a label that fits the button, otherwise the short one
std::string fittingLabel(GameWindow *button, const std::string &label, const std::string &shortLabel)
{
	Int width = 0;
	Int height = 0;
	button->winGetSize(&width, &height);
	return measure(button->winGetFont(), label) + TheWindowManager->winFontHeight(button->winGetFont()) <= width ? label : shortLabel;
}

void setState(State state, const std::string &title, const UnicodeString &message, bool isError = false)
{
	dialog.state = state;
	const bool form = state == State::Form || state == State::Checking;
	const bool failedAccept = state == State::Result && dialog.acceptMode && dialog.failed;
	GadgetButtonSetText(dialog.ok, TheGameText->fetch(failedAccept ? "GUI:Back" : "GUI:Ok"));
	GadgetButtonSetText(dialog.save, wideText(fittingLabel(dialog.save, "SAVE ANYWAY", "SAVE")));

	GadgetStaticTextSetText(dialog.title, wideText(upperText(title)));
	GadgetStaticTextSetText(dialog.message, message);
	dialog.message->winSetEnabledTextColors(isError ? GameMakeColor(255, 96, 96, 255) : GameMakeColor(255, 255, 255, 255), GameMakeColor(0, 0, 0, 255));
	for (GameWindow *window : { dialog.proxyLabel, dialog.proxy, dialog.windows, dialog.userLabel, dialog.userName, dialog.passwordLabel, dialog.password, dialog.remember })
	{
		window->winHide(!form);
	}
	dialog.save->winHide(!failedAccept);
	updateFormEnabled();
}

// A modal message box (such as the update check) would stay above the dialog, so those boxes are hidden while it is open
void hideMessageBoxes()
{
	const NameKeyType boxKey = TheNameKeyGenerator->nameToKey("MessageBox.wnd:MessageBoxParent");
	for (GameWindow *top = TheWindowManager->winGetWindowList(); top != nullptr; top = top->winGetNext())
	{
		if (top != dialog.parent && !top->winIsHidden() && TheWindowManager->winGetWindowFromId(top, boxKey) != nullptr)
		{
			dialog.coveredBoxes.push_back(top);
			top->winHide(TRUE);
		}
	}
}

// only boxes that are still open get shown again
void showMessageBoxes(const std::vector<GameWindow *> &boxes)
{
	for (GameWindow *top = TheWindowManager->winGetWindowList(); top != nullptr; top = top->winGetNext())
	{
		if (std::find(boxes.begin(), boxes.end(), top) != boxes.end())
		{
			top->winHide(FALSE);
		}
	}
}

// ---- results ---------------------------------------------------------------------------------------------------------------------

void finishDialog(bool loggedIn, AcceptChoice choice = AcceptChoice::KeepEditing)
{
	std::function<void(bool)> done = std::move(dialog.done);
	std::function<void(AcceptChoice)> decided = std::move(dialog.decided);
	GameWindow *previousFocus = dialog.previousFocus;
	std::vector<GameWindow *> coveredBoxes = std::move(dialog.coveredBoxes);
	TheWindowManager->winUnsetModal(dialog.parent);
	GadgetTextEntrySetText(dialog.password, UnicodeString::TheEmptyString);
	wipeString(dialog.pendingPassword);
	dialog.layout->destroyWindows();
	deleteInstance(dialog.layout);
	const unsigned generation = dialog.generation + 1;
	dialog = Dialog();
	dialog.generation = generation;
	showMessageBoxes(coveredBoxes);
	if (previousFocus != nullptr)
	{
		TheWindowManager->winSetFocus(previousFocus);
	}
	if (done)
	{
		done(loggedIn);
	}
	if (decided)
	{
		decided(choice);
	}
}

// the proxy accepted the login: keep it as the player chose
void commitLogin()
{
	std::string error;
	if (dialog.pendingRemember)
	{
		ClearSessionProxyCredentials(dialog.proxyURL);
		if (!SaveProxyCredentials(dialog.proxyURL, dialog.pendingUser, dialog.pendingPassword, error))
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[Connection] Unable to save the proxy login: %s", error.c_str());
			SetSessionProxyCredentials(dialog.proxyURL, dialog.pendingUser, dialog.pendingPassword);
		}
	}
	wipeString(dialog.pendingPassword);
}

void showForm(const UnicodeString &errorText, bool clearPassword)
{
	GadgetTextEntrySetText(dialog.proxy, wideText(ProxyDisplayName(dialog.proxyURL)));
	setState(State::Form, "Proxy login", errorText, true);
	if (clearPassword)
	{
		GadgetTextEntrySetText(dialog.password, UnicodeString::TheEmptyString);
	}
	TheWindowManager->winSetFocus(GadgetCheckBoxIsChecked(dialog.windows) ? dialog.ok : narrowText(GadgetTextEntryGetText(dialog.userName)).empty() ? dialog.userName : dialog.password);
}

void startProbe(ServiceConnectionSettings settings)
{
	const unsigned generation = ++dialog.generation;
	const std::string environment = environmentName();
	std::thread([generation, settings, environment]()
	{
		ConnectionTestResult result;
		try
		{
			result = TestServiceConnection(settings, environment, 12);
		}
		catch (...)
		{
		}
		RunOnMainThread([generation, result]()
		{
			if (dialog.layout == nullptr || dialog.generation != generation)
			{
				return;
			}
			using Outcome = ConnectionTestResult::Outcome;
			if (dialog.state == State::Testing)
			{
				if (result.outcome == Outcome::ProxyAuthRequired && !result.proxyURL.empty())
				{
					dialog.proxyURL = result.proxyURL;
					dialog.windowsLoginPossible = result.proxyIntegratedAuthPossible;
					if (!dialog.windowsLoginPossible)
					{
						GadgetCheckBoxSetChecked(dialog.windows, FALSE);
					}
					const std::string savedUser = ProxyUsername(result.proxyURL);
					GadgetTextEntrySetText(dialog.userName, wideText(savedUser));
					GadgetCheckBoxSetChecked(dialog.remember, HasProxyCredentials(result.proxyURL));
					showForm(result.detail == "refused" ? TheGameText->fetch("GUI:GSDisconReason8") : UnicodeString::TheEmptyString, false);
				}
				else if (dialog.acceptMode && result.outcome == Outcome::Connected)
				{
					finishDialog(true, AcceptChoice::Save);
				}
				else
				{
					dialog.failed = result.outcome != Outcome::Connected;
					setState(State::Result, "Connection test", wideText(DescribeConnectionTest(result)));
				}
				return;
			}
			// checking a login
			if (result.outcome == Outcome::ProxyAuthRequired)
			{
				ClearSessionProxyCredentials(dialog.proxyURL);
				wipeString(dialog.pendingPassword);
				showForm(GadgetCheckBoxIsChecked(dialog.windows) ? wideText("The Windows login was not accepted.") : TheGameText->fetch("GUI:GSDisconReason8"), true);
				return;
			}
			if (result.outcome == Outcome::Unreachable && !result.proxyAnswered)
			{
				ClearSessionProxyCredentials(dialog.proxyURL);
				wipeString(dialog.pendingPassword);
				showForm(TheGameText->fetch("GUI:ConnectionFailed"), false);
				return;
			}
			commitLogin();
			if (!dialog.testPath)
			{
				finishDialog(true);
				return;
			}
			if (dialog.acceptMode && result.outcome == Outcome::Connected)
			{
				finishDialog(true, AcceptChoice::Save);
				return;
			}
			dialog.failed = result.outcome != Outcome::Connected;
			setState(State::Result, "Connection test",
				wideText(result.outcome == Outcome::Connected ? DescribeConnectionTest(result) :
				result.outcome == Outcome::Unreachable ? "Logged in to the proxy, but could not reach the service." :
				"Logged in to the proxy. " + DescribeConnectionTest(result)));
		});
	}).detach();
}

// switches to the checking state; stays on the form when the input is incomplete
void acceptForm()
{
	const bool windowsLogin = GadgetCheckBoxIsChecked(dialog.windows) != FALSE;
	// the choice is kept, and the check below uses it; a proxy that cannot use the Windows login leaves it as it was
	if (dialog.windowsLoginPossible)
	{
		NGMP_OnlineServicesManager::Settings.Network_SaveWindowsLogin(windowsLogin);
		dialog.settings.windowsLogin = windowsLogin;
	}
	if (windowsLogin)
	{
		ClearSessionProxyCredentials(dialog.proxyURL);
		dialog.pendingUser.clear();
		wipeString(dialog.pendingPassword);
		dialog.pendingRemember = false;
		setState(State::Checking, "Proxy login", TheGameText->fetch("WOL:Connecting"));
		startProbe(dialog.testPath ? dialog.settings : NGMP_OnlineServicesManager::Settings.Network_GetConnectionSettings());
		return;
	}
	std::string user = narrowText(GadgetTextEntryGetText(dialog.userName));
	std::string password = narrowText(GadgetTextEntryGetText(dialog.password));
	if (user.empty())
	{
		showForm(wideText("Enter the user name."), false);
		wipeString(password);
		return;
	}
	dialog.pendingUser = user;
	dialog.pendingPassword = password;
	dialog.pendingRemember = GadgetCheckBoxIsChecked(dialog.remember) != FALSE;
	wipeString(password);
	GadgetTextEntrySetText(dialog.password, UnicodeString::TheEmptyString);

	// the login is used for the check first; it is kept once the proxy accepted it
	SetSessionProxyCredentials(dialog.proxyURL, dialog.pendingUser, dialog.pendingPassword);
	setState(State::Checking, "Proxy login", TheGameText->fetch("WOL:Connecting"));
	startProbe(dialog.testPath ? dialog.settings : NGMP_OnlineServicesManager::Settings.Network_GetConnectionSettings());
}

void cancelDialog()
{
	if (dialog.state == State::Checking)
	{
		ClearSessionProxyCredentials(dialog.proxyURL);
	}
	// Cancel after a failed check drops the connection changes, earlier it only stops the check
	finishDialog(false, dialog.acceptMode && dialog.state == State::Result ? AcceptChoice::Discard : AcceptChoice::KeepEditing);
}

WindowMsgHandledType dialogSystem(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
{
	if (dialog.layout == nullptr)
	{
		return MSG_IGNORED;
	}
	switch (msg)
	{
		case GWM_INPUT_FOCUS:
		{
			if (mData1 == TRUE)
			{
				*(Bool *)mData2 = TRUE;
			}
			return MSG_HANDLED;
		}
		case GBM_SELECTED:
		{
			GameWindow *source = (GameWindow *)mData1;
			if (source == dialog.ok)
			{
				if (dialog.state == State::Form)
				{
					acceptForm();
				}
				else if (dialog.state == State::Result)
				{
					// with a failed check before Accept this button is "Keep editing"
					finishDialog(!dialog.acceptMode, AcceptChoice::KeepEditing);
				}
			}
			else if (source == dialog.windows)
			{
				updateFormEnabled();
			}
			else if (source == dialog.save)
			{
				finishDialog(true, AcceptChoice::Save);
			}
			else if (source == dialog.cancel)
			{
				cancelDialog();
			}
			return MSG_HANDLED;
		}
		case GEM_EDIT_DONE:
		{
			GameWindow *source = (GameWindow *)mData1;
			if (dialog.state != State::Form)
			{
				return MSG_HANDLED;
			}
			if (source == dialog.userName)
			{
				TheWindowManager->winSetFocus(dialog.password);
			}
			else if (source == dialog.password)
			{
				acceptForm();
			}
			return MSG_HANDLED;
		}
	}
	return MSG_IGNORED;
}

WindowMsgHandledType dialogInput(GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
{
	if (dialog.layout == nullptr)
	{
		return MSG_IGNORED;
	}
	if (msg == GWM_CHAR)
	{
		if ((UnsignedByte)mData1 == KEY_ESC)
		{
			if (BitIsSet((UnsignedByte)mData2, KEY_STATE_UP))
			{
				cancelDialog();
			}
			return MSG_HANDLED;
		}
	}
	return MSG_IGNORED;
}

// builds the dialog; returns false when the layout could not be built
bool openDialog(bool testPath)
{
	if (dialog.layout != nullptr || TheWindowManager == nullptr)
	{
		return false;
	}
	WindowLayout *layout = TheWindowManager->winCreateLayout(AsciiString("Menus/ProxyLogin.wnd"));
	if (layout == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Connection] The proxy login layout Menus/ProxyLogin.wnd could not be loaded");
		return false;
	}
	Dialog d;
	d.generation = dialog.generation;
	d.layout = layout;
	d.parent = control("ProxyLogin.wnd:ProxyLoginRoot");
	d.title = control("ProxyLogin.wnd:StaticTextTitle");
	d.message = control("ProxyLogin.wnd:StaticTextMessage");
	d.proxyLabel = control("ProxyLogin.wnd:StaticTextProxy");
	d.proxy = control("ProxyLogin.wnd:TextEntryProxy");
	d.userLabel = control("ProxyLogin.wnd:StaticTextUserName");
	d.userName = control("ProxyLogin.wnd:TextEntryUserName");
	d.passwordLabel = control("ProxyLogin.wnd:StaticTextPassword");
	d.password = control("ProxyLogin.wnd:TextEntryPassword");
	d.windows = control("ProxyLogin.wnd:CheckBoxWindowsLogin");
	d.remember = control("ProxyLogin.wnd:CheckBoxRemember");
	d.ok = control("ProxyLogin.wnd:ButtonOk");
	d.save = control("ProxyLogin.wnd:ButtonSave");
	d.cancel = control("ProxyLogin.wnd:ButtonCancel");
	for (GameWindow *window : { d.parent, d.title, d.message, d.proxyLabel, d.proxy, d.userLabel, d.userName, d.passwordLabel, d.password, d.windows, d.remember, d.ok, d.save, d.cancel })
	{
		if (window == nullptr)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[Connection] The proxy login layout Menus/ProxyLogin.wnd is missing a control");
			layout->destroyWindows();
			deleteInstance(layout);
			return false;
		}
	}
	d.testPath = testPath;
	d.previousFocus = TheWindowManager->winGetFocus();
	dialog = std::move(d);

	GadgetStaticTextSetText(dialog.proxyLabel, UnicodeString(L"Proxy"));
	GadgetStaticTextSetText(dialog.userLabel, UnicodeString(L"Username"));
	dialog.proxy->winEnable(FALSE);
	GadgetStaticTextSetText(dialog.passwordLabel, TheGameText->fetch("GUI:Password"));
	GadgetCheckBoxSetText(dialog.remember, TheGameText->fetch("GUI:RememberPassword"));
	GadgetCheckBoxSetText(dialog.windows, UnicodeString(L"Use Windows login"));
	dialog.windows->winSetTooltip(UnicodeString(L"Log in to the proxy with the Windows account of this PC. Untick it to type a user name and password instead."));
	GadgetButtonSetText(dialog.ok, TheGameText->fetch("GUI:Ok"));
	GadgetButtonSetText(dialog.cancel, TheGameText->fetch("GUI:Cancel"));
	dialog.userName->winSetTooltip(UnicodeString(L"The proxy user name, often DOMAIN\\name."));
	dialog.password->winSetTooltip(UnicodeString(L"The proxy password."));
	dialog.remember->winSetTooltip(UnicodeString(L"Save this login for next time on this PC. Otherwise it is only used until the game closes."));
	GadgetCheckBoxSetChecked(dialog.remember, FALSE);
	GadgetTextEntrySetText(dialog.password, UnicodeString::TheEmptyString);

	dialog.parent->winSetSystemFunc(dialogSystem);
	dialog.parent->winSetInputFunc(dialogInput);
	return true;
}

void showDialog()
{
	hideMessageBoxes();
	dialog.layout->hide(FALSE);
	dialog.layout->bringForward();
	dialog.parent->winBringToTop();
	TheWindowManager->winSetModal(dialog.parent);
	TheWindowManager->winSetFocus(dialog.state == State::Form ? dialog.userName : dialog.cancel);
}
}

void RunOnMainThread(std::function<void()> task)
{
	std::lock_guard<std::mutex> lock(mainThreadMutex);
	mainThreadTasks.push_back(std::move(task));
}

void PumpMainThreadTasks()
{
	std::deque<std::function<void()>> tasks;
	{
		std::lock_guard<std::mutex> lock(mainThreadMutex);
		tasks.swap(mainThreadTasks);
	}
	for (auto &task : tasks)
	{
		task();
	}
}

bool IsProxyLoginOpen()
{
	return dialog.layout != nullptr;
}

void RequestProxyLogin(const ProxyRoute &route, std::function<void(bool)> done)
{
	if (!openDialog(false))
	{
		done(false);
		return;
	}
	dialog.proxyURL = route.url;
	dialog.windowsLoginPossible = route.integratedAuthPossible;
	dialog.done = std::move(done);
	GadgetTextEntrySetText(dialog.userName, wideText(ProxyUsername(route.url)));
	GadgetCheckBoxSetChecked(dialog.remember, HasProxyCredentials(route.url));
	GadgetCheckBoxSetChecked(dialog.windows, route.integratedAuthPossible && (route.allowIntegratedAuth || NGMP_OnlineServicesManager::Settings.Network_GetConnectionSettings().windowsLogin));
	showForm(route.allowIntegratedAuth ? wideText("The Windows login was not accepted.") : UnicodeString::TheEmptyString, true);
	showDialog();
}

// Accept in Options: tests the new settings first and reports what to do with them
void ShowAcceptTest(const ServiceConnectionSettings &settings, std::function<void(AcceptChoice)> decided)
{
	if (!openDialog(true))
	{
		decided(AcceptChoice::Save);
		return;
	}
	dialog.acceptMode = true;
	dialog.settings = settings;
	GadgetCheckBoxSetChecked(dialog.windows, settings.windowsLogin);
	dialog.decided = std::move(decided);
	setState(State::Testing, "Connection test", TheGameText->fetch("WOL:Connecting"));
	showDialog();
	startProbe(settings);
}
}
