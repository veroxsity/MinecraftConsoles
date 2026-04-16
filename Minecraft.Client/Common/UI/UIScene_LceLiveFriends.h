#pragma once

#include "UIScene.h"

#include <vector>

#if defined(_WINDOWS64) && !defined(MINECRAFT_SERVER_BUILD)
#include "../../Windows64/Windows64_LceLive.h"
#endif

// LceLive sub-scene: friends list.
// Uses the same panel/list layout style as Start Game (LoadOrJoinMenu) so it
// looks native: left list = friends, right list = actions, right text = status.

class UIScene_LceLiveFriends : public UIScene
{
private:
	enum EControls
	{
		eControl_FriendsList,
		eControl_ActionsList,
	};

	enum EActions
	{
		eAction_AddFriend = 0,
		eAction_Refresh,
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
	std::vector<Win64LceLive::SocialEntry> m_friends;
#endif
	std::wstring m_statusMessage;
    bool m_bDataReady;
#ifdef _WINDOWS64
	std::string  m_pendingRemovalAccountId;
	std::wstring m_pendingRemovalLabel;
#endif

public:
	UIScene_LceLiveFriends(int iPad, void *initData, UILayer *parentLayer);
	~UIScene_LceLiveFriends();

	virtual EUIScene getSceneType() { return eUIScene_LceLiveFriends; }
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
	void FetchAndDisplay();
	void RebuildLists();
	void UpdateStatusLabel();
	int FocusedFriendIndex();
	int SelectedActionIndex();
	void PerformSelectedAction();
#ifdef _WINDOWS64
	void PromptRemoveSelectedFriend();
	void PromptRemoveFriendAtIndex(int friendIndex);
	void RemovePendingFriend();
	static int RemoveFriendConfirmCallback(void *pParam, int iPad, C4JStorage::EMessageResult result);
#endif
#ifdef _WINDOWS64
	void OpenAddFriendKeyboard();
	static int AddFriendKeyboardCallback(LPVOID lpParam, const bool bResult);
#endif
};
