/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: Diplomacy.cpp ///////////////////////////////////////////////////////////////////////
// Author: Matthew D. Campbell - August 2002
// Desc: GUI callbacks for the diplomacy menu
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GlobalData.h"
#include "Common/MultiplayerSettings.h"
#include "Common/Player.h"
#if defined(RTS_ZEROHOUR)
#include "Common/AlliedMoneyTransfer.h"
#include "Common/MessageStream.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetSlider.h"
#endif
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/Recorder.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/Diplomacy.h"
#include "GameClient/DisconnectMenu.h"
#include "GameClient/GameWindow.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetRadioButton.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameText.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/GameSpy/BuddyDefs.h"
#include "GameNetwork/GameSpy/PeerDefs.h"


//-------------------------------------------------------------------------------------------------

static NameKeyType staticTextPlayerID[MAX_SLOTS];
static NameKeyType staticTextSideID[MAX_SLOTS];
static NameKeyType staticTextTeamID[MAX_SLOTS];
static NameKeyType staticTextStatusID[MAX_SLOTS];
static NameKeyType buttonMuteID[MAX_SLOTS];
static NameKeyType buttonUnMuteID[MAX_SLOTS];
static NameKeyType radioButtonInGameID = NAMEKEY_INVALID;
static NameKeyType radioButtonBuddiesID = NAMEKEY_INVALID;
static GameWindow *radioButtonInGame = nullptr;
static GameWindow *radioButtonBuddies = nullptr;
static NameKeyType winInGameID = NAMEKEY_INVALID;
static NameKeyType winBuddiesID = NAMEKEY_INVALID;
static NameKeyType winSoloID = NAMEKEY_INVALID;
static GameWindow *winInGame = nullptr;
static GameWindow *winBuddies = nullptr;
static GameWindow *winSolo = nullptr;
static GameWindow *staticTextPlayer[MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static GameWindow *staticTextSide[MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static GameWindow *staticTextTeam[MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static GameWindow *staticTextStatus[MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static GameWindow *buttonMute[MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static GameWindow *buttonUnMute[MAX_SLOTS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
static Int slotNumInRow[MAX_SLOTS];

//-------------------------------------------------------------------------------------------------

static WindowLayout *theLayout = nullptr;
static GameWindow *theWindow = nullptr;
static AnimateWindowManager *theAnimateWindowManager = nullptr;
WindowMsgHandledType BuddyControlSystem( GameWindow *window, UnsignedInt msg,
														 WindowMsgData mData1, WindowMsgData mData2);
void InitBuddyControls(Int type);
void updateBuddyInfo();
#if defined(RTS_ZEROHOUR)
static GameWindow *buttonGiveMoney[MAX_SLOTS] = {nullptr};
static GameWindow *moneyAmountSlider = nullptr;
static GameWindow *moneyAmountLabel = nullptr;
static GameWindow *moneyAmountLess = nullptr;
static GameWindow *moneyAmountMore = nullptr;
static Int selectedMoneyAmount = 1000;

static Bool canShowMoneyControls()
{
	if (!TheGameLogic || !TheRecorder || TheRecorder->isPlaybackMode())
		return false;
	const GameMode mode = TheGameLogic->getGameMode();
	return mode == GAME_SKIRMISH || mode == GAME_LAN || mode == GAME_INTERNET;
}

static Bool isEligibleMoneyRecipient(Player *donor, Player *recipient)
{
	return donor && recipient && donor != recipient && donor->isPlayerActive() &&
		recipient->isPlayerActive() && donor->getDefaultTeam() && recipient->getDefaultTeam() &&
		donor->getRelationship(recipient->getDefaultTeam()) == ALLIES &&
		recipient->getRelationship(donor->getDefaultTeam()) == ALLIES;
}

static GameWindow *createMoneyButton(GameWindow *parent, const char *name,
	Int x, Int y, Int width, Int height, GameFont *font, const WideChar *text)
{
	WinInstanceData data;
	data.m_id = NAMEKEY(name);
	data.m_style = GWS_PUSH_BUTTON | GWS_MOUSE_TRACK;
	GameWindow *button = TheWindowManager->gogoGadgetPushButton(parent,
		WIN_STATUS_ENABLED | WIN_STATUS_NO_FOCUS, x, y, width, height, &data, font, TRUE);
	if (button)
	{
		button->winSetOwner(theWindow);
		GadgetButtonSetText(button, UnicodeString(text));
	}
	return button;
}

static void createMoneyControls()
{
	if (moneyAmountSlider || !winInGame || !theWindow)
		return;
	GameWindow *hide = TheWindowManager->winGetWindowFromId(theWindow, NAMEKEY("Diplomacy.wnd:ButtonHide"));
	if (!hide)
		return;
	Int hideX, hideY, hideWidth, hideHeight;
	hide->winGetPosition(&hideX, &hideY);
	hide->winGetSize(&hideWidth, &hideHeight);
	GameFont *font = staticTextSide[0] ? staticTextSide[0]->winGetInstanceData()->getFont() :
		hide->winGetInstanceData()->getFont();
	// Anchor to the installed layout's footer, retaining its resolution scaling.
	const Int left = hideX / 32;
	const Int usableWidth = hideX - left * 2;
	if (usableWidth < 200 || hideHeight < 12)
		return;
	const Int labelWidth = usableWidth * 2 / 5;
	const Int buttonWidth = usableWidth / 16;
	const Int sliderWidth = usableWidth - labelWidth - 2 * buttonWidth - left * 3;
	WinInstanceData data;
	data.m_id = NAMEKEY("Diplomacy.wnd:AlliedMoneyAmountLabel");
	data.m_style = GWS_STATIC_TEXT;
	TextData textData;
	memset(&textData, 0, sizeof(textData));
	textData.centeredVertically = TRUE;
	moneyAmountLabel = TheWindowManager->gogoGadgetStaticText(theWindow, WIN_STATUS_ENABLED,
		left, hideY, labelWidth, hideHeight, &data, &textData, font, TRUE);
	moneyAmountLess = createMoneyButton(theWindow, "Diplomacy.wnd:AlliedMoneyLess",
		left + labelWidth, hideY, buttonWidth, hideHeight, font, L"-");
	data.init();
	data.m_id = NAMEKEY("Diplomacy.wnd:AlliedMoneySlider");
	data.m_style = GWS_HORZ_SLIDER | GWS_MOUSE_TRACK;
	SliderData sliderData;
	memset(&sliderData, 0, sizeof(sliderData));
	sliderData.minVal = 1;
	sliderData.maxVal = AlliedMoneyTransfer::MAX_HUMAN_AMOUNT / AlliedMoneyTransfer::HUMAN_AMOUNT_STEP;
	moneyAmountSlider = TheWindowManager->gogoGadgetSlider(theWindow, WIN_STATUS_ENABLED,
		left * 2 + labelWidth + buttonWidth, hideY, sliderWidth, hideHeight,
		&data, &sliderData, font, TRUE);
	if (moneyAmountSlider)
		moneyAmountSlider->winSetOwner(theWindow);
	moneyAmountMore = createMoneyButton(theWindow, "Diplomacy.wnd:AlliedMoneyMore",
		left * 3 + labelWidth + buttonWidth + sliderWidth, hideY,
		buttonWidth, hideHeight, font, L"+");
	for (Int row = 0; row < MAX_SLOTS; ++row)
	{
		if (!staticTextSide[row])
			continue;
		Int x, y, width, height;
		staticTextSide[row]->winGetPosition(&x, &y);
		staticTextSide[row]->winGetSize(&width, &height);
		const Int giveWidth = width / 4;
		const Int gap = width / 32;
		AsciiString name;
		name.format("Diplomacy.wnd:GiveMoney%d", row);
		buttonGiveMoney[row] = createMoneyButton(winInGame, name.str(),
			x + width - giveWidth, y, giveWidth, height,
			staticTextSide[row]->winGetInstanceData()->getFont(), L"Give");
		if (buttonGiveMoney[row])
			staticTextSide[row]->winSetSize(width - giveWidth - gap, height);
	}
}

static void updateMoneyControls()
{
	const Bool visible = canShowMoneyControls() && winInGame && !winInGame->winIsHidden();
	Player *donor = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	UnsignedInt maxAmount = donor ? donor->getMoney()->countMoney() : 0;
	if (maxAmount > AlliedMoneyTransfer::MAX_HUMAN_AMOUNT)
		maxAmount = AlliedMoneyTransfer::MAX_HUMAN_AMOUNT;
	maxAmount -= maxAmount % AlliedMoneyTransfer::HUMAN_AMOUNT_STEP;
	if (selectedMoneyAmount > static_cast<Int>(maxAmount))
		selectedMoneyAmount = static_cast<Int>(maxAmount);
	if (selectedMoneyAmount < AlliedMoneyTransfer::MIN_HUMAN_AMOUNT)
		selectedMoneyAmount = AlliedMoneyTransfer::MIN_HUMAN_AMOUNT;
	GameWindow *controls[] = {moneyAmountLabel, moneyAmountSlider, moneyAmountLess, moneyAmountMore};
	for (Int control = 0; control < 4; ++control)
		if (controls[control])
			controls[control]->winHide(!visible);
	const Bool donorEnabled = visible && donor && donor->isPlayerActive() && maxAmount >= 100;
	if (moneyAmountSlider)
	{
		Int sliderMax = static_cast<Int>(maxAmount / AlliedMoneyTransfer::HUMAN_AMOUNT_STEP);
		// A two-position disabled slider avoids the gadget's division by zero at a single value.
		if (sliderMax < 2)
			sliderMax = 2;
		Int oldMin, oldMax;
		GadgetSliderGetMinMax(moneyAmountSlider, &oldMin, &oldMax);
		if (oldMax != sliderMax)
			TheWindowManager->winSendSystemMsg(moneyAmountSlider, GSM_SET_MIN_MAX, 1, sliderMax);
		GadgetSliderSetPosition(moneyAmountSlider, selectedMoneyAmount / AlliedMoneyTransfer::HUMAN_AMOUNT_STEP);
		moneyAmountSlider->winEnable(donorEnabled && maxAmount > 100);
	}
	if (moneyAmountLess)
		moneyAmountLess->winEnable(donorEnabled && selectedMoneyAmount > 100);
	if (moneyAmountMore)
		moneyAmountMore->winEnable(donorEnabled && selectedMoneyAmount < static_cast<Int>(maxAmount));
	if (moneyAmountLabel)
	{
		UnicodeString label;
		label.format(L"Give $%d (max $%u)", selectedMoneyAmount, maxAmount);
		GadgetStaticTextSetText(moneyAmountLabel, label);
	}
	for (Int row = 0; row < MAX_SLOTS; ++row)
	{
		if (!buttonGiveMoney[row])
			continue;
		Player *recipient = slotNumInRow[row] >= 0 ?
			ThePlayerList->getPlayerFromSlotIndex(slotNumInRow[row]) : nullptr;
		const Bool eligible = visible && isEligibleMoneyRecipient(donor, recipient);
		buttonGiveMoney[row]->winHide(!eligible);
		buttonGiveMoney[row]->winEnable(eligible && donorEnabled &&
			AlliedMoneyTransfer::CanTransfer(selectedMoneyAmount, donor->getMoney()->countMoney(),
				recipient->getMoney()->countMoney(), TRUE, TRUE, TRUE, TRUE));
	}
}

static void resetMoneyControls()
{
	for (Int row = 0; row < MAX_SLOTS; ++row)
		buttonGiveMoney[row] = nullptr;
	moneyAmountSlider = nullptr;
	moneyAmountLabel = nullptr;
	moneyAmountLess = nullptr;
	moneyAmountMore = nullptr;
	selectedMoneyAmount = 1000;
}
#endif
static void grabWindowPointers()
{
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		AsciiString temp;
		temp.format("Diplomacy.wnd:StaticTextPlayer%d", i);
		staticTextPlayerID[i] = NAMEKEY(temp);
		temp.format("Diplomacy.wnd:StaticTextSide%d", i);
		staticTextSideID[i] = NAMEKEY(temp);
		temp.format("Diplomacy.wnd:StaticTextTeam%d", i);
		staticTextTeamID[i] = NAMEKEY(temp);
		temp.format("Diplomacy.wnd:StaticTextStatus%d", i);
		staticTextStatusID[i] = NAMEKEY(temp);
		temp.format("Diplomacy.wnd:ButtonMute%d", i);
		buttonMuteID[i] = NAMEKEY(temp);
		temp.format("Diplomacy.wnd:ButtonUnMute%d", i);
		buttonUnMuteID[i] = NAMEKEY(temp);

		staticTextPlayer[i] = TheWindowManager->winGetWindowFromId(theWindow, staticTextPlayerID[i]);
		staticTextSide[i] = TheWindowManager->winGetWindowFromId(theWindow, staticTextSideID[i]);
		staticTextTeam[i] = TheWindowManager->winGetWindowFromId(theWindow, staticTextTeamID[i]);
		staticTextStatus[i] = TheWindowManager->winGetWindowFromId(theWindow, staticTextStatusID[i]);
		buttonMute[i] = TheWindowManager->winGetWindowFromId(theWindow, buttonMuteID[i]);
		buttonUnMute[i] = TheWindowManager->winGetWindowFromId(theWindow, buttonUnMuteID[i]);

		slotNumInRow[i] = -1;
	}
}

static void releaseWindowPointers()
{
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		staticTextPlayer[i] = nullptr;
		staticTextSide[i] = nullptr;
		staticTextTeam[i] = nullptr;
		staticTextStatus[i] = nullptr;
		buttonMute[i] = nullptr;
		buttonUnMute[i] = nullptr;

		slotNumInRow[i] = -1;
	}
}


//-------------------------------------------------------------------------------------------------

static void updateFunc( WindowLayout *layout, void *param )
{
#if defined(RTS_ZEROHOUR)
	if (theWindow && !theWindow->winIsHidden())
		updateMoneyControls();
#endif
	if (theAnimateWindowManager && TheGlobalData->m_animateWindows)
	{
		Bool wasFinished = theAnimateWindowManager->isFinished();
		theAnimateWindowManager->update();
		if (theAnimateWindowManager->isFinished() && !wasFinished && theAnimateWindowManager->isReversed())
			theWindow->winHide( TRUE );
	}
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
static BriefingList theBriefingList;

//-------------------------------------------------------------------------------------------------
BriefingList* GetBriefingTextList()
{
	return &theBriefingList;
}

//-------------------------------------------------------------------------------------------------
void UpdateDiplomacyBriefingText(AsciiString newText, Bool clear)
{
	GameWindow *listboxSolo = TheWindowManager->winGetWindowFromId(theWindow, NAMEKEY("Diplomacy.wnd:ListboxSolo"));

	if (clear)
	{
		theBriefingList.clear();
		if (listboxSolo)
			GadgetListBoxReset(listboxSolo);
	}

	if (newText.isEmpty())
		return;

	if (std::find(theBriefingList.begin(), theBriefingList.end(), newText) != theBriefingList.end())
		return;

	theBriefingList.push_back(newText);
	if (!listboxSolo)
		return;

	UnicodeString translated = TheGameText->fetch(newText);

	Int numEntries = GadgetListBoxGetNumEntries(listboxSolo);
	GadgetListBoxAddEntryText(listboxSolo, translated, TheInGameUI->getMessageColor(numEntries%2), -1);
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void ShowDiplomacy( Bool immediate )
{
	if (!TheInGameUI->getInputEnabled() || TheGameLogic->isIntroMoviePlaying() ||
			TheGameLogic->isLoadingMap())
		return;


	if (TheInGameUI->isQuitMenuVisible())
		return;

	if (TheDisconnectMenu && TheDisconnectMenu->isScreenVisible())
		return;

	if (theWindow)
	{
		theWindow->winHide(FALSE);
		theWindow->winEnable(TRUE);
	}
	else
	{
		theLayout = TheWindowManager->winCreateLayout( "Diplomacy.wnd" );
		theWindow = theLayout->getFirstWindow();
		theLayout->setUpdate(updateFunc);
		theAnimateWindowManager = NEW AnimateWindowManager;
		radioButtonInGameID = TheNameKeyGenerator->nameToKey("Diplomacy.wnd:RadioButtonInGame");
		radioButtonBuddiesID = TheNameKeyGenerator->nameToKey("Diplomacy.wnd:RadioButtonBuddies");
		radioButtonInGame = TheWindowManager->winGetWindowFromId(nullptr, radioButtonInGameID);
		radioButtonBuddies = TheWindowManager->winGetWindowFromId(nullptr, radioButtonBuddiesID);
		winInGameID = TheNameKeyGenerator->nameToKey("Diplomacy.wnd:InGameParent");
		winBuddiesID = TheNameKeyGenerator->nameToKey("Diplomacy.wnd:BuddiesParent");
		winSoloID = TheNameKeyGenerator->nameToKey("Diplomacy.wnd:SoloParent");
		winInGame = TheWindowManager->winGetWindowFromId(nullptr, winInGameID);
		winBuddies = TheWindowManager->winGetWindowFromId(nullptr, winBuddiesID);
		winSolo = TheWindowManager->winGetWindowFromId(nullptr, winSoloID);

		if (!TheRecorder->isMultiplayer())
		{
			GameWindow *listboxSolo = TheWindowManager->winGetWindowFromId(theWindow, NAMEKEY("Diplomacy.wnd:ListboxSolo"));
			if (listboxSolo)
			{
				for (BriefingList::iterator it = theBriefingList.begin(); it != theBriefingList.end(); ++it)
				{
					UnicodeString translated = TheGameText->fetch(*it);
					Int numEntries = GadgetListBoxGetNumEntries(listboxSolo);
					GadgetListBoxAddEntryText(listboxSolo, translated, TheInGameUI->getMessageColor(numEntries%2), -1);
				}
			}
		}
	}
	theLayout->hide(FALSE);

	radioButtonInGame->winHide(TRUE);
	radioButtonBuddies->winHide(TRUE);
	GadgetRadioSetSelection(radioButtonInGame, FALSE);
	if (TheRecorder->isMultiplayer()
#if defined(RTS_ZEROHOUR)
		|| TheGameLogic->isInSkirmishGame()
#endif
		)
	{
		winInGame->winHide(FALSE);
		winBuddies->winHide(TRUE);
		winSolo->winHide(TRUE);
	}
	else
	{
		winInGame->winHide(TRUE);
		winBuddies->winHide(TRUE);
		winSolo->winHide(FALSE);
	}

	theAnimateWindowManager->reset();
	if (!immediate && TheGlobalData->m_animateWindows)
		theAnimateWindowManager->registerGameWindow( theWindow, WIN_ANIMATION_SLIDE_TOP, TRUE, 200 );

	TheInGameUI->registerWindowLayout(theLayout);
	grabWindowPointers();
	PopulateInGameDiplomacyPopup();
#if defined(RTS_ZEROHOUR)
	createMoneyControls();
	updateMoneyControls();
#endif

	if(TheGameSpyInfo && TheGameSpyInfo->getLocalProfileID() != 0)
	{
		radioButtonInGame->winHide(FALSE);
		radioButtonBuddies->winHide(FALSE);
		InitBuddyControls(1);
		PopulateOldBuddyMessages();
		updateBuddyInfo();
	}

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void ResetDiplomacy()
{
#if defined(RTS_ZEROHOUR)
	resetMoneyControls();
#endif
	if(theLayout)
	{
		TheInGameUI->unregisterWindowLayout(theLayout);
		theLayout->destroyWindows();
		deleteInstance(theLayout);
		InitBuddyControls(-1);
		theLayout = nullptr;
	}
	theWindow = nullptr;

	delete theAnimateWindowManager;
	theAnimateWindowManager = nullptr;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void HideDiplomacy( Bool immediate )
{
	releaseWindowPointers();
	if (theWindow)
	{
		if (immediate || !TheGlobalData->m_animateWindows)
		{
			theWindow->winHide(TRUE);
			theWindow->winEnable(FALSE);
		}
		else
		{
			if (theAnimateWindowManager->isFinished())
				theAnimateWindowManager->reverseAnimateWindow();
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void ToggleDiplomacy( Bool immediate )
{
	// If we bring this up, let's hide the quit menu
	HideQuitMenu();

	if (theWindow)
	{
		Bool show = theWindow->winIsHidden();
		if (show)
			ShowDiplomacy( immediate );
		else
			HideDiplomacy( immediate );
	}
	else
	{
		ShowDiplomacy( immediate );
	}
}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType DiplomacyInput( GameWindow *window, UnsignedInt msg,
																			WindowMsgData mData1, WindowMsgData mData2 )
{

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = static_cast<UnsignedByte>(WindowMsgDataToUnsignedInt(mData1));
//			UnsignedByte state = mData2;

			switch( key )
			{

				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{
					HideDiplomacy();
					return MSG_HANDLED;
					//return MSG_IGNORED;
				}

			}

			return MSG_HANDLED;

		}

	}

	return MSG_IGNORED;

}

//-------------------------------------------------------------------------------------------------
WindowMsgHandledType DiplomacySystem( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	if(BuddyControlSystem(window, msg, mData1, mData2) == MSG_HANDLED)
	{
		return MSG_HANDLED;
	}
	switch( msg )
	{
#if defined(RTS_ZEROHOUR)
		case GSM_SLIDER_TRACK:
		case GSM_SLIDER_DONE:
		{
			if (moneyAmountSlider && WindowMsgDataToPointer(mData1) == moneyAmountSlider)
			{
				selectedMoneyAmount = GadgetSliderGetPosition(moneyAmountSlider) *
					AlliedMoneyTransfer::HUMAN_AMOUNT_STEP;
				updateMoneyControls();
				return MSG_HANDLED;
			}
			break;
		}
#endif
		//---------------------------------------------------------------------------------------------
		case GGM_FOCUS_CHANGE:
		{
//			Bool focus = (Bool) mData1;
			//if (focus)
				//TheWindowManager->winSetGrabWindow( chatTextEntry );
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{
			// if we're given the opportunity to take the keyboard focus we must say we don't want it
			if( mData1 == TRUE )
				*static_cast<Bool *>(WindowMsgDataToPointer(mData2)) = FALSE;

			return MSG_HANDLED;
		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			GameWindow *control = static_cast<GameWindow *>(WindowMsgDataToPointer(mData1));
#if defined(RTS_ZEROHOUR)
			if (control && (control == moneyAmountLess || control == moneyAmountMore))
			{
				selectedMoneyAmount += control == moneyAmountLess ?
					-AlliedMoneyTransfer::HUMAN_AMOUNT_STEP : AlliedMoneyTransfer::HUMAN_AMOUNT_STEP;
				updateMoneyControls();
				return MSG_HANDLED;
			}
			for (Int row = 0; row < MAX_SLOTS; ++row)
			{
				if (!control || control != buttonGiveMoney[row])
					continue;
				Player *donor = ThePlayerList->getLocalPlayer();
				Player *recipient = slotNumInRow[row] >= 0 ?
					ThePlayerList->getPlayerFromSlotIndex(slotNumInRow[row]) : nullptr;
				if (!canShowMoneyControls() || !isEligibleMoneyRecipient(donor, recipient) ||
					!AlliedMoneyTransfer::IsHumanAmountValid(selectedMoneyAmount) ||
					!AlliedMoneyTransfer::CanTransfer(selectedMoneyAmount, donor->getMoney()->countMoney(),
						recipient->getMoney()->countMoney(), TRUE, TRUE, TRUE, TRUE))
				{
					TheInGameUI->message(UnicodeString(L"Cannot give money. Check your cash and the ally's status."));
					return MSG_HANDLED;
				}
				GameMessage *command = TheMessageStream->appendMessage(GameMessage::MSG_TRANSFER_MONEY_TO_ALLY);
				command->appendIntegerArgument(recipient->getPlayerIndex());
				command->appendIntegerArgument(selectedMoneyAmount);
				TheInGameUI->message(UnicodeString(L"Queued $%d for %ls."),
					selectedMoneyAmount, recipient->getPlayerDisplayName().str());
				return MSG_HANDLED;
			}
#endif
			NameKeyType controlID = (NameKeyType)control->winGetWindowId();
			static NameKeyType buttonHideID = NAMEKEY( "Diplomacy.wnd:ButtonHide" );
			if (controlID == buttonHideID)
			{
				HideDiplomacy( FALSE );
			}
			else if( controlID == radioButtonInGameID)
			{
				winInGame->winHide(FALSE);
				winBuddies->winHide(TRUE);
			}
			else if( controlID == radioButtonBuddiesID)
			{
				winInGame->winHide(TRUE);
				winBuddies->winHide(FALSE);
			}

			for (Int i=0; i<MAX_SLOTS; ++i)
			{
				if (controlID == buttonMuteID[i] && slotNumInRow[i] >= 0)
				{
					TheGameInfo->getSlot(slotNumInRow[i])->mute(TRUE);
					PopulateInGameDiplomacyPopup();
					break;
				}
				if (controlID == buttonUnMuteID[i] && slotNumInRow[i] >= 0)
				{
					TheGameInfo->getSlot(slotNumInRow[i])->mute(FALSE);
					PopulateInGameDiplomacyPopup();
					break;
				}
			}
			break;

		}

		//---------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}

void PopulateInGameDiplomacyPopup()
{
	if (!TheGameInfo)
		return;

	Int rowNum = 0;
	for (Int slotNum=0; slotNum<MAX_SLOTS; ++slotNum)
	{
		const GameSlot *slot = TheGameInfo->getConstSlot(slotNum);
		if (slot && slot->isOccupied())
		{
			Bool isInGame = false;
			// Note - for skirmish, TheNetwork == nullptr.  jba.
			if (TheNetwork &&	TheNetwork->isPlayerConnected(slotNum)) {
				isInGame = true;
			} else if ((TheNetwork == nullptr) && slot->isHuman()) {
				// this is a skirmish game and it is the human player.
				isInGame = true;
			}
			if (slot->isAI())
				isInGame = true;
			Player *player = ThePlayerList->getPlayerFromSlotIndex(slotNum);
			Bool isAlive = !TheVictoryConditions->hasSinglePlayerBeenDefeated(player);
			Bool isObserver = player->isPlayerObserver();

			if (slot->isHuman() && TheGameInfo->getLocalSlotNum() != slotNum && isInGame)
			{
				// show mute button
				if (buttonMute[rowNum])
				{
					buttonMute[rowNum]->winHide(slot->isMuted());
				}
				if (buttonUnMute[rowNum])
				{
					buttonUnMute[rowNum]->winHide(!slot->isMuted());
				}
			}
			else
			{
				// can't mute self, AI players, or MIA humans
				if (buttonMute[rowNum])
					buttonMute[rowNum]->winHide(TRUE);
				if (buttonUnMute[rowNum])
					buttonUnMute[rowNum]->winHide(TRUE);
			}

			Color playerColor = TheMultiplayerSettings->getColor(slot->getApparentColor())->getColor();
			Color backColor = GameMakeColor(0, 0, 0, 255);
			Color aliveColor = GameMakeColor(0, 255, 0, 255);
			Color deadColor = GameMakeColor(255, 0, 0, 255);
			Color observerInGameColor = GameMakeColor(255, 255, 255, 255);
			Color goneColor = GameMakeColor(196, 0, 0, 255);
			Color observerGoneColor = GameMakeColor(196, 196, 196, 255);

			if (staticTextPlayer[rowNum])
			{
				staticTextPlayer[rowNum]->winSetEnabledTextColors( playerColor, backColor );
				GadgetStaticTextSetText(staticTextPlayer[rowNum], slot->getName());
			}
			if (staticTextSide[rowNum])
			{
				staticTextSide[rowNum]->winSetEnabledTextColors( playerColor, backColor );
				GadgetStaticTextSetText(staticTextSide[rowNum], slot->getApparentPlayerTemplateDisplayName() );
			}
			if (staticTextTeam[rowNum])
			{
				staticTextTeam[rowNum]->winSetEnabledTextColors( playerColor, backColor );
				AsciiString teamStr;
				teamStr.format("Team:%d", slot->getTeamNumber() + 1);
				if (slot->isAI() && slot->getTeamNumber() == -1)
					teamStr = "Team:AI";
				GadgetStaticTextSetText(staticTextTeam[rowNum], TheGameText->fetch(teamStr) );
			}
			if (staticTextStatus[rowNum])
			{
				staticTextStatus[rowNum]->winHide(FALSE);
				if (isInGame)
				{
					if (isAlive)
					{
						staticTextStatus[rowNum]->winSetEnabledTextColors( aliveColor, backColor );
						GadgetStaticTextSetText(staticTextStatus[rowNum], TheGameText->fetch("GUI:PlayerAlive"));
					}
					else
					{
						if (isObserver)
						{
							staticTextStatus[rowNum]->winSetEnabledTextColors( observerInGameColor, backColor );
							GadgetStaticTextSetText(staticTextStatus[rowNum], TheGameText->fetch("GUI:PlayerObserver"));
						}
						else
						{
							staticTextStatus[rowNum]->winSetEnabledTextColors( deadColor, backColor );
							GadgetStaticTextSetText(staticTextStatus[rowNum], TheGameText->fetch("GUI:PlayerDead"));
						}
					}
				}
				else
				{
					// not in game
					if (isObserver)
					{
						staticTextStatus[rowNum]->winSetEnabledTextColors( observerGoneColor, backColor );
						GadgetStaticTextSetText(staticTextStatus[rowNum], TheGameText->fetch("GUI:PlayerObserverGone"));
					}
					else
					{
						staticTextStatus[rowNum]->winSetEnabledTextColors( goneColor, backColor );
						GadgetStaticTextSetText(staticTextStatus[rowNum], TheGameText->fetch("GUI:PlayerGone"));
					}
				}
			}

			slotNumInRow[rowNum++] = slotNum;
		}
	}

	while (rowNum < MAX_SLOTS)
	{
		slotNumInRow[rowNum] = -1;
		if (staticTextPlayer[rowNum])
			staticTextPlayer[rowNum]->winHide(TRUE);
		if (staticTextSide[rowNum])
			staticTextSide[rowNum]->winHide(TRUE);
		if (staticTextTeam[rowNum])
			staticTextTeam[rowNum]->winHide(TRUE);
		if (staticTextStatus[rowNum])
			staticTextStatus[rowNum]->winHide(TRUE);
		if (buttonMute[rowNum])
			buttonMute[rowNum]->winHide(TRUE);
		if (buttonUnMute[rowNum])
			buttonUnMute[rowNum]->winHide(TRUE);

		++rowNum;
	}
}



