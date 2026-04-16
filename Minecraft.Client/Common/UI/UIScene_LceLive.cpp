#include "stdafx.h"
#include "UI.h"
#include "UIScene_LceLive.h"
#include "../../../Minecraft.World/StringHelpers.h"

#if defined(_WINDOWS64) && !defined(MINECRAFT_SERVER_BUILD)
#include "../../Windows64/Windows64_LceLive.h"
#endif

UIScene_LceLive::UIScene_LceLive(int iPad, void *initData, UILayer *parentLayer) : UIScene(iPad, parentLayer)
{
	initialiseMovie();

	parentLayer->addComponent(iPad, eUIComponent_Panorama);
	parentLayer->addComponent(iPad, eUIComponent_Logo);

	m_buttonEnabled = true;
	m_descriptionApplied = false;
	m_buttonPrimaryAction.init(L"START LINK", eControl_PrimaryAction);
	m_labelTitle.init(L"LCELIVE");
	m_labelDescription.init(L"");

	IggyDataValue result;
	IggyDataValue value[2];
	value[0].type = IGGY_DATATYPE_number;
	value[0].number = 1;
	value[1].type = IGGY_DATATYPE_number;
	value[1].number = 0;
	IggyPlayerCallMethodRS(getMovie(), &result, IggyPlayerRootPath(getMovie()), m_funcInit, 2, value);
	IggyPlayerCallMethodRS(getMovie(), &result, IggyPlayerRootPath(getMovie()), m_funcAutoResize, 0, nullptr);
}

UIScene_LceLive::~UIScene_LceLive()
{
	m_parentLayer->removeComponent(eUIComponent_Panorama);
	m_parentLayer->removeComponent(eUIComponent_Logo);
}

wstring UIScene_LceLive::getMoviePath()
{
	return L"LceLive";
}

void UIScene_LceLive::updateTooltips()
{
	if (m_buttonEnabled)
		ui.SetTooltips(DEFAULT_XUI_MENU_USER, IDS_TOOLTIPS_SELECT, IDS_TOOLTIPS_BACK);
	else
		ui.SetTooltips(DEFAULT_XUI_MENU_USER, -1, IDS_TOOLTIPS_BACK);
}

void UIScene_LceLive::updateComponents()
{
	m_parentLayer->showComponent(m_iPad, eUIComponent_Panorama, true);
	m_parentLayer->showComponent(m_iPad, eUIComponent_Logo, true);
}

void UIScene_LceLive::tick()
{
	UIScene::tick();
	RefreshUi(false);
}

void UIScene_LceLive::handleReload()
{
	m_descriptionApplied = false;
	IggyDataValue result;
	IggyDataValue value[2];
	value[0].type = IGGY_DATATYPE_number;
	value[0].number = 1;
	value[1].type = IGGY_DATATYPE_number;
	value[1].number = 0;
	IggyPlayerCallMethodRS(getMovie(), &result, IggyPlayerRootPath(getMovie()), m_funcInit, 2, value);
	IggyPlayerCallMethodRS(getMovie(), &result, IggyPlayerRootPath(getMovie()), m_funcAutoResize, 0, nullptr);
	RefreshUi(true);
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
		if (pressed && !repeat && m_buttonEnabled)
		{
			handled = true;
			handlePress(static_cast<F64>(eControl_PrimaryAction), 0.0);
		}
		break;
#ifdef __ORBIS__
	case ACTION_MENU_TOUCHPAD_PRESS:
		if (pressed && !repeat && m_buttonEnabled)
		{
			handled = true;
			handlePress(static_cast<F64>(eControl_PrimaryAction), 0.0);
		}
		break;
#endif
	case ACTION_MENU_DOWN:
	case ACTION_MENU_UP:
	case ACTION_MENU_PAGEUP:
	case ACTION_MENU_PAGEDOWN:
	case ACTION_MENU_OTHER_STICK_DOWN:
	case ACTION_MENU_OTHER_STICK_UP:
		sendInputToMovie(key, repeat, pressed, released);
		break;
	}
}

void UIScene_LceLive::handlePress(F64 controlId, F64 childId)
{
	if (static_cast<int>(controlId) != eControl_PrimaryAction || !m_buttonEnabled)
		return;

#if defined(_WINDOWS64) && !defined(MINECRAFT_SERVER_BUILD)
	const Win64LceLive::Snapshot snapshot = Win64LceLive::GetSnapshot();
	ui.PlayUISFX(eSFX_Press);

	if (snapshot.state == Win64LceLive::EClientState::SignedIn)
		Win64LceLive::SignOut();
	else
		Win64LceLive::StartDeviceLink();
#endif
}

void UIScene_LceLive::RefreshUi(bool force)
{
	std::wstring buttonLabel = L"LCELIVE UNAVAILABLE";
	std::wstring description = L"LCELIVE\r\n\r\nThis build does not provide the Windows64 LCELive client runtime.";
	bool buttonEnabled = false;

#if defined(_WINDOWS64) && !defined(MINECRAFT_SERVER_BUILD)
	const Win64LceLive::Snapshot snapshot = Win64LceLive::GetSnapshot();

	switch (snapshot.state)
	{
	case Win64LceLive::EClientState::SignedIn:
		buttonLabel = L"SIGN OUT";
		buttonEnabled = !snapshot.requestInFlight;
		break;
	case Win64LceLive::EClientState::StartingLink:
	case Win64LceLive::EClientState::Polling:
		buttonLabel = L"PLEASE WAIT";
		buttonEnabled = false;
		break;
	case Win64LceLive::EClientState::LinkPending:
		buttonLabel = L"RESTART LINK";
		buttonEnabled = true;
		break;
	case Win64LceLive::EClientState::SignedOut:
	default:
		buttonLabel = L"START LINK";
		buttonEnabled = true;
		break;
	}

	description = snapshot.statusMessage;
	if (snapshot.hasError && !snapshot.errorMessage.empty())
	{
		description += L"\r\nError:\r\n";
		description += snapshot.errorMessage;
	}
#endif

	if (force || m_lastButtonLabel != buttonLabel)
	{
		m_lastButtonLabel = buttonLabel;
		m_buttonPrimaryAction.setLabel(buttonLabel, true, true);
	}

	if (force || m_buttonEnabled != buttonEnabled)
	{
		m_buttonEnabled = buttonEnabled;
		m_buttonPrimaryAction.setEnable(buttonEnabled);
	}

	if (!m_descriptionApplied || m_lastDescription != description)
	{
		m_lastDescription = description;
		ApplyDescription(description);
	}

	updateTooltips();
}

void UIScene_LceLive::ApplyDescription(const std::wstring &description)
{
	m_labelDescription.setLabel(description, true, true);

	IggyDataValue result;
	IggyPlayerCallMethodRS(getMovie(), &result, IggyPlayerRootPath(getMovie()), m_funcAutoResize, 0, nullptr);
	m_descriptionApplied = true;
}
