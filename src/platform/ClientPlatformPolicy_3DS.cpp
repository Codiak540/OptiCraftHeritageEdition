#include "platform/ClientPlatformPolicy.h"

#include "net/minecraft/src/GameResources.h"
#include "pc/CrashHandler.h"

#include <citro3d.h>

namespace ClientPlatformPolicy
{
int initialWidth()
{
    // Phase-1 constant: the 3DS top screen is 400x240. lwjgl::Display does
    // declare getWidth()/getHeight(), but nothing owns them until the
    // citro3d renderer creates the framebuffer, so reading them pre-init
    // would report whatever a stub backend left behind -- the constant also
    // compiles without pulling Display into this target's link. Once the
    // renderer lands, lwjgl::Display owns this value the way the Wii's
    // wiigl_width() owns it there.
    return 400;
}

int initialHeight()
{
    // Top screen of the same 400x240 pair; see initialWidth() for why this
    // is a constant rather than a Display query.
    return 240;
}

std::string minecraftDirectory()
{
    return GameResources::getExeDir() + "/.minecraft";
}

bool saveConverterUsesSavesSubdirectory()
{
    return true;
}

void applyGameSettingsDefaults(GameSettings*)
{
}

void preloadStartupTextures(RenderEngine*)
{
}

void releaseWorldEntryAssets(RenderEngine*)
{
}

void shutdownFlush()
{
    // The last frame is submitted asynchronously (renderSubmitFrame ->
    // C3D_FrameEnd(0)) and only drains at the next frame's
    // C3D_FrameBegin(C3D_FRAME_SYNCDRAW); on shutdown there is no next frame.
    // SYNCDRAW performs a C3D_FrameSync before anything else, so this dummy
    // begin/end pair waits for the queue to finish -- after it, no GPU work
    // references game textures, and the teardown below can free them safely.
    // Same rationale (and same fix shape) as the CrashHandler_3ds copy; see
    // also shutdownServices() in main_3ds.cpp, which never runs on this path
    // because PLATFORM_EXIT_PROCESS_ON_SHUTDOWN has shutdownMinecraftApplet()
    // call exit(0) instead of returning to main().
    if (C3D_FrameBegin(C3D_FRAME_SYNCDRAW))
    {
        C3D_FrameEnd(0);
    }
}

int panoramaSampleGrid()
{
    // The PS2's 2x2 accumulation (24 draws total): every sample is its own
    // one-quad Tessellator::draw(), and on this backend each draw is a
    // staging-arena submit -- the desktop/Wii 8x8 grid bills the menu 384
    // submits per frame for a blur the 400x240 target cannot resolve anyway.
    return 2;
}

void reportCrash(const std::string& description)
{
    CrashHandler::Crash(description);
}
}
