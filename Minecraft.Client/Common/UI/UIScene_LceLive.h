#pragma once

#include "UIScene.h"

class UIScene_LceLive : public UIScene
{
private:
	enum EControls
	{
		eControl_PrimaryAction,
	};

	bool m_buttonEnabled;
	bool m_descriptionApplied;
	std::wstring m_lastButtonLabel;
	std::wstring m_lastDescription;

	UIControl_Button m_buttonPrimaryAction;
	UIControl_Label m_labelTitle;
	UIControl_Label m_labelDescription;
	IggyName m_funcInit;
	IggyName m_funcAutoResize;
	UI_BEGIN_MAP_ELEMENTS_AND_NAMES(UIScene)
		UI_MAP_ELEMENT(m_buttonPrimaryAction, "Button3")
		UI_MAP_ELEMENT(m_labelTitle, "Title")
		UI_MAP_ELEMENT(m_labelDescription, "Content")
		UI_MAP_NAME(m_funcInit, L"Init")
		UI_MAP_NAME(m_funcAutoResize, L"AutoResize")
	UI_END_MAP_ELEMENTS_AND_NAMES()

public:
	UIScene_LceLive(int iPad, void *initData, UILayer *parentLayer);
	~UIScene_LceLive();

	virtual EUIScene getSceneType() { return eUIScene_LceLive; }
	virtual void updateTooltips();
	virtual void updateComponents();
	virtual void tick();

protected:
	virtual wstring getMoviePath();
	virtual void handleReload();

public:
	virtual void handleInput(int iPad, int key, bool repeat, bool pressed, bool released, bool &handled);

protected:
	void handlePress(F64 controlId, F64 childId);

private:
	void RefreshUi(bool force);
	void ApplyDescription(const std::wstring &description);
};
