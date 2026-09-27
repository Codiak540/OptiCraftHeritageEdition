// DsInput.cpp -- touchscreen + circle-pad input for the 3DS port.
//
// One self-contained libctru scan per frame, shaped into the snapshot that
// platform/InputBackend_3DS.cpp feeds into the shared Input API, plus the
// key/mouse/wheel edges the engine actually plays with.
//
// A button means different things with a screen open and with it closed, so
// each one has up to two channels:
//
//   button  menu channel (PLATFORM_TEXT_*)   gameplay channel
//   ------  -------------------------------  --------------------------------
//   A       TYPE      confirm/select         jump      -> DS_KEY_A
//   B       BACK      cancel                 use       -> mouse button 1
//   X       SPACE     GUI/virtual-kbd space  attack    -> mouse button 0
//   Y       CLOSE     exit/back-out          inventory -> DS_KEY_Y
//   L       SPACE     left shoulder          attack    -> mouse button 0
//   R       SHIFT     right shoulder         use       -> mouse button 1
//   SELECT  SPACE     (PS2 SELECT/Wii MINUS) sneak     -> DS_KEY_SELECT
//   D-pad   UP/DOWN/LEFT/RIGHT               hotbar    -> wheel +1 (LEFT) / -1 (RIGHT),
//                                                         UP/DOWN unmapped in-game
//   START   ENTER     only while typing      pause     -> KEY_ESCAPE
//   ZL/ZR   --                               unmapped (New-3DS only; phase 2)
//
//   circle pad -> stick axes (analog movement, PLATFORM_DIRECT_ANALOG_MOVEMENT)
//   touch      -> absolute pointer + button 0, in BOTH contexts: the finger
//                 is the pointer, so it keeps clicking screens (see below) --
//                 except while a text field has focus, where the on-screen
//                 keyboard owns the panel and reads RAW 320x240 coordinates
//
// Attack and Use are mouse buttons rather than keys because that is what
// GameSettings binds them to (-100 / -99) and what clickMouse() reads -- and
// L and R are the shoulders as left/right click: they funnel into the same
// button 0 / button 1 X and B already own (updateGameplay ORs the sources, so
// any hold order works), which is what the player asked for. That is also why
// they are menu-suppressed: buttons pushed while a screen is open become a
// click under the cursor (GuiScreen::handleInput() drains the queue
// unconditionally), whereas the touch tap must keep working there.
//
// The gameplay channel is dropped while a screen is open, and a button still
// physically held across the menu/gameplay boundary stays dropped until it is
// released. Without that second half the press that confirmed a menu would
// arrive again as a gameplay button the moment the world opened -- exactly
// the bug WiiPadState.cpp documents as "joining a world bounced straight back
// to the title". The PS2 solves the same problem with releaseGameplayKeys().
//
// Coordinates: the top screen (400x240) is the game surface, the bottom
// panel (320x240) is touch. Touch X is scaled into top-screen pixels
// (x * screenW / 320); Y is copied as-is because both screens are 240 tall,
// so the GUI can compare pointer coordinates against 400x240 directly. The
// exception is text entry: while a field has focus the coordinates stay in
// the panel's own 320x240 space (no scaling, no mouse-queue forwarding),
// because the fallback keyboard lays itself out there -- see dsInputPoll().
#ifdef CTR_PLATFORM

#include "3ds/input/DsInput.h"

#include <3ds.h>

#include "3ds/input/DsPadKeyCodes.h"
#include "lwjgl/Keyboard.h"
#include "lwjgl/Mouse.h"

#include <algorithm>
#include <cstdio>

namespace
{
// Top-screen geometry remembered by dsInputInit(): touch arrives in panel
// pixels and has to be mapped into the same space the GUI lives in.
int g_screenW = 400;
int g_screenH = 240;

// dsInputDebugLine() reports nullptr until the input system has been set up
// (the "idle" case DsInput.h allows).
bool g_initialized = false;

DsInputState g_state = {};

// PLATFORM_TEXT_* rising edges accumulated between polls, cleared by
// dsInputConsumePressed(): the consume-on-read contract DsInput.h shares
// with the Wii's wiiTextInputConsumePressed().
std::uint32_t g_latchedPressed = 0;

// Previous poll's mapped mask, so edges are detected against the mask rather
// than raw keys -- one physical button can map to several bits (SELECT, L, X
// all set SPACE) or to none at all (START).
std::uint32_t g_prevHeld = 0;

// Previous poll's touch sample. The panel reports an ABSOLUTE contact point,
// but the mouse queue wants relative motion too, so deltas are differenced
// here -- the same job the Wii's IR producer does in WiiPointer.cpp.
bool g_prevTouchDown = false;
int g_prevTouchX = 0;
int g_prevTouchY = 0;

// Last poll's text-exclusive state (a field focused). dsInputPoll compares
// against it to spot the transitions that change what touch and START mean.
bool g_prevTextExclusive = false;

// Gameplay channel state. Separate from PLATFORM_TEXT_*, which is the menu
// channel and keeps working with a screen open.
//
// GP_* bits are one per *action*, not per physical button, because attack and
// use are mouse buttons while jump and friends are keys and they need
// different edge handling.
constexpr std::uint32_t GP_JUMP        = 1u << 0;
constexpr std::uint32_t GP_INVENTORY   = 1u << 1;
constexpr std::uint32_t GP_SNEAK       = 1u << 2;
constexpr std::uint32_t GP_USE         = 1u << 3; // mouse button 1 (B or R)
constexpr std::uint32_t GP_ATTACK      = 1u << 4; // mouse button 0 (X or L)
constexpr std::uint32_t GP_DPAD_UP     = 1u << 5;
constexpr std::uint32_t GP_DPAD_DOWN   = 1u << 6;
constexpr std::uint32_t GP_DPAD_LEFT   = 1u << 7; // wheel +1 (previous slot)
constexpr std::uint32_t GP_DPAD_RIGHT  = 1u << 8; // wheel -1 (next slot)

// Latched by dsInputPoll() from the screen-open state Display hands it; the
// input layer cannot discover that for itself (see DsInput.h).
bool g_inMenu = false;
bool g_prevInMenu = false;

// Buttons held across a menu/gameplay boundary: ignored until physically
// released, so a confirm press is not re-delivered as a gameplay press.
std::uint32_t g_suppressed = 0;

// Last gameplay mask actually delivered, so keys/mouse move only on a change
// -- the same diffing WiiPadState::flushKeysAndButtons() does.
std::uint32_t g_prevGameplay = 0;

// Menu-context navigation edges (see updateGameplay): the D-pad and A/B
// pushed as keyboard arrow/return/escape codes while a screen is up.
std::uint32_t g_prevMenuNav = 0;

// Mouse button levels. Button 0 is driven by the touch tap OR GP_ATTACK, so
// they are tracked together rather than per source.
bool g_prevBtn0 = false;
bool g_prevBtn1 = false;

// The bottom touch panel: fixed 320x240 hardware on every 3DS model.
constexpr int kTouchPanelW = 320;

// circlePosition carries no named maximum in libctru (hid.h declares dx/dy
// as plain s16 with no range documentation), and the pad saturates around
// +/-156 on both axes, so normalise against that directly.
constexpr float kCirclePadMax = 156.0f;

char g_debugLine[96];

std::uint32_t mapTextButtons(u32 keys)
{
	std::uint32_t value = 0;
	if (keys & KEY_DUP)    value |= PLATFORM_TEXT_UP;
	if (keys & KEY_DDOWN)  value |= PLATFORM_TEXT_DOWN;
	if (keys & KEY_DLEFT)  value |= PLATFORM_TEXT_LEFT;
	if (keys & KEY_DRIGHT) value |= PLATFORM_TEXT_RIGHT;
	if (keys & KEY_A)      value |= PLATFORM_TEXT_TYPE;
	if (keys & KEY_B)      value |= PLATFORM_TEXT_BACK;
	if (keys & KEY_X)      value |= PLATFORM_TEXT_SPACE;
	if (keys & KEY_Y)      value |= PLATFORM_TEXT_CLOSE;
	if (keys & KEY_L)      value |= PLATFORM_TEXT_SPACE;
	if (keys & KEY_R)      value |= PLATFORM_TEXT_SHIFT;
	if (keys & KEY_SELECT) value |= PLATFORM_TEXT_SPACE;
	// START is context-split rather than absent: while a field has focus it
	// is ENTER -- the submit key VirtualKeyboard::tick pushes as KEY_RETURN
	// (so A can send chat after the system keyboard filled the field) --
	// and otherwise forwardStartToEscape() keeps its fixed KEY_ESCAPE role.
	if ((keys & KEY_START) && platformTextInputExclusive())
		value |= PLATFORM_TEXT_ENTER;
	// ZL/ZR wait for phase 2. Pure D-pad bits otherwise: the circle pad is
	// analog movement and reaches the game through the stick axes, not
	// through this mask; the D-pad's gameplay role is the hotbar wheel (see
	// updateGameplay()), and UP/DOWN have no gameplay action at all.
	return value;
}

// The physical buttons that carry a gameplay action (header table). Kept
// separate from mapTextButtons(): this mask is what inMenu gates.
std::uint32_t readGameplayButtons(u32 keys)
{
	std::uint32_t value = 0;
	if (keys & KEY_A)      value |= GP_JUMP;
	if (keys & KEY_B)      value |= GP_USE;
	if (keys & KEY_X)      value |= GP_ATTACK;
	if (keys & KEY_Y)      value |= GP_INVENTORY;
	// The shoulders are the left/right click the player asked for: L joins X
	// on button 0, R joins B on button 1 (see the header table).
	if (keys & KEY_L)      value |= GP_ATTACK;
	if (keys & KEY_R)      value |= GP_USE;
	if (keys & KEY_SELECT) value |= GP_SNEAK;
	if (keys & KEY_DUP)    value |= GP_DPAD_UP;
	if (keys & KEY_DDOWN)  value |= GP_DPAD_DOWN;
	if (keys & KEY_DLEFT)  value |= GP_DPAD_LEFT;
	if (keys & KEY_DRIGHT) value |= GP_DPAD_RIGHT;
	return value;
}

// Deliver the gameplay half of this frame's scan. `touchDown` comes in
// because button 0 has two sources and the finger is exempt from inMenu: it
// is the pointer, and it has to keep clicking screens.
//
// Ordering matters and is deliberate: the touch position is published before
// this runs, so every click below lands where the finger is.
void updateGameplay(u32 keys, bool touchDown)
{
	const std::uint32_t held = readGameplayButtons(keys);

	if (g_inMenu != g_prevInMenu)
	{
		// Anything down at the boundary keeps its old meaning until it is
		// physically released (header comment; WiiPadState's g_suppressedKeys
		// is the same rule).
		g_suppressed |= held;
		g_prevInMenu = g_inMenu;
		// Latched menu edges must not cross the boundary in either
		// direction -- Ps2InputMapper clears them on both transitions.
		g_latchedPressed = 0;
		// The navigation pushes below seed from here, so a button held at
		// the boundary does not fire a menu navigation step it was never
		// pressed for; it navigates on its next fresh press instead.
		g_prevMenuNav = held;
	}
	g_suppressed &= held; // forget buttons that have since been released

	// Gameplay does not use the text/menu latches. Drop them every frame so
	// an in-world press cannot be replayed as a menu press later (the same
	// per-frame clear Ps2InputMapper does after updateGameplay()).
	if (!g_inMenu)
		g_latchedPressed = 0;

	const std::uint32_t active = g_inMenu ? 0u : (held & ~g_suppressed);
	const std::uint32_t changed = active ^ g_prevGameplay;
	const std::uint32_t pressed = active & ~g_prevGameplay;

	// Keys are pushed on every edge, both down and up, so isKeyDown() and
	// KeyBinding::pressed stay truthful and nothing is left stuck down. The
	// D-pad is deliberately absent: it stopped carrying movement (the circle
	// pad owns that) and now feeds the hotbar wheel below in gameplay and
	// menu navigation when a screen is up.
	if (changed & GP_JUMP)      lwjgl::Keyboard::detail::pushKey(DS_KEY_A, (active & GP_JUMP) != 0);
	if (changed & GP_INVENTORY) lwjgl::Keyboard::detail::pushKey(DS_KEY_Y, (active & GP_INVENTORY) != 0);
	if (changed & GP_SNEAK)     lwjgl::Keyboard::detail::pushKey(DS_KEY_SELECT, (active & GP_SNEAK) != 0);

	// Menu navigation (the console's menu schema). The legacy screens'
	// keyTyped handlers listen for the keyboard arrow/return/escape codes,
	// which nothing else on this console emits -- the gameplay pushes above
	// are gated off in menus by design, so the D-pad and A never reached them
	// and the menu sat there deaf to the D-pad. In menu context those buttons
	// deliver the navigation codes directly, both edges like every push
	// above; in gameplay the same buttons keep their action meanings through
	// the key bindings and these codes are not pushed at all.
	//
	// Nothing goes out while a field has focus: the text channel
	// (VirtualKeyboard::tick) is the only reader meant to act on those
	// buttons, and the screen underneath would otherwise act too -- chat
	// submitted on the very first A press that way. g_prevMenuNav keeps
	// tracking the buttons anyway, so one held across focus arriving does
	// not fire a stale step when focus goes away; it navigates on its next
	// fresh press instead (same rule as g_suppressed at the menu boundary).
	// START's KEY_ESCAPE is decided in dsInputPoll(), and B here is the
	// screens' own back button.
	const bool typing = platformTextInputExclusive();
	const std::uint32_t navActive = g_inMenu
	    ? (held & (GP_DPAD_UP | GP_DPAD_DOWN | GP_DPAD_LEFT | GP_DPAD_RIGHT |
	               GP_JUMP | GP_USE))
	    : 0u;
	const std::uint32_t navChanged = navActive ^ g_prevMenuNav;
	if (!typing)
	{
		if (navChanged & GP_DPAD_UP)    lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_UP, (navActive & GP_DPAD_UP) != 0);
		if (navChanged & GP_DPAD_DOWN)  lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_DOWN, (navActive & GP_DPAD_DOWN) != 0);
		if (navChanged & GP_DPAD_LEFT)  lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_LEFT, (navActive & GP_DPAD_LEFT) != 0);
		if (navChanged & GP_DPAD_RIGHT) lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_RIGHT, (navActive & GP_DPAD_RIGHT) != 0);
		if (navChanged & GP_JUMP)       lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_RETURN, (navActive & GP_JUMP) != 0);
		if (navChanged & GP_USE)        lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_ESCAPE, (navActive & GP_USE) != 0);
	}
	g_prevMenuNav = navActive;

	const int x = g_state.pointerX;
	const int y = g_state.pointerY;

	// The wheel is an impulse rather than a state, so it fires on the press
	// only. Signs match Ps2InputMapper (R1 -> -1, L1 -> +1) and
	// InventoryPlayer::changeCurrentItem() subtracts its argument, so LEFT
	// steps the hotbar back and RIGHT steps it forward. The D-pad owns the
	// wheel in gameplay precisely because L/R clicked away to mouse buttons
	// (header table).
	if (pressed & GP_DPAD_LEFT)  lwjgl::Mouse::detail::pushWheel(1, x, y);
	if (pressed & GP_DPAD_RIGHT) lwjgl::Mouse::detail::pushWheel(-1, x, y);

	// Mouse buttons are a level. The touch tap ORs into button 0 so a finger
	// and X can be held in any order without one releasing the other.
	const bool want0 = touchDown || (active & GP_ATTACK) != 0;
	const bool want1 = (active & GP_USE) != 0;
	if (want0 != g_prevBtn0)
	{
		lwjgl::Mouse::detail::pushButton(0, want0, x, y);
		g_prevBtn0 = want0;
	}
	if (want1 != g_prevBtn1)
	{
		lwjgl::Mouse::detail::pushButton(1, want1, x, y);
		g_prevBtn1 = want1;
	}

	g_prevGameplay = active;
}

// START edges -> KEY_ESCAPE on the keyboard queue, both down and up so
// isKeyDown(KEY_ESCAPE) stays truthful. This split (escape on the key queue,
// nothing in the held mask) is the fixed pause/back role: the PS2's START
// carries ENTER as well as part of its fuller mapping, and this console's
// gains ENTER only while a field has focus (mapTextButtons), so the escape
// forwarding below never runs at the same time as the enter bit.
//
// The parameters are NOT called keysDown/keysUp: libctru's hid.h defines
// those as compatibility macros expanding to hidKeysDown/hidKeysUp, which
// would silently rewrite every use of the name.
void forwardStartToEscape(u32 keysPressed, u32 keysReleased)
{
	if (keysPressed & KEY_START)
		lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_ESCAPE, true);
	if (keysReleased & KEY_START)
		lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_ESCAPE, false);
}
} // namespace

void dsInputInit(int screenW, int screenH)
{
	// Idempotent by contract (Display::create may run again): reset the
	// whole snapshot so a re-entry doesn't replay stale edges or a dangling
	// touch position.
	if (screenW > 0)
		g_screenW = screenW;
	if (screenH > 0)
		g_screenH = screenH;
	g_initialized = true;
	g_state = DsInputState{};
	g_latchedPressed = 0;
	g_prevHeld = 0;
	g_prevTouchDown = false;
	g_prevTouchX = 0;
	g_prevTouchY = 0;
	g_prevTextExclusive = false;
	g_inMenu = false;
	g_prevInMenu = false;
	g_suppressed = 0;
	g_prevGameplay = 0;
	g_prevMenuNav = 0;
	g_prevBtn0 = false;
	g_prevBtn1 = false;
}

void dsInputPoll(bool inMenu)
{
	// The scan lives here so the poll is self-contained: HID state is latched
	// per scan, so every read below must come from the same one, and callers
	// (lwjgl::Display::processMessages) never have to remember to scan.
	hidScanInput();
	const u32 keys = hidKeysHeld();
	// Never name locals keysDown/keysUp: hid.h's compatibility macros
	// (#define keysDown hidKeysDown) would rewrite the tokens.
	const u32 keysPressed = hidKeysDown();
	const u32 keysReleased = hidKeysUp();

	// Which channel the gameplay buttons route to this frame. Read by
	// updateGameplay() below rather than threaded through every helper.
	g_inMenu = inMenu;

	// A text field owns the session's input while it has focus (set by
	// VirtualKeyboard::notifyFocus): it decides what the touch panel and
	// START mean this frame.
	const bool typing = platformTextInputExclusive();

	// Buttons -> PLATFORM_TEXT_* mask; rising edges accumulate until
	// dsInputConsumePressed() takes them.
	const std::uint32_t held = mapTextButtons(keys);
	g_latchedPressed |= held & ~g_prevHeld;
	g_prevHeld = held;
	g_state.held = held;

	// Touch -> absolute pointer in top-screen pixels, forwarded into the
	// mouse queue so the GUI is clickable from the bottom screen. Only the
	// MOTION lives here: the button 0 edge it implies is emitted by
	// updateGameplay(), which knows whether X is allowed to add a second
	// source to it.
	//
	// While typing none of that happens: the coordinates stay in the panel's
	// own 320x240 space for the on-screen keyboard's hit-testing (which
	// converts them against InputBackend_3DS's pointerWidth), and nothing
	// enters the mouse queue -- a click at panel coordinates would land
	// under the top screen's cursor instead of under the finger.
	const bool touchDown = (keys & KEY_TOUCH) != 0;
	if (touchDown)
	{
		touchPosition touch = {};
		hidTouchRead(&touch);
		// X: 320-wide panel -> top-screen width. Y: both screens are 240
		// tall, so it needs no scaling; clamp against the stored height
		// anyway so the pointer can never land outside the GUI regardless
		// of what geometry dsInputInit was handed.
		const int x = typing
		    ? static_cast<int>(touch.px)
		    : static_cast<int>(touch.px) * g_screenW / kTouchPanelW;
		const int y = typing
		    ? static_cast<int>(touch.py)
		    : std::min(static_cast<int>(touch.py), g_screenH - 1);

		g_state.pointerActive = true;
		g_state.pointerX = x;
		g_state.pointerY = y;

		// Publish the position before any click derived from it, and on new
		// contact publish it first so the click lands where the finger is.
		// A finger surviving the end of a typing session counts as a new
		// contact too: the previous sample was in the other coordinate
		// space, and differencing across that would fling the cursor once.
		if (!typing)
		{
			if (!g_prevTouchDown || g_prevTextExclusive)
				lwjgl::Mouse::detail::pushMotion(x, y, 0, 0);
			else if (x != g_prevTouchX || y != g_prevTouchY)
				lwjgl::Mouse::detail::pushMotion(x, y, x - g_prevTouchX, y - g_prevTouchY);
		}
	}
	else
	{
		// Release at the last contact point, not at (0,0): pointerX/Y keep
		// the final sample, which is where updateGameplay() emits the up.
		g_state.pointerActive = false;
	}
	g_prevTouchDown = touchDown;
	g_prevTouchX = g_state.pointerX;
	g_prevTouchY = g_state.pointerY;

	// Circle pad -> raw -1..1 stick axes (deadzones are downstream, per
	// DsInput.h). Raw dy grows downward; the game's stick Y is up-positive,
	// so negate it. Clamp because the s16 can exceed the nominal saturation.
	circlePosition circle = {};
	hidCircleRead(&circle);
	g_state.stickX = std::clamp(static_cast<float>(circle.dx) / kCirclePadMax, -1.0f, 1.0f);
	g_state.stickY = std::clamp(static_cast<float>(-circle.dy) / kCirclePadMax, -1.0f, 1.0f);

	// After the touch block, so clicks carry this frame's coordinates. The
	// finger stops being a click source while typing (see above): the tap
	// that focused the field already landed, and from there the panel
	// belongs to the keyboard.
	updateGameplay(keys, touchDown && !typing);

	// START keeps its fixed KEY_ESCAPE role only while nothing has focus:
	// pause in-world, "go back" in a screen, edge-driven off keysPressed
	// rather than the held mask so one held across the boundary does not
	// re-fire. With a field focused, mapTextButtons turned START into
	// ENTER instead (what submits chat), and forwarding its edges as ESC
	// here would close the very screen being typed in. An escape key whose
	// press went out before focus arrived is released on the transition,
	// otherwise it would stay stuck down for the rest of the session.
	if (typing && !g_prevTextExclusive)
		lwjgl::Keyboard::detail::pushKey(lwjgl::Keyboard::KEY_ESCAPE, false);
	g_prevTextExclusive = typing;
	if (!typing)
		forwardStartToEscape(keysPressed, keysReleased);
}

const DsInputState& dsInputState()
{
	return g_state;
}

std::uint32_t dsInputConsumePressed()
{
	const std::uint32_t pressed = g_latchedPressed;
	g_latchedPressed = 0;
	return pressed;
}

const char* dsInputDebugLine()
{
	// Idle = the input system hasn't been set up yet; DsInput.h lets this
	// report nullptr until dsInputInit() has run.
	if (!g_initialized)
		return nullptr;
	std::snprintf(g_debugLine, sizeof(g_debugLine),
	              "held=%03X t%c %d,%d cp%+.2f,%+.2f",
	              static_cast<unsigned>(g_state.held),
	              g_state.pointerActive ? '+' : '-',
	              g_state.pointerX, g_state.pointerY,
	              static_cast<double>(g_state.stickX),
	              static_cast<double>(g_state.stickY));
	return g_debugLine;
}

#endif // CTR_PLATFORM
