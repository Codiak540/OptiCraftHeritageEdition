#include "net/minecraft/src/UiStrings.h"
#include "LegacyMainMenu.h"

#include "platform/PlatformConfig.h"
#include "LegacyGuiButton.h"
#include "LegacyMainMenuLayout.h"
#include "net/minecraft/src/GuiButton.h"
#include "net/minecraft/src/StringTranslate.h"

void legacyCreateMainMenuButtons(std::vector<GuiButton *> &controlList, GuiButton *&multiplayerButton,
    int_t screenWidth, int_t screenHeight, bool hideQuitButton)
{
    const int_t buttonCount = legacyMainMenuButtonCount(hideQuitButton);
    const LegacyMainMenuLayout layout = legacyMainMenuLayout(screenWidth, screenHeight, buttonCount);
    const int_t stride = layout.buttonHeight + layout.buttonSpacing;
    int_t row = 0;

    auto addButton = [&](int_t id, const std::string &label) -> GuiButton *
    {
        GuiButton *button = new LegacyGuiButton(id, layout.buttonX, layout.firstButtonY + row * stride,
            layout.buttonWidth, layout.buttonHeight, label);
        controlList.push_back(button);
        ++row;
        return button;
    };

    StringTranslate *tr = StringTranslate::getInstance();
    addButton(1, uiText("Play Game"));
    multiplayerButton = addButton(2, tr->translateKey("menu.multiplayer"));
    addButton(3, uiText("Mods"));
    addButton(6, "Skins");
    addButton(0, uiText("Help & Options"));
    addButton(5, uiText("Language"));
    if (!hideQuitButton)
        addButton(4, tr->translateKey("menu.quit"));

#if PLATFORM_3DS
    // The dual-screen main menu gives every entry an icon drawn from the
    // game's own assets, Legacy-Console list style: blocks from
    // /terrain.png (16x16 tiles of the 256x256 beta atlas), the default
    // player's face from /mob/char.png, and the vanilla language glyph
    // from /gui/gui.png. Every other platform sets no icon and keeps the
    // shared centred label.
    for (GuiButton *button : controlList)
    {
        auto *legacy = static_cast<LegacyGuiButton *>(button);
        switch (button->id)
        {
        case 1: legacy->setMenuIcon("/terrain.png", 3.0f / 16.0f, 0.0f, 4.0f / 16.0f, 1.0f / 16.0f); break;            // grass side
        case 2: legacy->setMenuIcon("/terrain.png", 3.0f / 16.0f, 3.0f / 16.0f, 4.0f / 16.0f, 4.0f / 16.0f); break;    // redstone ore
        case 3: legacy->setMenuIcon("/terrain.png", 8.0f / 16.0f, 0.0f, 9.0f / 16.0f, 1.0f / 16.0f); break;            // tnt
        case 6: legacy->setMenuIcon("/mob/char.png", 8.0f / 64.0f, 8.0f / 32.0f, 16.0f / 64.0f, 16.0f / 32.0f); break; // player face
        case 0: legacy->setMenuIcon("/terrain.png", 3.0f / 16.0f, 2.0f / 16.0f, 4.0f / 16.0f, 3.0f / 16.0f); break;    // bookshelf
        case 5: legacy->setMenuIcon("/gui/gui.png", 0.0f, 106.0f / 256.0f, 20.0f / 256.0f, 126.0f / 256.0f); break;    // language glyph
        case 4: legacy->setMenuIcon("/terrain.png", 1.0f / 16.0f, 1.0f / 16.0f, 2.0f / 16.0f, 2.0f / 16.0f); break;    // bedrock
        default: break;
        }
    }
#endif
}
