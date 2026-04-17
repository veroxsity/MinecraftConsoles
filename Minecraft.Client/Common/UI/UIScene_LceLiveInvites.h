#pragma once

#include "UIScene.h"

#include <string>
#include <vector>

#if defined(_WINDOWS64) && !defined(MINECRAFT_SERVER_BUILD)
#include "../../Windows64/Windows64_LceLive.h"
#endif

// LceLive sub-scene: in-game invite picker.
// This is intentionally separate from Friends management so remove/add actions
// stay in the Friends scene while Invite Friends opens a dedicated picker.

class UIScene_LceLiveInvites : public UIScene
{
private:
	enum EControls
	{
		eControl_FriendsList,
		eControl_ActionsList,
	};

	enum EActions
	{
		eAction_Refresh = 0,
	};

	UIControl_ButtonList m_friendsList;
	UIControl_ButtonList m_actionsList;
	UIControl_Label m_labelFriendsTitle;
	UIControl_Label m_labelActionsTitle;
	UIControl_Label m_labelStatus;
	UIControl m_controlFriendsTimer;
	UIControl m_controlActionsTimer;

	UI_BEGIN_MAP_ELEMENTS_AND_NAMES(UIScene)
		UI_MAP_ELEMENT(m_friendsList, "SavesList")
		UI_MAP_ELEMENT(m_actionsList, "JoinList")
		UI_MAP_ELEMENT(m_labelFriendsTitle, "SavesListTitle")
		UI_MAP_ELEMENT(m_labelActionsTitle, "JoinListTitle")
		UI_MAP_ELEMENT(m_labelStatus, "NoGames")
		UI_MAP_ELEMENT(m_controlFriendsTimer, "SavesTimer")
		UI_MAP_ELEMENT(m_controlActionsTimer, "JoinTimer")
	UI_END_MAP_ELEMENTS_AND_NAMES()

#if defined(_WINDOWS64) && !defined(MINECRAFT_SERVER_BUILD)
	// Send-invite mode (in-game, hosting)
	std::vector<Win64LceLive::SocialEntry> m_friends;
	std::vector<std::string> m_invitedAccountIds;
	std::string m_pendingInviteAccountId;
	std::wstring m_pendingInviteLabel;

	// Receive-invite mode (main menu, not in game)
	std::vector<Win64LceLive::GameInviteEntry> m_gameInvites;
	std::string m_pendingAcceptInviteId;
	std::string m_pendingAcceptHostIp;
	int         m_pendingAcceptHostPort;
	std::string m_pendingAcceptHostName;
	std::string m_pendingAcceptSignalingSessionId;
#endif

	std::wstring m_statusMessage;
	bool m_bDataReady;

public:
	UIScene_LceLiveInvites(int iPad, void *initData, UILayer *parentLayer);
	~UIScene_LceLiveInvites();

	virtual EUIScene getSceneType() { return eUIScene_LceLiveInvites; }
	virtual void updateTooltips();
	virtual void updateComponents();

protected:
	virtual wstring getMoviePath();
	virtual void handleReload();
	virtual void handleFocusChange(F64 controlId, F64 childId);

public:
	virtual void handleInput(int iPad, int key, bool repeat, bool pressed, bool released, bool &handled);

protected:
	void handlePress(F64 controlId, F64 childId);

private:
	bool IsReceiveMode() const; // true when opened from main menu (no active game)
	void FetchAndDisplay();
	void RebuildLists();
	void UpdateStatusLabel();
	int FocusedFriendIndex();
	int SelectedActionIndex();
	void PerformSelectedAction();

#ifdef _WINDOWS64
	// Send mode
	void PromptInviteSelectedFriend();
	void PromptInviteFriendAtIndex(int friendIndex);
	void InvitePendingFriend();
	bool AlreadyInvited(const std::string &accountId) const;
	static int InviteFriendConfirmCallback(void *pParam, int iPad, C4JStorage::EMessageResult result);

	// Receive mode
	void PromptAcceptSelectedInvite();
	void ResolvePendingInvite(bool accept);
	void JoinAcceptedInvite();
	static int AcceptInviteConfirmCallback(void *pParam, int iPad, C4JStorage::EMessageResult result);
#endif
};
