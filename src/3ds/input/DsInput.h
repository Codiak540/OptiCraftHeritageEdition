#pragma once

#include <cstdint>

#include "platform/Input.h" // PLATFORM_TEXT_* action masks

// Touchscreen + circle-pad input for the 3DS port (src/3ds/input/DsInput.cpp).
//
// One snapshot per frame -- what platform/InputBackend_3DS.cpp feeds into the
// shared Input API -- plus the key/mouse/wheel edges the engine plays with.
// Every physical button has up to two channels, because an open screen and a
// closed one give it different meanings; DsInput.cpp's header carries the
// full button -> channel table.
//
// Coordinates: the game surface is the top screen (400x240), the touch panel
// the bottom screen (320x240). Touch positions are mapped to top-screen
// pixels inside DsInput (x * 400/320, y unchanged), so callers can compare
// them against the 400x240 GUI directly. The exception is text entry: while
// a field has focus the coordinates stay in the panel's own 320x240 space
// and no mouse events are forwarded at all, because the on-screen keyboard
// (or the system keyboard it may open instead) reads the panel itself -- see
// platformTextInputExclusive() in the .cpp.

struct DsInputState
{
    bool connected = true;   // the console itself is always "attached"
    std::uint32_t held = 0;  // PLATFORM_TEXT_* actions held right now

    bool pointerActive = false; // finger down during this poll
    int pointerX = 0;           // top-screen pixels (raw panel pixels while a
    int pointerY = 0;           // field has focus); valid while pointerActive

    bool stickConnected = true; // circle pad present (always true on 3DS)
    float stickX = 0.0f;        // -1..1, raw (deadzone handling is downstream)
    float stickY = 0.0f;

    // Right stick (New 3DS C-Stick, or a Circle Pad Pro clipped onto an Old
    // 3DS/XL -- DsInput.cpp folds the DsCirclePadPro ir:USER worker's
    // sample in on Old hardware, while libctru's stock hidShouldUseIrrst()
    // gate keeps the New model's internal nub reporting through ir:rst),
    // same raw -1..1 axis contract as the circle pad fields (Y
    // down-positive, nub up reads negative).
    // With no right stick attached (nothing linked, nothing present) these
    // stay 0 -- the gameplay look channel is inert rather than absent,
    // and the shared snapshot's right-stick fields read a centred stick.
    float cstickX = 0.0f;
    float cstickY = 0.0f;
};

// One-time setup: remember the top-screen size used for the touch mapping.
// Called from lwjgl::Display::create().
void dsInputInit(int screenW, int screenH);

// hidScanInput() + refresh the snapshot; called once per frame from
// lwjgl::Display::processMessages(). Also forwards touch -> mouse, START ->
// KEY_ESCAPE (or ENTER while a field has focus), and the gameplay channel
// (jump/inventory/sneak keys; attack from X or R and use from B or L as
// mouse buttons; hotbar wheel from D-pad LEFT/RIGHT, ZL/ZR (New 3DS or
// Circle Pad Pro), and the right stick as a look pad there; chat from D-pad
// UP). While a field
// has focus the menu navigation and the mouse forwarding stand down -- see
// DsInput.cpp.
//
// inMenu is "a GuiScreen is currently open", which the input layer cannot
// work out for itself -- the same reason WiiPadState::wiiPadPoll() and
// Ps2Input::update() take it as a parameter (see the header comment in
// DsInput.cpp for what it gates).
void dsInputPoll(bool inMenu);

// Face-button camera toggle (OptiCraft Options): while on, gameplay gives
// the A/B/X/Y diamond to the camera (Y/A/X/B = look left/right/up/down),
// jump moves to SELECT-tap or double-tap-B, sneak to holding SELECT, and
// inventory to double-tap-Y; attack/use stay on the shoulders. Called from
// GameSettings whenever the option is (re)loaded or changed.
void dsInputSetFaceButtonCamera(bool enabled);

// Pocket-Edition touch gestures toggle (OptiCraft Options, "Touch Click"):
// while on, a short stationary touch on the camera pad taps (place/swing,
// routed by the crosshair target) and a hold of 180 ms or more breaks/uses.
// While off the pad is the plain camera drag it was before those gestures
// existed -- the widgets (hotbar, inventory/crafting/pause) and the menus
// are unaffected either way. Called from GameSettings whenever the option
// is (re)loaded or changed.
void dsInputSetPocketTouch(bool enabled);

// Pad-code claim mask (DsPadKeyCodes.h contract): one bit per pad key code,
// bit n = DS_KEY_A + n, set for every KeyBinding whose keyCode names a pad
// button. A claimed code re-purposes its button: DsInput stops feeding the
// button's hardcoded click channel (L/R/X place/attack) and emits the code
// instead, so what the Controls screen bound is ALL the button does. Codes
// nobody claims change nothing -- the default layout ships with L/R/X
// unclaimed, keeping place on L and attack on R/X exactly as before. B is
// never claimable (the capture channel keeps it as the cancel button), so
// its back role is fixed on every menu.
// Computed by platformGameSettingsSyncControllerBindings() and pushed here
// whenever bindings load, reset or change.
void dsInputSetBoundPadCodes(std::uint32_t codes);

// The touch-HUD action widgets' codes: keyBindJump's and keyBindInventory's
// current codes, pushed by the same GameSettings sync. The on-screen Jump
// and Inventory buttons stand for the ACTIONS, so they must fire whatever
// the binding currently names -- not the physical A/Y codes, which a rebind
// can move away from under them. A code outside the pad range parks the
// matching widget dead (nothing to push) instead of firing a stale binding.
void dsInputSetTouchHudActionCodes(int jumpKeyCode, int inventoryKeyCode);

const DsInputState& dsInputState();

// PLATFORM_TEXT_* actions that went down since the last call: returns the
// accumulated edge mask and clears it (consume-on-read, same contract as the
// Wii's wiiTextInputConsumePressed()).
std::uint32_t dsInputConsumePressed();

// One-line human-readable state for the debug overlay, or nullptr when idle.
const char* dsInputDebugLine();
