#pragma once

#include <string>

class GameSettings;
class RenderEngine;

namespace ClientPlatformPolicy
{
    int initialWidth();
    int initialHeight();
    std::string minecraftDirectory();
    bool saveConverterUsesSavesSubdirectory();
    void applyGameSettingsDefaults(GameSettings* settings);
    void preloadStartupTextures(RenderEngine* renderEngine);
    void releaseWorldEntryAssets(RenderEngine* renderEngine);
    // Called once from Minecraft::shutdownMinecraftApplet() before any
    // teardown touches GPU-owned memory. Only the 3DS needs it: the game
    // exits through exit(0) there (PLATFORM_EXIT_PROCESS_ON_SHUTDOWN), so
    // nothing returns to the entry point, and the last submitted citro3d
    // frame must be drained before GLAllocation::deleteTexturesAndDisplayLists()
    // frees C3D_Tex storage the in-flight frame may still be sampling.
    void shutdownFlush();
    int panoramaSampleGrid();
    void reportCrash(const std::string& description);
}
