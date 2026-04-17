#include "stdafx.h"
#include "UI.h"
#include "UIScene_LceLiveInvites.h"
#include "../../Minecraft.h"
#include "../../Windows64/Network/WinsockNetLayer.h"

// Fallbacks until string ID headers are regenerated.
#ifndef IDS_TITLE_SEND_INVITE
#define IDS_TITLE_SEND_INVITE IDS_PLAYERS_INVITE
#endif
#ifndef IDS_TEXT_SEND_INVITE_CONFIRMATION
#define IDS_TEXT_SEND_INVITE_CONFIRMATION IDS_CONFIRM_EXIT_GAME
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

UIScene_LceLiveInvites::UIScene_LceLiveInvites(int iPad, void *initData, UILayer *parentLayer) : UIScene(iPad, parentLayer)
{
	if (initData)
		delete initData;

	initialiseMovie();

	parentLayer->addComponent(iPad, eUIComponent_Panorama);
	parentLayer->addComponent(iPad, eUIComponent_Logo);

	m_friendsList.init(eControl_FriendsList);
	m_actionsList.init(eControl_ActionsList);
	m_labelFriendsTitle.init(L"INVITE FRIENDS");
	m_labelActionsTitle.init(L"ACTIONS");
	m_labelStatus.init(L"");
	m_controlFriendsTimer.setVisible(false);
	m_controlActionsTimer.setVisible(false);

	m_actionsList.addItem(L"REFRESH");
	m_actionsList.setCurrentSelection(eAction_Refresh);

	m_bDataReady = false;
	m_statusMessage.clear();

#ifdef _WINDOWS64
	m_friends.clear();
	m_invitedAccountIds.clear();
	m_pendingInviteAccountId.clear();
	m_pendingInviteLabel.clear();
#endif

	doHorizontalResizeCheck();
	FetchAndDisplay();
}

UIScene_LceLiveInvites::~UIScene_LceLiveInvites()
{
	m_parentLayer->showComponent(m_iPad, eUIComponent_Panorama, false);
	m_parentLayer->showComponent(m_iPad, eUIComponent_Logo, false);
	m_parentLayer->removeComponent(eUIComponent_Panorama);
	m_parentLayer->removeComponent(eUIComponent_Logo);
}

wstring UIScene_LceLiveInvites::getMoviePath()
{
	return L"LoadOrJoinMenu";
}

void UIScene_LceLiveInvites::updateTooltips()
{
	ui.SetTooltips(m_iPad, IDS_TOOLTIPS_SELECT, IDS_TOOLTIPS_BACK, IDS_TOOLTIPS_INVITE_FRIENDS, IDS_TOOLTIPS_REFRESH);
}

void UIScene_LceLiveInvites::updateComponents()
{
	const bool notInGame = (Minecraft::GetInstance()->level == nullptr);
	m_parentLayer->showComponent(m_iPad, eUIComponent_Panorama, notInGame);
	m_parentLayer->showComponent(m_iPad, eUIComponent_Logo, true);
}

void UIScene_LceLiveInvites::handleReload()
{
	doHorizontalResizeCheck();
	m_controlFriendsTimer.setVisible(false);
	m_controlActionsTimer.setVisible(false);
	FetchAndDisplay();
}

void UIScene_LceLiveInvites::handleFocusChange(F64 controlId, F64 childId)
{
	if (static_cast<int>(controlId) == eControl_FriendsList)
		m_friendsList.updateChildFocus(static_cast<int>(childId));
	else if (static_cast<int>(controlId) == eControl_ActionsList)
		m_actionsList.updateChildFocus(static_cast<int>(childId));

	updateTooltips();
}

void UIScene_LceLiveInvites::handleInput(int iPad, int key, bool repeat, bool pressed, bool released, bool &handled)
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
				PromptInviteSelectedFriend();
		}
		break;
	case ACTION_MENU_X:
		if (pressed && !repeat)
		{
#ifdef _WINDOWS64
			PromptInviteSelectedFriend();
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

void UIScene_LceLiveInvites::handlePress(F64 controlId, F64 childId)
{
	if (static_cast<int>(controlId) == eControl_FriendsList)
	{
		m_friendsList.updateChildFocus(static_cast<int>(childId));
		PromptInviteSelectedFriend();
	}
	else if (static_cast<int>(controlId) == eControl_ActionsList)
	{
		m_actionsList.updateChildFocus(static_cast<int>(childId));
		PerformSelectedAction();
	}
}

void UIScene_LceLiveInvites::FetchAndDisplay()
{
#ifdef _WINDOWS64
	const std::string accessToken = Win64LceLive::GetAccessToken();
	if (accessToken.empty())
	{
		m_friends.clear();
		m_invitedAccountIds.clear();
		m_bDataReady = true;
		m_statusMessage = L"Sign in to LCELIVE to invite friends.";
		RebuildLists();
		return;
	}

	const int previousSelection = m_friendsList.getCurrentSelection();
	const Win64LceLive::FriendsListResult result = Win64LceLive::GetFriendsSync();
	if (!result.success)
	{
		m_friends.clear();
		m_bDataReady = true;
		m_statusMessage = Utf8ToWideLocal(result.error);
		RebuildLists();
		return;
	}

	m_friends = result.friends;
	m_invitedAccountIds.clear();
	const Win64LceLive::GameInvitesResult inviteState = Win64LceLive::GetGameInvitesSync();
	if (inviteState.success)
	{
		for (const Win64LceLive::GameInviteEntry &invite : inviteState.outgoing)
		{
			if (invite.status == "pending")
				m_invitedAccountIds.push_back(invite.recipientAccountId);
		}
	}
	m_bDataReady = true;

	if (m_friends.empty())
		m_statusMessage = L"No friends available to invite.";

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
	m_bDataReady = true;
	m_statusMessage = L"Invites are only available on Windows64 builds.";
	RebuildLists();
#endif
}

void UIScene_LceLiveInvites::RebuildLists()
{
	m_friendsList.clearList();

#ifdef _WINDOWS64
	for (const Win64LceLive::SocialEntry &entry : m_friends)
	{
		std::wstring label = BuildFriendLabel(entry);
		if (AlreadyInvited(entry.accountId))
			label += L" [SENT]";
		m_friendsList.addItem(label);
	}
#endif

	UpdateStatusLabel();
}

void UIScene_LceLiveInvites::UpdateStatusLabel()
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

	m_labelFriendsTitle.setLabel(L"INVITE FRIENDS", true, true);
	m_labelActionsTitle.setLabel(L"ACTIONS", true, true);
}

int UIScene_LceLiveInvites::FocusedFriendIndex()
{
	return m_friendsList.getCurrentSelection();
}

int UIScene_LceLiveInvites::SelectedActionIndex()
{
	return m_actionsList.getCurrentSelection();
}

void UIScene_LceLiveInvites::PerformSelectedAction()
{
	switch (SelectedActionIndex())
	{
	case eAction_Refresh:
		FetchAndDisplay();
		break;
	default:
		break;
	}
}

#ifdef _WINDOWS64
bool UIScene_LceLiveInvites::AlreadyInvited(const std::string &accountId) const
{
	for (const std::string &id : m_invitedAccountIds)
	{
		if (id == accountId)
			return true;
	}

	return false;
}

void UIScene_LceLiveInvites::PromptInviteSelectedFriend()
{
	PromptInviteFriendAtIndex(FocusedFriendIndex());
}

void UIScene_LceLiveInvites::PromptInviteFriendAtIndex(int friendIndex)
{
	if (friendIndex < 0 || friendIndex >= static_cast<int>(m_friends.size()))
	{
		m_statusMessage = L"Select a friend first.";
		UpdateStatusLabel();
		return;
	}

	m_pendingInviteAccountId = m_friends[friendIndex].accountId;
	m_pendingInviteLabel = BuildFriendLabel(m_friends[friendIndex]);

	UINT optionIds[2];
	optionIds[0] = IDS_NO;
	optionIds[1] = IDS_YES;

	ui.RequestAlertMessage(
		IDS_TITLE_SEND_INVITE,
		IDS_TEXT_SEND_INVITE_CONFIRMATION,
		optionIds,
		2,
		m_iPad,
		&UIScene_LceLiveInvites::InviteFriendConfirmCallback,
		this);
}

void UIScene_LceLiveInvites::InvitePendingFriend()
{
	if (m_pendingInviteAccountId.empty())
		return;

	if (!g_NetworkManager.IsHost() || !WinsockNetLayer::IsHosting() || g_Win64MultiplayerIP[0] == 0 || g_Win64MultiplayerPort <= 0)
	{
		m_statusMessage = L"The game session is no longer active.";
		m_pendingInviteAccountId.clear();
		m_pendingInviteLabel.clear();
		UpdateStatusLabel();
		return;
	}

	const Win64LceLive::SocialActionResult result = Win64LceLive::SendGameInviteSync(
		m_pendingInviteAccountId,
		g_Win64MultiplayerIP,
		g_Win64MultiplayerPort,
		"");
	if (!result.success)
	{
		m_statusMessage = Utf8ToWideLocal(result.error);
		m_pendingInviteAccountId.clear();
		m_pendingInviteLabel.clear();
		UpdateStatusLabel();
		return;
	}

	m_invitedAccountIds.push_back(m_pendingInviteAccountId);

	m_statusMessage = L"Invite sent to ";
	m_statusMessage += m_pendingInviteLabel;
	m_statusMessage += L".";

	m_pendingInviteAccountId.clear();
	m_pendingInviteLabel.clear();
	UpdateStatusLabel();
}

int UIScene_LceLiveInvites::InviteFriendConfirmCallback(void *pParam, int iPad, C4JStorage::EMessageResult result)
{
	UIScene_LceLiveInvites *scene = static_cast<UIScene_LceLiveInvites *>(pParam);
	if (scene == nullptr)
		return 0;

	(void)iPad;

	// UI returns "Decline" for the 2nd option. With [NO, YES], that means YES => send invite.
	if (result == C4JStorage::EMessage_ResultDecline)
		scene->InvitePendingFriend();
	else
	{
		scene->m_pendingInviteAccountId.clear();
		scene->m_pendingInviteLabel.clear();
	}

	return 0;
}
#endif
