#pragma once

#include "UIScene.h"

#include <string>
#include <vector>

#if defined(_WINDOWS64) && !defined(MINECRAFT_SERVER_BUILD)
#include "../../Windows64/Windows64_LceLive.h"
#endif

// LceLive sub-scene: incoming friend-request inbox.
// Uses the native Start Game style (LoadOrJoinMenu): left list of requests,
// right list of actions.

class UIScene_LceLiveRequests : public UIScene
{
private:
	enum EControls
	{
		eControl_RequestsList,
		eControl_ActionsList,
	};

	enum EActions
	{
		eAction_Refresh = 0,
	};

	struct RequestEntry
	{
		std::string accountId;    // sender's account ID
		std::string username;
		std::string displayName;
	};

	UIControl_ButtonList m_requestsList;
	UIControl_ButtonList m_actionsList;
	UIControl_Label m_labelRequestsTitle;
	UIControl_Label m_labelActionsTitle;
	UIControl_Label m_labelStatus;
	UIControl m_controlRequestsTimer;
	UIControl m_controlActionsTimer;
	std::vector<RequestEntry> m_entries;
	std::wstring m_statusMessage;
	bool m_bDataReady;
#ifdef _WINDOWS64
	std::string m_pendingFromAccountId;
#endif
	UI_BEGIN_MAP_ELEMENTS_AND_NAMES(UIScene)
		UI_MAP_ELEMENT(m_requestsList, "SavesList")
		UI_MAP_ELEMENT(m_actionsList, "JoinList")
		UI_MAP_ELEMENT(m_labelRequestsTitle, "SavesListTitle")
		UI_MAP_ELEMENT(m_labelActionsTitle, "JoinListTitle")
		UI_MAP_ELEMENT(m_labelStatus, "NoGames")
		UI_MAP_ELEMENT(m_controlRequestsTimer, "SavesTimer")
		UI_MAP_ELEMENT(m_controlActionsTimer, "JoinTimer")
	UI_END_MAP_ELEMENTS_AND_NAMES()

public:
	UIScene_LceLiveRequests(int iPad, void *initData, UILayer *parentLayer);
	~UIScene_LceLiveRequests();

	virtual EUIScene getSceneType() { return eUIScene_LceLiveRequests; }
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
	int FocusedRequestIndex();
	int SelectedActionIndex();
	void PerformSelectedAction();
	void PromptResolveSelectedRequest();
	void PerformAccept();
	void PerformDecline();
#ifdef _WINDOWS64
	void ResolvePendingRequest(bool accept);
	static int ResolveRequestConfirmCallback(void *pParam, int iPad, C4JStorage::EMessageResult result);
#endif
};
