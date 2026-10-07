#pragma once
#ifdef CTR_PLATFORM

#include "lwjgl/Keyboard.h"

// Synthetic key codes for raw 3DS pad buttons.
//
// GameSettings' KeyBinding::keyCode is just an int shared with the real
// lwjgl keyboard codes (0..KEY_MAX). Reusing that same field to name a pad
// button -- instead of adding a parallel "which button" field -- means the
// existing Controls screen, its rebind flow, options.txt persistence and
// GameSettings::keyName() all work for pad buttons with zero changes: they
// just see an int they don't recognize as a "real" key and Keyboard_3ds's
// getKeyName()/dsPadKeyName() treat it specially. This is the PS2's
// Ps2PadKeyCodes contract verbatim; only the button list differs.
//
// Only buttons that carry a rebindable game action are exposed. START stays
// reserved as the pause/menu key (DsInput turns its edges into KEY_ESCAPE,
// so a rebind cannot take it away from the player), and ZL/ZR are New-3DS
// only hardware -- deliberately absent while Old 3DS/2DS is the floor; they
// join this table when phase 2 maps them.
enum DsPadKeyCode : int
{
	DS_KEY_A = lwjgl::Keyboard::KEY_MAX,
	DS_KEY_B,
	DS_KEY_X,
	DS_KEY_Y,
	DS_KEY_L,
	DS_KEY_R,
	DS_KEY_SELECT,
	DS_KEY_DPAD_UP,
	DS_KEY_DPAD_DOWN,
	DS_KEY_DPAD_LEFT,
	DS_KEY_DPAD_RIGHT,
	DS_KEY_SENTINEL_END,
	// "No button" -- this console's unbind code. Nothing ever emits it: the
	// pad-code channel speaks only the physical buttons above, and the
	// character events carry KEY_NONE = 0, which is exactly why 0 cannot
	// serve here -- Minecraft::runTick routes every event through
	// KeyBinding::setKeyBindState(), so a binding keyed 0 would press on
	// every typed character. Sitting past DS_KEY_SENTINEL_END means every
	// claim/capture range check already excludes it, so an unbind parked
	// here can never be claimed, captured or migrated by accident. The four
	// digital movement binds live here: the circle pad carries all movement
	// (PLATFORM_DIRECT_ANALOG_MOVEMENT) and the D-pad's codes are live in
	// gameplay, so a movement bind parked on one would walk the player under
	// the D-pad's fixed roles -- see GameSettingsBackend_3DS.
	DS_KEY_NONE
};

static_assert(DS_KEY_NONE < 256, "DsPadKeyCode must fit the 256-slot key-state arrays");

// Display name for the Controls screen, or nullptr if `key` isn't one of these.
const char *dsPadKeyName(int key);

#endif // CTR_PLATFORM
