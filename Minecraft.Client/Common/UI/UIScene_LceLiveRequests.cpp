#include "stdafx.h"
#include "UI.h"
#include "UIScene_LceLiveRequests.h"
#include "../../Minecraft.h"

// Fallbacks until string ID headers are regenerated.
#ifndef IDS_TITLE_FRIEND_REQUEST
#define IDS_TITLE_FRIEND_REQUEST IDS_TOOLTIPS_SELECT
#endif
#ifndef IDS_TEXT_ACCEPT_REQUEST_CONFIRMATION
#define IDS_TEXT_ACCEPT_REQUEST_CONFIRMATION IDS_CONFIRM_EXIT_GAME
#endif
#ifndef IDS_TITLE_SEND_INVITE
#define IDS_TITLE_SEND_INVITE IDS_TITLE_FRIEND_REQUEST
#endif
#ifndef IDS_TEXT_SEND_INVITE_CONFIRMATION
#define IDS_TEXT_SEND_INVITE_CONFIRMATION IDS_TEXT_ACCEPT_REQUEST_CONFIRMATION
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

	std::wstring BuildInviteLabel(
		const std::string &senderDisplayName,
		const std::string &senderUsername,
		const std::string &hostName,
		bool sessionActive)
	{
		std::wstring line = L"[INVITE] ";

		if (!senderDisplayName.empty())
			line += Utf8ToWideLocal(senderDisplayName);
		else if (!senderUsername.empty())
			line += Utf8ToWideLocal(senderUsername);
		else
			line += L"<unknown>";

		if (!senderUsername.empty())
		{
			line += L" (@";
			line += Utf8ToWideLocal(senderUsername);
			line += L")";
		}

		if (!hostName.empty())
		{
			line += L"  -  ";
			line += Utf8ToWideLocal(hostName);
		}

		if (!sessionActive)
			line += L"  [inactive]";

		return line;
	}
}
#endif

UIScene_LceLiveRequests::UIScene_LceLiveRequests(int iPad, void *initData, UILayer *parentLayer) : UIScene(iPad, parentLayer)
{
	initialiseMovie();

	parentLayer->addComponent(iPad, eUIComponent_Panorama);
	parentLayer->addComponent(iPad, eUIComponent_Logo);

	m_requestsList.init(eControl_RequestsList);
	m_actionsList.init(eControl_ActionsList);
	m_labelRequestsTitle.init(L"GAME INVITES");
	m_labelActionsTitle.init(L"ACTIONS");
	m_labelStatus.init(L"");
	m_controlRequestsTimer.setVisible(false);
	m_controlActionsTimer.setVisible(false);

	m_actionsList.addItem(L"REFRESH");
	m_actionsList.setCurrentSelection(eAction_Refresh);

	m_entries.clear();
	m_statusMessage.clear();
	m_bDataReady = false;
#ifdef _WINDOWS64
	m_pendingInviteId.clear();
	m_pendingInviteHostIp.clear();
	m_pendingInviteHostPort = 0;
	m_pendingInviteHostName.clear();
#endif

	doHorizontalResizeCheck();
	FetchAndDisplay();
}

UIScene_LceLiveRequests::~UIScene_LceLiveRequests()
{
	m_parentLayer->removeComponent(eUIComponent_Panorama);
	m_parentLayer->removeComponent(eUIComponent_Logo);
}

wstring UIScene_LceLiveRequests::getMoviePath()
{
	return L"LoadOrJoinMenu";
}

void UIScene_LceLiveRequests::updateTooltips()
{
	ui.SetTooltips(m_iPad, IDS_TOOLTIPS_SELECT, IDS_TOOLTIPS_BACK, IDS_TOOLTIPS_DELETE, IDS_TOOLTIPS_REFRESH);
}

void UIScene_LceLiveRequests::updateComponents()
{
	const bool notInGame = (Minecraft::GetInstance()->level == nullptr);
	m_parentLayer->showComponent(m_iPad, eUIComponent_Panorama, notInGame);
	m_parentLayer->showComponent(m_iPad, eUIComponent_Logo, true);
}

void UIScene_LceLiveRequests::handleReload()
{
	doHorizontalResizeCheck();
	m_controlRequestsTimer.setVisible(false);
	m_controlActionsTimer.setVisible(false);
	FetchAndDisplay();
}

void UIScene_LceLiveRequests::handleFocusChange(F64 controlId, F64 childId)
{
	if (static_cast<int>(controlId) == eControl_RequestsList)
		m_requestsList.updateChildFocus(static_cast<int>(childId));
	else if (static_cast<int>(controlId) == eControl_ActionsList)
		m_actionsList.updateChildFocus(static_cast<int>(childId));

	updateTooltips();
}

void UIScene_LceLiveRequests::handleInput(int iPad, int key, bool repeat, bool pressed, bool released, bool &handled)
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
			else if (controlHasFocus(eControl_RequestsList))
				PromptResolveSelectedRequest();
		}
		break;
	case ACTION_MENU_X:
		if (pressed && !repeat)
		{
			PerformDecline();
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

void UIScene_LceLiveRequests::handlePress(F64 controlId, F64 childId)
{
	if (static_cast<int>(controlId) == eControl_RequestsList)
	{
		m_requestsList.updateChildFocus(static_cast<int>(childId));
		PromptResolveSelectedRequest();
	}
	else if (static_cast<int>(controlId) == eControl_ActionsList)
	{
		m_actionsList.updateChildFocus(static_cast<int>(childId));
		PerformSelectedAction();
	}
}

void UIScene_LceLiveRequests::FetchAndDisplay()
{
#ifdef _WINDOWS64
	const std::string accessToken = Win64LceLive::GetAccessToken();
	if (accessToken.empty())
	{
		m_entries.clear();
		m_bDataReady = true;
		m_statusMessage = L"Sign in to view and manage game invites.";
		RebuildLists();
		return;
	}

	const int previousSelection = m_requestsList.getCurrentSelection();
	const Win64LceLive::GameInvitesResult result = Win64LceLive::GetGameInvitesSync();
	if (!result.success)
	{
		m_entries.clear();
		m_bDataReady = true;
		m_statusMessage = Utf8ToWideLocal(result.error);
		RebuildLists();
		return;
	}

	m_entries.clear();
	for (const Win64LceLive::GameInviteEntry &entry : result.incoming)
	{
		RequestEntry row = {};
		row.inviteId = entry.inviteId;
		row.senderAccountId = entry.senderAccountId;
		row.senderUsername = entry.senderUsername;
		row.senderDisplayName = entry.senderDisplayName;
		row.recipientAccountId = entry.recipientAccountId;
		row.recipientUsername = entry.recipientUsername;
		row.recipientDisplayName = entry.recipientDisplayName;
		row.hostIp = entry.hostIp;
		row.hostPort = entry.hostPort;
		row.hostName = entry.hostName;
		row.status = entry.status;
		row.sessionActive = entry.sessionActive;
		m_entries.push_back(row);
	}

	m_bDataReady = true;
	if (m_entries.empty() && m_statusMessage.empty())
		m_statusMessage = L"No pending game invites.";
	else if (!m_entries.empty() && m_statusMessage == L"No pending game invites.")
		m_statusMessage.clear();

	RebuildLists();

	if (!m_entries.empty())
	{
		int newSelection = previousSelection;
		if (newSelection < 0)
			newSelection = 0;
		if (newSelection >= static_cast<int>(m_entries.size()))
			newSelection = static_cast<int>(m_entries.size()) - 1;
		m_requestsList.setCurrentSelection(newSelection);
	}
#else
	m_entries.clear();
	m_bDataReady = true;
	m_statusMessage = L"Game invites are only available on Windows64 builds.";
	RebuildLists();
#endif
}

void UIScene_LceLiveRequests::RebuildLists()
{
	m_requestsList.clearList();

#ifdef _WINDOWS64
	for (const RequestEntry &entry : m_entries)
		m_requestsList.addItem(BuildInviteLabel(entry.senderDisplayName, entry.senderUsername, entry.hostName, entry.sessionActive));
#endif

	UpdateStatusLabel();
}

void UIScene_LceLiveRequests::UpdateStatusLabel()
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

	m_labelRequestsTitle.setLabel(L"GAME INVITES", true, true);
	m_labelActionsTitle.setLabel(L"ACTIONS", true, true);
}

int UIScene_LceLiveRequests::FocusedRequestIndex()
{
	return m_requestsList.getCurrentSelection();
}

int UIScene_LceLiveRequests::SelectedActionIndex()
{
	return m_actionsList.getCurrentSelection();
}

void UIScene_LceLiveRequests::PerformSelectedAction()
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

void UIScene_LceLiveRequests::PromptResolveSelectedRequest()
{
	const int selectedIndex = FocusedRequestIndex();
	if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_entries.size()))
	{
		m_statusMessage = L"Select an invite first.";
		UpdateStatusLabel();
		return;
	}

#ifdef _WINDOWS64
	m_pendingInviteId = m_entries[selectedIndex].inviteId;
	m_pendingInviteHostIp = m_entries[selectedIndex].hostIp;
	m_pendingInviteHostPort = m_entries[selectedIndex].hostPort;
	m_pendingInviteHostName = m_entries[selectedIndex].hostName;

	UINT optionIds[2];
	optionIds[0] = IDS_NO;
	optionIds[1] = IDS_YES;

	ui.RequestAlertMessage(
		IDS_TITLE_SEND_INVITE,
		IDS_TEXT_SEND_INVITE_CONFIRMATION,
		optionIds,
		2,
		m_iPad,
		&UIScene_LceLiveRequests::ResolveRequestConfirmCallback,
		this);
#endif
}

void UIScene_LceLiveRequests::PerformAccept()
{
#ifdef _WINDOWS64
	const int selectedIndex = FocusedRequestIndex();
	if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_entries.size()))
	{
		m_statusMessage = L"Select an invite first.";
		UpdateStatusLabel();
		return;
	}
	ResolvePendingRequest(true);
#endif
}

void UIScene_LceLiveRequests::PerformDecline()
{
#ifdef _WINDOWS64
	const int selectedIndex = FocusedRequestIndex();
	if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_entries.size()))
	{
		m_statusMessage = L"Select an invite first.";
		UpdateStatusLabel();
		return;
	}
	ResolvePendingRequest(false);
#endif
}

#ifdef _WINDOWS64
void UIScene_LceLiveRequests::ResolvePendingRequest(bool accept)
{
	if (m_pendingInviteId.empty())
		return;

	const std::string inviteId = m_pendingInviteId;

	if (accept)
	{
		const Win64LceLive::GameInviteActionResult result = Win64LceLive::AcceptGameInviteSync(inviteId);
		if (result.success)
		{
			m_pendingInviteHostIp = result.hostIp;
			m_pendingInviteHostPort = result.hostPort;
			m_pendingInviteHostName = result.hostName;
			JoinAcceptedInvite();
			m_pendingInviteId.clear();
			return;
		}

		m_statusMessage = Utf8ToWideLocal(result.error);
		UpdateStatusLabel();
		FetchAndDisplay();
		m_pendingInviteId.clear();
		return;
	}

	const Win64LceLive::SocialActionResult declineResult = Win64LceLive::DeclineGameInviteSync(inviteId);
	if (declineResult.success)
		m_statusMessage = L"Game invite declined.";
	else
		m_statusMessage = Utf8ToWideLocal(declineResult.error);
	m_pendingInviteId.clear();

	FetchAndDisplay();
}

void UIScene_LceLiveRequests::JoinAcceptedInvite()
{
	if (m_pendingInviteHostIp.empty() || m_pendingInviteHostPort <= 0)
	{
		m_statusMessage = L"The game session is no longer active.";
		UpdateStatusLabel();
		FetchAndDisplay();
		return;
	}

	ProfileManager.SetLockedProfile(m_iPad);
	ProfileManager.SetPrimaryPad(m_iPad);
	g_NetworkManager.SetLocalGame(false);
	ProfileManager.QuerySigninStatus();
	Minecraft::GetInstance()->clearConnectionFailed();

	int localUsersMask = 0;
	for (unsigned int index = 0; index < XUSER_MAX_COUNT; ++index)
	{
		if (ProfileManager.IsSignedIn(index))
			localUsersMask |= g_NetworkManager.GetLocalPlayerMask(index);
	}

	INVITE_INFO inviteInfo = {};
	inviteInfo.netVersion = MINECRAFT_NET_VERSION;
	strcpy_s(inviteInfo.hostIP, m_pendingInviteHostIp.c_str());
	inviteInfo.hostPort = m_pendingInviteHostPort;
	inviteInfo.sessionActive = true;
	const std::wstring hostNameWide = m_pendingInviteHostName.empty() ? L"LCELive" : Utf8ToWideLocal(m_pendingInviteHostName);
	wcsncpy_s(inviteInfo.hostName, hostNameWide.c_str(), _TRUNCATE);
	strcpy_s(inviteInfo.inviteId, m_pendingInviteId.c_str());

	const bool success = g_NetworkManager.JoinGameFromInviteInfo(m_iPad, localUsersMask, &inviteInfo);
	if (!success)
		m_statusMessage = L"Could not join this game.";
	else
		m_statusMessage = L"Joining game...";

	m_pendingInviteId.clear();
	m_pendingInviteHostIp.clear();
	m_pendingInviteHostPort = 0;
	m_pendingInviteHostName.clear();
	UpdateStatusLabel();
	FetchAndDisplay();
}

int UIScene_LceLiveRequests::ResolveRequestConfirmCallback(void *pParam, int iPad, C4JStorage::EMessageResult result)
{
	UIScene_LceLiveRequests *scene = static_cast<UIScene_LceLiveRequests *>(pParam);
	if (scene == nullptr)
		return 0;

	(void)iPad;

	// UI returns "Decline" for the 2nd option. With [NO, YES], that means YES => accept.
	if (result == C4JStorage::EMessage_ResultDecline)
		scene->ResolvePendingRequest(true);
	else
		scene->ResolvePendingRequest(false);

	return 0;
}
#endif
