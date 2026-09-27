#pragma once

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM) || defined(CTR_PLATFORM)

#include "Gui.h"
#include "java/Type.h"

class GuiTextField;
class FontRenderer;

// On-screen keyboard for PS2, Wii and 3DS. It behaves like SDL2 text input: it pops up while a
// GuiTextField is focused (chat, world name/seed, rename, sign...), is navigated
// with the D-pad and typed with Cross. Characters are injected into the
// lwjgl::Keyboard event queue, so the focused field receives them through the
// normal keyTyped() path -- no per-screen wiring needed.
//
// The 3DS prefers the SYSTEM keyboard (swkbd, src/3ds/DsSwkbd.h): it opens on
// the tick after focus arrives, and on OK it fills the field and leaves it
// unfocused, so a confirm press can then submit it (chat sends on KEY_RETURN,
// which the menu channel pushes for A). When the applet cannot be used, the
// panel below draws on the bottom LCD instead of over the top screen and the
// pad plus touch type as on the other consoles.
class VirtualKeyboard : public Gui
{
public:
	static VirtualKeyboard& instance();

	// Called by GuiTextField::setFocused(). The keyboard is active while a field
	// is focused; focusing a different field resets the selection.
	void notifyFocus(GuiTextField* field, bool focused);
	bool isActive() const { return focusedField != nullptr; }

	// Per-frame while active: read the pad and inject input events.
	void tick();
	// Draw the keyboard panel (call after the screen is drawn, in scaled coords).
	void render(FontRenderer* font, int_t screenWidth, int_t screenHeight);

private:
	VirtualKeyboard() = default;
	void resetSelection();
	// Inject the character under the selection as a typed event.
	void typeSelectedKey();
#if defined(CTR_PLATFORM)
	// Launch the system keyboard for the focused field (see tick()).
	void openNativeKeyboard();
#endif

	GuiTextField*  focusedField = nullptr;
	int_t          selX = 0;
	int_t          selY = 0;
	bool           shift = false;
	unsigned int lastHeld = 0;
	int            nextRepeatMs = 0;
	int_t          lastScreenWidth = 0;
	int_t          lastScreenHeight = 0;
	float_t        panelX = 0.0f;
	float_t        panelY = 0.0f;
	int            lastMoveMs = 0;
	bool           panelPositionInitialized = false;
#if defined(CTR_PLATFORM)
	// A focus event launched the system keyboard on the next tick (deferred
	// so the screen finishes wiring the field first).
	bool           pendingNativeOpen = false;
	// swkbd failed once this session: keep the bottom-screen panel for every
	// field instead of retrying the launch (see openNativeKeyboard).
	bool           nativeKeyboardFailed = false;
	// Draw on the bottom LCD rather than over the top screen.
	bool           bottomMode = false;
	// Previous tick's touch state, for the panel's tap-to-type edge.
	bool           lastPointerValid = false;
#endif
};

#endif // PS2_PLATFORM || WII_PLATFORM || CTR_PLATFORM
