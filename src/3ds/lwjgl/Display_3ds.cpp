// Display_3ds.cpp -- 3DS implementation of lwjgl::Display.
//
// Thin wrapper, like src/wii/lwjgl/Display_wii.cpp: there is no window to
// manage -- main_3ds.cpp owns gfx and the bottom-screen console -- so what
// lives here is the lwjgl-shaped surface plus the two things that are
// genuinely display-level on this console:
//
//   * "Close requested" has no window to close. It maps to aptMainLoop()
//     returning false (the system asked the app to quit: HOME-menu close,
//     another applet taking over, ...), which is latched once per frame so
//     isCloseRequested() stays true afterwards and the game's shutdown path
//     runs exactly once.
//
//   * The video mode is not ours to choose. The panel geometry is fixed
//     hardware -- 400x240 top (the game surface), 320x240 touch below (owned
//     by the console and DsInput) -- so setDisplayMode is advisory only,
//     same reasoning as the Wii's VIDEO_GetPreferredMode note.
//
//   * swapBuffers is the present half of the citro3d frame lifecycle: it
//     closes the frame if the game's renderSubmitFrame() did not already,
//     and only paces the loop itself when no citro3d frame was submitted
//     this iteration (see DsRender.cpp for why stacking the waits would
//     halve the frame rate).
#ifdef CTR_PLATFORM

#include "lwjgl/Display.h"

#include <3ds.h>

#include <cstddef>

#include "3ds/DsSwkbd.h"
#include "3ds/input/DsCirclePadPro.h"
#include "3ds/input/DsInput.h"
#include "3ds/render/DsRender.h"

#include "client/Minecraft.h"
#include "net/minecraft/src/GuiScreen.h"

namespace
{
// Fixed top-screen geometry: every 3DS model is 400x240, so this is a
// hardware constant rather than a negotiated mode.
constexpr int kScreenWidth = 400;
constexpr int kScreenHeight = 240;

bool g_created = false;

// Latched by processMessages() when aptMainLoop() says quit. Display::
// processMessages() is void (Display.h), so the quit request cannot be
// returned -- it is recorded here and read back through isCloseRequested().
bool g_closeRequested = false;

// APT applet-lifecycle hook: while an applet owns the foreground the game
// thread is parked inside aptMainLoop(), but the app's other threads keep
// running on core 1 at the share main_3ds.cpp's APT_SetAppCpuTimeLimit(80)
// granted -- and on Old 3DS the HOME menu applet runs on that same core
// with only the remainder, which is why the menu felt so laggy while a
// chunk-generation job was in flight. Suspending the share hands the whole
// core to the applet for the duration of the menu; restoring on the way
// back puts the workers beside the game thread again. swkbd is exempt:
// its dialog keeps the network session alive through exactly the threads
// the share feeds (see DsSwkbd.h), so throttling it mid-chat would kick
// the player off servers.
aptHookCookie g_aptHookCookie;
u32 g_suspendRestoreLimit = 0;

void aptStateHook(APT_HookType hook, void *)
{
	// Circle Pad Pro worker vs the system's power state: the ir:USER session
	// it drives must never ride a sleep transition. The 2026-10 Old 2DS
	// Luma3DS dump (crash_dump_00000000.dmp, parsed with
	// LumaTeam/luma3ds_exception_dump_parser) is a data abort INSIDE the ir
	// system module (process "ir", title 0004013000003302, core 1): on the
	// lid close it wrote byte 1 to 0x1000200E -- a hardware register page
	// that module does not even have mapped (second-level Translation fault,
	// a write) -- so the console crashed system-side, not in the game. The
	// game's only tie to that module is the CPP worker's IRNOP session. The
	// endless initialize/connect/teardown cycle that worker used to run on
	// CPP-less Old hardware -- a state no retail title holds -- is gone
	// (bounded probe, then parked; see DsCirclePadPro.cpp); this teardown
	// is the other half of the same posture: whatever the worker's state,
	// its session must not survive into the freeze, and ONWAKEUP/ONRESTORE
	// re-arm it into a fresh probe afterwards.
	//
	// ONSLEEP is the in-game lid close: libctru runs it from aptMainLoop()
	// BEFORE APT_ReplySleepNotificationComplete(), so the teardown lands
	// while this process is still scheduled -- guaranteed before the system
	// commits to sleeping. ONSUSPEND covers the HOME-menu park: the park
	// keeps this process's other threads running, and libctru answers
	// SLEEP_ENTER itself for an inactive app (no ONSLEEP ever fires from
	// there), so the session must already be down when the park begins.
	// ONWAKEUP and ONRESTORE (the HOME return arrives as APTCMD_WAKEUP_PAUSE)
	// bring it back; a library-applet return deliberately never fires
	// ONRESTORE (APTCMD_WAKEUP), which the dsInputPoll re-arm covers anyway.
	if (hook == APTHOOK_ONSLEEP || (hook == APTHOOK_ONSUSPEND && !dsSwkbdActive()))
		DsCirclePadPro::shutdown(); // idempotent; New 3DS never runs the worker
	else if (hook == APTHOOK_ONWAKEUP || hook == APTHOOK_ONRESTORE)
		DsCirclePadPro::init();      // likewise idempotent; New 3DS early-outs

	// swkbd skips both blocks above on purpose: aptLaunchLibraryApplet fires
	// ONSUSPEND from DsSwkbd's helper thread, and shutdown()/init() are
	// main-thread-only calls -- the dialog's own teardown lives in
	// processMessages(), which runs on this thread.

	if (g_suspendRestoreLimit == 0)
		return; // no core-1 share was granted at boot; nothing to move
	if (hook == APTHOOK_ONSUSPEND)
	{
		if (dsSwkbdActive())
			return;
		APT_SetAppCpuTimeLimit(0);
	}
	else if (hook == APTHOOK_ONRESTORE)
	{
		APT_SetAppCpuTimeLimit(g_suspendRestoreLimit);
	}
}
} // namespace

namespace lwjgl
{
namespace Display
{

void create()
{
	if (g_created) return;

	// Video and the console are already up (main_3ds.cpp ran before
	// Minecraft::start); what create() owns is the render context and telling
	// the input layer which surface the touch coordinates map onto -- see
	// DsInput.h, the touch panel is mapped into these top-screen pixels.
	//
	// This is the citro3d bring-up point, mirroring the Wii exactly: there
	// Display_wii.cpp::create() is where wiigl_init() runs, because the
	// renderer comes up with the display, not from the entry point --
	// pc/Main.cpp is the only port that calls GLContext::instantiate(). A
	// failed init is logged inside ds::init(); every later ds:: call then
	// no-ops, so the frame loop keeps presenting VBlanks over a black panel
	// with the reason in the log rather than hanging.
	ds::init();
	dsInputInit(kScreenWidth, kScreenHeight);

	// Give the applet core back to the system while an applet (the HOME
	// menu) owns the foreground, and take the boot-granted share back on
	// resume -- see aptStateHook() above. The boot value is read, not
	// assumed, so a refused APT_SetAppCpuTimeLimit keeps this a no-op.
	u32 bootCpuLimit = 0;
	if (R_SUCCEEDED(APT_GetAppCpuTimeLimit(&bootCpuLimit)))
		g_suspendRestoreLimit = bootCpuLimit;
	aptHook(&g_aptHookCookie, aptStateHook, nullptr);

	g_created = true;
}

void setDisplayMode(const DisplayMode &)
{
	// The console decides. See the header comment.
}

DisplayMode getDisplayMode()
{
	return DisplayMode(kScreenWidth, kScreenHeight);
}

void setTitle(const jstring &) {}
void setFullscreen(bool)       {}

bool isCloseRequested() { return g_closeRequested; }
bool isVisible()        { return true; }
bool isActive()         { return true; }
bool isMinimized()      { return false; }

void processMessages()
{
	// While the swkbd applet is up (see 3ds/DsSwkbd.h) the applet owns the
	// foreground: it holds the APT session from its own helper thread, both
	// LCDs, and the HID state the player's typing goes through. The 3DS does
	// NOT suspend this process for a library applet -- the rest of the frame
	// loop keeps running below -- so what must stand down is exactly these
	// two foreground services:
	//
	//   * aptMainLoop() would race the applet's APT calls on the one APT
	//     session (and the applet, not this app, is what the OS is talking
	//     to anyway). Any exit/sleep order the OS wanted to deliver queues
	//     until the applet closes and this call resumes.
	//   * dsInputPoll() must not sample the buttons the player is typing
	//     into the applet with -- those presses belong to the dialog, and
	//     freezing the poll keeps them out of the game's input snapshot and
	//     the DsInput edge latches entirely.
	//
	// The renderer learned the same fact from here (ds::setAppletForeground),
	// so frames stop being submitted as well; the world tick, the network
	// threads and everything else keep running, which is what keeps a server
	// session alive while the player types a chat line or a login command.
	const bool appletActive = dsSwkbdActive();
	ds::setAppletForeground(appletActive);
	if (appletActive)
	{
		// The swkbd dialog is the third lid-close window: a library applet
		// never suspends this process (the frame loop keeps running -- the
		// "don't pause the game while typing" behaviour above), and libctru
		// answers sleep for an inactive app without ever firing the
		// ONSLEEP teardown in aptStateHook(), so the system can freeze us
		// mid-dialog with the CPP worker's IRNOP session live -- the same
		// ir-module fault that teardown exists for. The worker is useless
		// while the dialog owns the foreground anyway (dsInputPoll below is
		// skipped for the whole dialog, so no sample is ever read), so
		// stand it down here on the main thread. Idempotent: only the first
		// dialog frame pays the join, and the dsInputPoll re-arm brings the
		// worker back within a couple of seconds of the dialog closing --
		// the CPP worker's clip-on-any-time reconnect absorbs the rest.
		DsCirclePadPro::shutdown();
		return;
	}

	// libctru's per-frame pump: it handles sleep mode and HOME-menu jumps and
	// reports whether the app should keep running. The false case is the
	// 3DS's "close requested", latched for isCloseRequested() above.
	if (!aptMainLoop())
		g_closeRequested = true;

	// Same reason as the Wii/PS2 versions of this function: the shared GUI
	// never re-captures gameplay focus itself, so after the first pause menu
	// closes, inGameHasFocus would stay false and the world would keep no
	// input. Ask the game directly -- and only when there is a world to focus
	// into, because setIngameFocus() reads "no screen and no world" as "show
	// the title" and would bounce back to the title screen mid-load.
	Minecraft *mc = Minecraft::getMinecraft();
	const bool inMenu = (mc != nullptr && mc->currentScreen != nullptr);
	if (!inMenu && mc != nullptr && mc->theWorld != nullptr && !mc->inGameHasFocus)
		mc->setIngameFocus();

	// aptMainLoop + focus re-capture + the whole DsInput scan (touch, circle
	// pad and both button channels); DsInput scans HID itself so every reader
	// of this frame's state sees one consistent sample. inMenu goes with it
	// because the input layer cannot work out whether a screen is open --
	// WiiPadState::wiiPadPoll() and Ps2Input::update() take it for the same
	// reason.
	dsInputPoll(inMenu);
}

void swapBuffers()
{
	// The present half of the citro3d frame lifecycle. Normally the game has
	// already closed the frame at renderSubmitFrame() -- the last point in a
	// tick at which anything draws -- so ds::present() finds nothing open
	// and only reports that a frame was submitted; the pacing for that frame
	// comes from the next C3D_FrameBegin(C3D_FRAME_SYNCDRAW).
	//
	// The VBlank wait below is only for iterations that submitted nothing at
	// all: without it the long stretches of startGame() with no
	// LoadingScreenRenderer frame would free-run, and Azahar reports that as
	// "App: 0 FPS" because no frame is ever allowed to settle. Waiting there
	// as well would double the sync points of a submitted frame and halve the
	// frame rate during normal play.
	if (!ds::present())
		gspWaitForVBlank();
}

void update(bool doProcessMessages)
{
	// The present half closes whatever frame the game left open; the message
	// half is the pump: Minecraft's frame loop calls update() and never
	// processMessages() directly in-game, so this is where aptMainLoop() and
	// the input snapshot run once per frame.
	swapBuffers();
	if (doProcessMessages)
		processMessages();
}

int_t getX() { return 0; }
int_t getY() { return 0; }
int_t getWidth()  { return kScreenWidth; }
int_t getHeight() { return kScreenHeight; }

} // namespace Display
} // namespace lwjgl

#endif // CTR_PLATFORM
