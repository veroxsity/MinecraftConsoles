#include "stdafx.h"
#include "UI.h"
#include "UIScene_LceLiveFriends.h"
#include "../../Minecraft.h"

// Fallback until string ID headers are regenerated.
#ifndef IDS_TEXT_REMOVE_FRIEND_CONFIRMATION
#define IDS_TEXT_REMOVE_FRIEND_CONFIRMATION IDS_TEXT_DELETE_SAVE
#endif

#ifdef _WINDOWS64
namespace
{
	std::wstring Utf8ToWideLocal(const std::string &text)
	{
		if (text.empty())
			return L"";

		const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
		if (required <= 0)
			return L"";

		std::wstring result(static_cast<size_t>(required), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &result[0], required);
		if (!result.empty() && result.back() == L'\0')
			result.pop_back();
		return result;
	}

	std::string WideToUtf8Local(const std::wstring &text)
	{
		if (text.empty())
			return std::string();

		const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (required <= 0)
			return std::string();

		std::string result(static_cast<size_t>(required), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &result[0], required, nullptr, nullptr);
		if (!result.empty() && result.back() == '\0')
			result.pop_back();
		return result;
	}

	std::wstring TrimWhitespaceLocal(const std::wstring &text)
	{
		const size_t begin = text.find_first_not_of(L" \t\r\n");
		if (begin == std::wstring::npos)
			return L"";

		const size_t end = text.find_last_not_of(L" \t\r\n");
		return text.substr(begin, end - begin + 1);
	}

	std::wstring BuildFriendLabel(const Win64LceLive::SocialEntry &entry)
	{
		std::wstring label;
		if (!entry.displayName.empty())
			label = Utf8ToWideLocal(entry.displayName);
		else if (!entry.username.empty())
			label = Utf8ToWideLocal(entry.username);

		if (!entry.username.empty())
		{
			if (!label.empty())
				label += L" (@";
			else
				label = L"@";

			label += Utf8ToWideLocal(entry.username);
			if (!entry.displayName.empty())
				label += L")";
		}

		if (label.empty())
			label = L"<unknown friend>";

		return label;
	}
}
#endif

UIScene_LceLiveFriends::UIScene_LceLiveFriends(int iPad, void *initData, UILayer *parentLayer) : UIScene(iPad, parentLayer)
{
	initialiseMovie();

	parentLayer->addComponent(iPad, eUIComponent_Panorama);
	parentLayer->addComponent(iPad, eUIComponent_Logo);

	m_friendsList.init(eControl_FriendsList);
	m_actionsList.init(eControl_ActionsList);
	m_labelFriendsTitle.init(L"FRIENDS");
	m_labelActionsTitle.init(L"ACTIONS");
	m_labelStatus.init(L"");
	m_controlFriendsTimer.setVisible(false);
	m_controlActionsTimer.setVisible(false);

	m_actionsList.addItem(L"ADD FRIEND");
	m_actionsList.addItem(L"REFRESH");
	m_actionsList.setCurrentSelection(eAction_AddFriend);

	m_bDataReady = false;
	m_statusMessage.clear();
#ifdef _WINDOWS64
	m_pendingRemovalAccountId.clear();
	m_pendingRemovalLabel.clear();
#endif

	doHorizontalResizeCheck();
	FetchAndDisplay();
}

UIScene_LceLiveFriends::~UIScene_LceLiveFriends()
{
	m_parentLayer->removeComponent(eUIComponent_Panorama);
	m_parentLayer->removeComponent(eUIComponent_Logo);
}

wstring UIScene_LceLiveFriends::getMoviePath()
{
	return L"LoadOrJoinMenu";
}

void UIScene_LceLiveFriends::updateTooltips()
{
	ui.SetTooltips(m_iPad, IDS_TOOLTIPS_SELECT, IDS_TOOLTIPS_BACK, IDS_TOOLTIPS_DELETE, IDS_TOOLTIPS_REFRESH);
}

void UIScene_LceLiveFriends::updateComponents()
{
	const bool notInGame = (Minecraft::GetInstance()->level == nullptr);
	m_parentLayer->showComponent(m_iPad, eUIComponent_Panorama, notInGame);
	m_parentLayer->showComponent(m_iPad, eUIComponent_Logo, true);
}

void UIScene_LceLiveFriends::handleReload()
{
	doHorizontalResizeCheck();
	m_controlFriendsTimer.setVisible(false);
	m_controlActionsTimer.setVisible(false);
	FetchAndDisplay();
}

void UIScene_LceLiveFriends::handleFocusChange(F64 controlId, F64 childId)
{
	if (static_cast<int>(controlId) == eControl_FriendsList)
		m_friendsList.updateChildFocus(static_cast<int>(childId));
	else if (static_cast<int>(controlId) == eControl_ActionsList)
		m_actionsList.updateChildFocus(static_cast<int>(childId));

	updateTooltips();
}

void UIScene_LceLiveFriends::handleInput(int iPad, int key, bool repeat, bool pressed, bool released, bool &handled)
{
	ui.AnimateKeyPress(m_iPad, key, repeat, pressed, released);

	switch (key)
	{
	case ACTION_MENU_CANCEL:
		if (pressed && !repeat)
		{
			ui.PlayUISFX(eSFX_Back);
			navigateBack();
		}
		break;
	case ACTION_MENU_OK:
#ifdef __ORBIS__
	case ACTION_MENU_TOUCHPAD_PRESS:
#endif
		if (pressed)
			ui.PlayUISFX(eSFX_Press);
		if (pressed && !repeat)
		{
			handled = true;
			if (controlHasFocus(eControl_ActionsList))
				PerformSelectedAction();
			else if (controlHasFocus(eControl_FriendsList))
				PromptRemoveSelectedFriend();
		}
		break;
	case ACTION_MENU_X:
		if (pressed && !repeat)
		{
#ifdef _WINDOWS64
			PromptRemoveSelectedFriend();
#endif
			handled = true;
		}
		break;
	case ACTION_MENU_Y:
		if (pressed && !repeat)
		{
			FetchAndDisplay();
			handled = true;
		}
		break;
	case ACTION_MENU_UP:
	case ACTION_MENU_DOWN:
	case ACTION_MENU_LEFT:
	case ACTION_MENU_RIGHT:
	case ACTION_MENU_PAGEUP:
	case ACTION_MENU_PAGEDOWN:
		sendInputToMovie(key, repeat, pressed, released);
		break;
	}
}

void UIScene_LceLiveFriends::handlePress(F64 controlId, F64 childId)
{
	if (static_cast<int>(controlId) == eControl_FriendsList)
	{
		m_friendsList.updateChildFocus(static_cast<int>(childId));
		PromptRemoveSelectedFriend();
	}
	else if (static_cast<int>(controlId) == eControl_ActionsList)
	{
		m_actionsList.updateChildFocus(static_cast<int>(childId));
		PerformSelectedAction();
	}
}

void UIScene_LceLiveFriends::FetchAndDisplay()
{
#ifdef _WINDOWS64
	const std::string accessToken = Win64LceLive::GetAccessToken();
	if (accessToken.empty())
	{
		m_friends.clear();
		m_bDataReady = true;
		m_statusMessage = L"Sign in to view and manage friends.";
		RebuildLists();
		return;
	}

	const Win64LceLive::FriendsListResult result = Win64LceLive::GetFriendsSync();
	if (!result.success)
	{
		m_friends.clear();
		m_bDataReady = true;
		m_statusMessage = Utf8ToWideLocal(result.error);
		RebuildLists();
		return;
	}

	const int previousSelection = m_friendsList.getCurrentSelection();
	m_friends = result.friends;
	m_bDataReady = true;

	if (m_friends.empty() && m_statusMessage.empty())
		m_statusMessage = L"No friends yet. Use ACTIONS to add one.";
	else if (!m_friends.empty() && m_statusMessage == L"No friends yet. Use ACTIONS to add one.")
		m_statusMessage.clear();

	RebuildLists();

	if (!m_friends.empty())
	{
		int newSelection = previousSelection;
		if (newSelection < 0)
			newSelection = 0;
		if (newSelection >= static_cast<int>(m_friends.size()))
			newSelection = static_cast<int>(m_friends.size()) - 1;
		m_friendsList.setCurrentSelection(newSelection);
	}
#else
	m_friends.clear();
	m_bDataReady = true;
	m_statusMessage = L"Friends are only available on Windows64 builds.";
	RebuildLists();
#endif
}

void UIScene_LceLiveFriends::RebuildLists()
{
	m_friendsList.clearList();

#ifdef _WINDOWS64
	for (const Win64LceLive::SocialEntry &entry : m_friends)
		m_friendsList.addItem(BuildFriendLabel(entry));
#endif

	UpdateStatusLabel();
}

void UIScene_LceLiveFriends::UpdateStatusLabel()
{
	if (m_statusMessage.empty())
	{
		m_labelStatus.setVisible(false);
	}
	else
	{
		m_labelStatus.setLabel(m_statusMessage, true, true);
		m_labelStatus.setVisible(true);
	}
	m_labelFriendsTitle.setLabel(L"FRIENDS", true, true);
	m_labelActionsTitle.setLabel(L"ACTIONS", true, true);
}

int UIScene_LceLiveFriends::FocusedFriendIndex()
{
	return m_friendsList.getCurrentSelection();
}

int UIScene_LceLiveFriends::SelectedActionIndex()
{
	return m_actionsList.getCurrentSelection();
}

void UIScene_LceLiveFriends::PerformSelectedAction()
{
	const int action = SelectedActionIndex();
	switch (action)
	{
	case eAction_AddFriend:
		OpenAddFriendKeyboard();
		break;
	case eAction_Refresh:
		FetchAndDisplay();
		break;
	default:
		break;
	}
}

#ifdef _WINDOWS64
void UIScene_LceLiveFriends::PromptRemoveSelectedFriend()
{
	PromptRemoveFriendAtIndex(FocusedFriendIndex());
}

void UIScene_LceLiveFriends::PromptRemoveFriendAtIndex(int friendIndex)
{
	if (friendIndex < 0 || friendIndex >= static_cast<int>(m_friends.size()))
	{
		m_statusMessage = L"Select a friend first.";
		UpdateStatusLabel();
		return;
	}

	m_pendingRemovalAccountId = m_friends[friendIndex].accountId;
	m_pendingRemovalLabel = BuildFriendLabel(m_friends[friendIndex]);

	UINT optionIds[2];
	optionIds[0] = IDS_NO;
	optionIds[1] = IDS_YES;

	ui.RequestAlertMessage(
		IDS_TOOLTIPS_DELETE,
		IDS_TEXT_REMOVE_FRIEND_CONFIRMATION,
		optionIds,
		2,
		m_iPad,
		&UIScene_LceLiveFriends::RemoveFriendConfirmCallback,
		this);
}

void UIScene_LceLiveFriends::RemovePendingFriend()
{
	if (m_pendingRemovalAccountId.empty())
		return;

	const Win64LceLive::SocialActionResult result = Win64LceLive::RemoveFriendSync(m_pendingRemovalAccountId);
	if (result.success)
	{
		m_statusMessage = L"Friend removed: ";
		m_statusMessage += m_pendingRemovalLabel;
	}
	else
	{
		m_statusMessage = Utf8ToWideLocal(result.error);
	}

	m_pendingRemovalAccountId.clear();
	m_pendingRemovalLabel.clear();
	FetchAndDisplay();
}

int UIScene_LceLiveFriends::RemoveFriendConfirmCallback(void *pParam, int iPad, C4JStorage::EMessageResult result)
{
	UIScene_LceLiveFriends *scene = static_cast<UIScene_LceLiveFriends *>(pParam);
	if (scene == nullptr)
		return 0;

	(void)iPad;

	if (result == C4JStorage::EMessage_ResultDecline)
		scene->RemovePendingFriend();
	else
	{
		scene->m_pendingRemovalAccountId.clear();
		scene->m_pendingRemovalLabel.clear();
	}

	return 0;
}

void UIScene_LceLiveFriends::OpenAddFriendKeyboard()
{
	UIKeyboardInitData kbData;
	kbData.title       = L"Add Friend";
	kbData.defaultText = L"";
	kbData.maxChars    = 32;
	kbData.callback    = &UIScene_LceLiveFriends::AddFriendKeyboardCallback;
	kbData.lpParam     = this;
	kbData.pcMode      = g_KBMInput.IsKBMActive();
	ui.NavigateToScene(m_iPad, eUIScene_Keyboard, &kbData);
}

int UIScene_LceLiveFriends::AddFriendKeyboardCallback(LPVOID lpParam, const bool bResult)
{
	UIScene_LceLiveFriends *scene = static_cast<UIScene_LceLiveFriends *>(lpParam);
	if (scene == nullptr || !bResult)
		return 0;

	uint16_t ui16Text[256] = {};
	Win64_GetKeyboardText(ui16Text, 256);

	wchar_t wBuf[256] = {};
	for (int k = 0; k < 255 && ui16Text[k]; ++k)
		wBuf[k] = static_cast<wchar_t>(ui16Text[k]);

	const std::wstring usernameW = TrimWhitespaceLocal(wBuf);
	if (usernameW.empty())
	{
		scene->m_statusMessage = L"Enter a username to send a friend request.";
		scene->UpdateStatusLabel();
		return 0;
	}

	const std::string username = WideToUtf8Local(usernameW);
	const Win64LceLive::SocialActionResult result = Win64LceLive::SendFriendRequestSync(username);
	if (result.success)
		scene->m_statusMessage = L"Friend request sent.";
	else
		scene->m_statusMessage = Utf8ToWideLocal(result.error);

	scene->FetchAndDisplay();
	return 0;
}
#endif
