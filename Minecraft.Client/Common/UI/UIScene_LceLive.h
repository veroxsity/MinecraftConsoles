#pragma once

#include "UIScene.h"

// LceLive hub menu — three sub-sections navigated via the scene nav-stack.
// Modelled on UIScene_HelpAndOptionsMenu.
// Reuses the HelpAndOptionsMenu Iggy movie (7-button layout); unused slots are
// removed so only Linking / Friends / Requests appear.

#define BUTTON_LCELIVE_LINKING      0
#define BUTTON_LCELIVE_FRIENDS      1
#define BUTTON_LCELIVE_REQUESTS     2
// Slots 3-6 exist in the SWF and must be removed at construction time
#define BUTTON_LCELIVE_UNUSED_3     3
#define BUTTON_LCELIVE_UNUSED_4     4
#define BUTTON_LCELIVE_UNUSED_5     5
#define BUTTON_LCELIVE_UNUSED_6     6
#define BUTTONS_LCELIVE_TOTAL       BUTTON_LCELIVE_UNUSED_6 + 1

class UIScene_LceLive : public UIScene
{
private:
	UIControl_Button m_buttons[BUTTONS_LCELIVE_TOTAL];
	UI_BEGIN_MAP_ELEMENTS_AND_NAMES(UIScene)
		UI_MAP_ELEMENT(m_buttons[BUTTON_LCELIVE_LINKING],   "Button1")
		UI_MAP_ELEMENT(m_buttons[BUTTON_LCELIVE_FRIENDS],   "Button2")
		UI_MAP_ELEMENT(m_buttons[BUTTON_LCELIVE_REQUESTS],  "Button3")
		UI_MAP_ELEMENT(m_buttons[BUTTON_LCELIVE_UNUSED_3],  "Button4")
		UI_MAP_ELEMENT(m_buttons[BUTTON_LCELIVE_UNUSED_4],  "Button5")
		UI_MAP_ELEMENT(m_buttons[BUTTON_LCELIVE_UNUSED_5],  "Button6")
		UI_MAP_ELEMENT(m_buttons[BUTTON_LCELIVE_UNUSED_6],  "Button7")
	UI_END_MAP_ELEMENTS_AND_NAMES()

public:
	UIScene_LceLive(int iPad, void *initData, UILayer *parentLayer);
	virtual ~UIScene_LceLive();

	virtual EUIScene getSceneType() { return eUIScene_LceLive; }

	virtual void updateTooltips();
	virtual void updateComponents();

protected:
	virtual wstring getMoviePath();

public:
	virtual void handleReload();
	virtual void handleInput(int iPad, int key, bool repeat, bool pressed, bool released, bool &handled);

protected:
	void handlePress(F64 controlId, F64 childId);
};
