#include "stdafx.h"
#include "UI.h"
#include "UIScene_LceLive.h"
#include "../../Minecraft.h"

UIScene_LceLive::UIScene_LceLive(int iPad, void *initData, UILayer *parentLayer) : UIScene(iPad, parentLayer)
{
	initialiseMovie();

	parentLayer->addComponent(iPad, eUIComponent_Panorama);
	parentLayer->addComponent(iPad, eUIComponent_Logo);

	m_buttons[BUTTON_LCELIVE_LINKING].init(L"LINKING",  BUTTON_LCELIVE_LINKING);
	m_buttons[BUTTON_LCELIVE_FRIENDS].init(L"FRIENDS",  BUTTON_LCELIVE_FRIENDS);
	m_buttons[BUTTON_LCELIVE_REQUESTS].init(L"REQUESTS", BUTTON_LCELIVE_REQUESTS);

	// Remove the four unused button slots that exist in the HelpAndOptionsMenu SWF
	// so they don't appear as blank entries in the hub list
	removeControl(&m_buttons[BUTTON_LCELIVE_UNUSED_3], false);
	removeControl(&m_buttons[BUTTON_LCELIVE_UNUSED_4], false);
	removeControl(&m_buttons[BUTTON_LCELIVE_UNUSED_5], false);
	removeControl(&m_buttons[BUTTON_LCELIVE_UNUSED_6], false);

	doHorizontalResizeCheck();
}

UIScene_LceLive::~UIScene_LceLive()
{
	m_parentLayer->removeComponent(eUIComponent_Panorama);
	m_parentLayer->removeComponent(eUIComponent_Logo);
}

wstring UIScene_LceLive::getMoviePath()
{
	if (app.GetLocalPlayerCount() > 1)
		return L"HelpAndOptionsMenuSplit";
	return L"HelpAndOptionsMenu";
}

void UIScene_LceLive::updateTooltips()
{
	ui.SetTooltips(m_iPad, IDS_TOOLTIPS_SELECT, IDS_TOOLTIPS_BACK);
}

void UIScene_LceLive::updateComponents()
{
	bool bNotInGame = (Minecraft::GetInstance()->level == nullptr);
	if (bNotInGame)
	{
		m_parentLayer->showComponent(m_iPad, eUIComponent_Panorama, true);
		m_parentLayer->showComponent(m_iPad, eUIComponent_Logo, true);
	}
	else
	{
		m_parentLayer->showComponent(m_iPad, eUIComponent_Panorama, false);
		m_parentLayer->showComponent(m_iPad, eUIComponent_Logo, true);
	}
}

void UIScene_LceLive::handleReload()
{
	removeControl(&m_buttons[BUTTON_LCELIVE_UNUSED_3], false);
	removeControl(&m_buttons[BUTTON_LCELIVE_UNUSED_4], false);
	removeControl(&m_buttons[BUTTON_LCELIVE_UNUSED_5], false);
	removeControl(&m_buttons[BUTTON_LCELIVE_UNUSED_6], false);
	doHorizontalResizeCheck();
}

void UIScene_LceLive::handleInput(int iPad, int key, bool repeat, bool pressed, bool released, bool &handled)
{
	ui.AnimateKeyPress(m_iPad, key, repeat, pressed, released);

	switch (key)
	{
	case ACTION_MENU_CANCEL:
		if (pressed && !repeat)
			navigateBack();
		break;
	case ACTION_MENU_OK:
#ifdef __ORBIS__
	case ACTION_MENU_TOUCHPAD_PRESS:
#endif
		if (pressed)
			ui.PlayUISFX(eSFX_Press);
	case ACTION_MENU_UP:
	case ACTION_MENU_DOWN:
		sendInputToMovie(key, repeat, pressed, released);
		break;
	}
}

void UIScene_LceLive::handlePress(F64 controlId, F64 childId)
{
	switch (static_cast<int>(controlId))
	{
	case BUTTON_LCELIVE_LINKING:
		ui.NavigateToScene(m_iPad, eUIScene_LceLiveLinking);
		break;
	case BUTTON_LCELIVE_FRIENDS:
		ui.NavigateToScene(m_iPad, eUIScene_LceLiveFriends);
		break;
	case BUTTON_LCELIVE_REQUESTS:
		ui.NavigateToScene(m_iPad, eUIScene_LceLiveRequests);
		break;
	}
}
