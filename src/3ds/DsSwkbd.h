#pragma once

// DsSwkbd.h -- the system software keyboard (the swkbd applet) as the 3DS's
// primary text-entry path. Only 3DS code includes this; VirtualKeyboard.cpp
// reaches it through its CTR_PLATFORM branch and falls back to the on-screen
// keyboard panel on the bottom screen when it reports Unavailable (see
// VirtualKeyboard::tick()).
//
// The applet only needs APT (swkbd.o references no other service), so
// resources/3ds_cia.rsf grants nothing new: the APT:U it already has covers
// aptLaunchLibraryApplet().

#include <string>

// What the player did when the applet disappeared.
enum class DsSwkbdOutcome
{
	Confirmed,   // the right button ("OK") -- result.text holds the input
	Cancelled,   // the left button ("Cancel"), or HOME/RESET/POWER
	Unavailable, // the applet never produced a dialog (bad launch, no memory);
	             // the caller falls back to the bottom-screen panel, and
	             // stops trying for the rest of the session
};

struct DsSwkbdResult
{
	DsSwkbdOutcome outcome = DsSwkbdOutcome::Unavailable;
	std::string text; // UTF-8; set only when outcome == Confirmed
};

// Open the system keyboard with `initial` pre-filled and block until it
// closes -- the applet owns both LCDs while it runs. maxLength is the field's
// maxStringLength in UTF-16 code units, the unit swkbd counts its limit in
// (the game's jstring is UTF-8; the result buffer sizes for that).
DsSwkbdResult dsSwkbdOpen(const std::string& initial, int maxLength);
