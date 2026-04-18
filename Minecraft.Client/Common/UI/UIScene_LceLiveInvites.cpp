#include "stdafx.h"
#include "UI.h"
#include "UIScene_LceLiveInvites.h"
#include "../../Minecraft.h"
#include "../../Windows64/Network/WinsockNetLayer.h"
#ifdef _WINDOWS64
#include "../../Windows64/Windows64_LceLiveP2P.h"
#include "../../Windows64/Windows64_LceLiveSignaling.h"
#include "../../Windows64/Windows64_LceLiveRelay.h"
#include "../../Windows64/Windows64_Log.h"
#endif

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
	m_gameInvites.clear();
	m_pendingAcceptInviteId.clear();
	m_pendingAcceptHostIp.clear();
	m_pendingAcceptHostPort = 0;
	m_pendingAcceptHostName.clear();
	m_pendingAcceptSignalingSessionId.clear();
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
			{
#ifdef _WINDOWS64
				if (IsReceiveMode())
					PromptAcceptSelectedInvite();
				else
					PromptInviteSelectedFriend();
#endif
			}
		}
		break;
	case ACTION_MENU_X:
		if (pressed && !repeat)
		{
#ifdef _WINDOWS64
			if (IsReceiveMode())
				PromptAcceptSelectedInvite();
			else
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
#ifdef _WINDOWS64
		if (IsReceiveMode())
			PromptAcceptSelectedInvite();
		else
			PromptInviteSelectedFriend();
#endif
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
		m_gameInvites.clear();
		m_bDataReady = true;
		m_statusMessage = IsReceiveMode()
			? L"Sign in to LCELive to view game invites."
			: L"Sign in to LCELive to invite friends.";
		RebuildLists();
		return;
	}

	// ── Receive mode: show incoming game invites ──────────────────────────
	if (IsReceiveMode())
	{
		const int previousSelection = m_friendsList.getCurrentSelection();
		const Win64LceLive::GameInvitesResult result = Win64LceLive::GetGameInvitesSync();
		if (!result.success)
		{
			m_gameInvites.clear();
			m_bDataReady = true;
			m_statusMessage = Utf8ToWideLocal(result.error);
			RebuildLists();
			return;
		}

		m_gameInvites = result.incoming;
		m_bDataReady  = true;

		if (m_gameInvites.empty())
			m_statusMessage = L"No pending game invites.";
		else if (m_statusMessage == L"No pending game invites.")
			m_statusMessage.clear();

		RebuildLists();

		if (!m_gameInvites.empty())
		{
			int sel = previousSelection;
			if (sel < 0) sel = 0;
			if (sel >= static_cast<int>(m_gameInvites.size()))
				sel = static_cast<int>(m_gameInvites.size()) - 1;
			m_friendsList.setCurrentSelection(sel);
		}
		return;
	}

	// ── Send mode: show friends list for inviting ─────────────────────────
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
	if (IsReceiveMode())
	{
		for (const Win64LceLive::GameInviteEntry &entry : m_gameInvites)
		{
			std::wstring label;
			if (!entry.senderDisplayName.empty())
				label = Utf8ToWideLocal(entry.senderDisplayName);
			else if (!entry.senderUsername.empty())
				label = Utf8ToWideLocal(entry.senderUsername);
			else
				label = L"<unknown>";

			if (!entry.senderUsername.empty())
			{
				label += L" (@";
				label += Utf8ToWideLocal(entry.senderUsername);
				label += L")";
			}
			if (!entry.hostName.empty())
			{
				label += L"  -  ";
				label += Utf8ToWideLocal(entry.hostName);
			}
			if (!entry.sessionActive)
				label += L"  [inactive]";
			m_friendsList.addItem(label);
		}
	}
	else
	{
		for (const Win64LceLive::SocialEntry &entry : m_friends)
		{
			std::wstring label = BuildFriendLabel(entry);
			if (AlreadyInvited(entry.accountId))
				label += L" [SENT]";
			m_friendsList.addItem(label);
		}
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

	m_labelFriendsTitle.setLabel(IsReceiveMode() ? L"GAME INVITES" : L"INVITE FRIENDS", true, true);
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

	if (!g_NetworkManager.IsHost() || !WinsockNetLayer::IsHosting() || WinsockNetLayer::GetHostPort() <= 0)
	{
		m_statusMessage = L"The game session is no longer active.";
		m_pendingInviteAccountId.clear();
		m_pendingInviteLabel.clear();
		UpdateStatusLabel();
		return;
	}

	// Pick the right host IP for joiners:
	//   tcpPortMapped=true  → UPnP mapped the TCP game port on the router,
	//                         so internet joiners can reach externalIp:port directly.
	//   tcpPortMapped=false → no TCP port mapping; use the LAN IP so same-network
	//                         testing works. Internet play without UPnP needs KCP.
	const Win64LceLiveP2P::P2PSnapshot snap = Win64LceLiveP2P::GetP2PSnapshot();
	const std::string hostIp   = (snap.tcpPortMapped && !snap.externalIp.empty())
	                             ? snap.externalIp
	                             : WinsockNetLayer::GetLocalIPv4();
	const int         hostPort = WinsockNetLayer::GetHostPort();

	// Include the P2P signaling session ID so the joiner can do hole punching.
	// Guard against stale IDs: invites sent while signaling is not active can
	// fail immediately for joiners (for example WS close 4317).
	const Win64LceLiveSignaling::SignalingSnapshot sigSnap =
		Win64LceLiveSignaling::GetSnapshot();
	const bool signalingReady =
		(sigSnap.state == Win64LceLiveSignaling::ESignalingState::Connecting ||
		 sigSnap.state == Win64LceLiveSignaling::ESignalingState::Connected);
	if (!signalingReady || sigSnap.sessionId.empty())
	{
		m_statusMessage = L"Invite channel is refreshing. Try again in a second.";
		m_pendingInviteAccountId.clear();
		m_pendingInviteLabel.clear();
		UpdateStatusLabel();
		return;
	}
	const std::string signalingSessionId = sigSnap.sessionId;

	const Win64LceLive::SocialActionResult result = Win64LceLive::SendGameInviteSync(
		m_pendingInviteAccountId,
		hostIp,
		hostPort,
		"",
		signalingSessionId);
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

// ── Receive mode ──────────────────────────────────────────────────────────

bool UIScene_LceLiveInvites::IsReceiveMode() const
{
	return Minecraft::GetInstance()->level == nullptr;
}

void UIScene_LceLiveInvites::PromptAcceptSelectedInvite()
{
	const int idx = FocusedFriendIndex();
	if (idx < 0 || idx >= static_cast<int>(m_gameInvites.size()))
	{
		m_statusMessage = L"Select an invite first.";
		UpdateStatusLabel();
		return;
	}

	m_pendingAcceptInviteId = m_gameInvites[idx].inviteId;

	UINT optionIds[2];
	optionIds[0] = IDS_NO;
	optionIds[1] = IDS_YES;

	ui.RequestAlertMessage(
		IDS_TITLE_SEND_INVITE,
		IDS_TEXT_SEND_INVITE_CONFIRMATION,
		optionIds,
		2,
		m_iPad,
		&UIScene_LceLiveInvites::AcceptInviteConfirmCallback,
		this);
}

void UIScene_LceLiveInvites::ResolvePendingInvite(bool accept)
{
	if (m_pendingAcceptInviteId.empty())
		return;

	const std::string inviteId = m_pendingAcceptInviteId;

	if (accept)
	{
		const Win64LceLive::GameInviteActionResult result = Win64LceLive::AcceptGameInviteSync(inviteId);
		if (result.success)
		{
			m_pendingAcceptHostIp             = result.hostIp;
			m_pendingAcceptHostPort           = result.hostPort;
			m_pendingAcceptHostName           = result.hostName;
			m_pendingAcceptSignalingSessionId = result.signalingSessionId;

			if (!result.signalingSessionId.empty())
			{
				// Always open the joiner relay — the host always has one ready.
				// Relay goes over WSS (port 443) so it works on any network including
				// campus WiFi, hotel WiFi, or any firewall that blocks port 25565.
				const int relayProxyPort =
					Win64LceLiveRelay::JoinerOpen(result.signalingSessionId);

				// Decide which address to give the game:
				//   Publicly routable host IP → try direct TCP first (UPnP may have
				//     mapped the port).  If port 25565 is blocked outbound the direct
				//     attempt fails quickly and the relay fallback fires automatically.
				//   Non-routable host IP (RFC 1918 private, CGNAT 100.64.0.0/10,
				//     loopback, link-local, etc.) → direct TCP can never work from the
				//     internet; go straight to relay.  This is the fix for CGNAT hosts:
				//     the router happily returns a 100.64.x.x UPnP mapping that looks
				//     like a success but is not reachable from outside the carrier.
				const std::string& hIp = result.hostIp;
				const bool hostIsPublicIp = Win64LceLiveP2P::IsPublicRoutableIPv4(hIp);

				if (!hostIsPublicIp)
					LCELOG("JOIN", "host IP %s is non-routable (CGNAT/private) — "
					              "skipping direct join, using relay only", hIp.c_str());

				if (!hostIsPublicIp && relayProxyPort > 0)
				{
					// No public IP — relay is the only path.
					m_pendingAcceptHostIp   = "127.0.0.1";
					m_pendingAcceptHostPort = relayProxyPort;
				}
				else if (relayProxyPort > 0)
				{
					// Public IP → try direct TCP first.  Stash the relay proxy port so
					// the main Tick auto-retries through the relay if the direct attempt
					// fails (e.g. campus / hotel WiFi blocks port 25565 outbound).
					// This is Xbox Live's TURN fallback pattern: relay is pre-allocated
					// on both sides before any direct connection is even attempted.
					g_LceLiveRelayFallbackPort = relayProxyPort;
				}

				// Set up the P2P/signaling path for hole-punching (best-effort).
				Win64LceLiveP2P::HostOpen();
				Win64LceLiveSignaling::PrepareJoin(result.signalingSessionId);
			}

			JoinAcceptedInvite();
			m_pendingAcceptInviteId.clear();
			return;
		}

		m_statusMessage = Utf8ToWideLocal(result.error);
		UpdateStatusLabel();
		FetchAndDisplay();
		m_pendingAcceptInviteId.clear();
		return;
	}

	const Win64LceLive::SocialActionResult declineResult = Win64LceLive::DeclineGameInviteSync(inviteId);
	m_statusMessage = declineResult.success ? L"Game invite declined." : Utf8ToWideLocal(declineResult.error);
	m_pendingAcceptInviteId.clear();

	FetchAndDisplay();
}

void UIScene_LceLiveInvites::JoinAcceptedInvite()
{
	if (m_pendingAcceptHostIp.empty() || m_pendingAcceptHostPort <= 0)
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
	strcpy_s(inviteInfo.hostIP, m_pendingAcceptHostIp.c_str());
	inviteInfo.hostPort   = m_pendingAcceptHostPort;
	inviteInfo.sessionActive = true;
	const std::wstring hostNameWide = m_pendingAcceptHostName.empty()
		? L"LCELive" : Utf8ToWideLocal(m_pendingAcceptHostName);
	wcsncpy_s(inviteInfo.hostName, hostNameWide.c_str(), _TRUNCATE);
	strcpy_s(inviteInfo.inviteId, m_pendingAcceptInviteId.c_str());

	const bool success = g_NetworkManager.JoinGameFromInviteInfo(m_iPad, localUsersMask, &inviteInfo);
	if (!success)
		m_statusMessage = L"Could not join this game.";
	else
		m_statusMessage = L"Joining game...";

	m_pendingAcceptInviteId.clear();
	m_pendingAcceptHostIp.clear();
	m_pendingAcceptHostPort = 0;
	m_pendingAcceptHostName.clear();
	m_pendingAcceptSignalingSessionId.clear();
	UpdateStatusLabel();
	FetchAndDisplay();
}

int UIScene_LceLiveInvites::AcceptInviteConfirmCallback(void *pParam, int iPad, C4JStorage::EMessageResult result)
{
	UIScene_LceLiveInvites *scene = static_cast<UIScene_LceLiveInvites *>(pParam);
	if (scene == nullptr)
		return 0;

	(void)iPad;

	// [NO, YES] — "Decline" is the 2nd option (YES).
	if (result == C4JStorage::EMessage_ResultDecline)
		scene->ResolvePendingInvite(true);
	else
	{
		scene->m_pendingAcceptInviteId.clear();
		scene->FetchAndDisplay();
	}

	return 0;
}
#endif
