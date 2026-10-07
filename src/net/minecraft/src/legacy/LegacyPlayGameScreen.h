#pragma once

#include "net/minecraft/src/GuiSelectWorld.h"
#include "LegacyOptionsLayout.h"
#include "LegacyOptionsPanel.h"

class LegacyPlayGameScreen : public GuiSelectWorld
{
public:
    explicit LegacyPlayGameScreen(GuiScreen *parent);

    void initGui() override;
    void updateScreen() override;
    void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;
    void handleMouseInput() override;

protected:
    bool usesSpecializedMenuNavigation() const override { return true; }
    void actionPerformed(GuiButton *button) override;
    void keyTyped(char_t c, int_t key) override;

private:
    void rebuildButtons();
    // Activating a world row opens its action rows (Play/Rename/Delete/Back)
    // in place of the list, the way the Legacy console editions did; this is
    // where the rename/delete options the common select-world menu exposes
    // live in the Legacy UI.
    void openWorldActions(int_t saveIndex);
    void closeWorldActions();
    // The delete-confirmation callback: a confirmed delete reloads the save
    // list, and a still-open action panel would land on whichever world
    // shifts into the deleted one's index -- showing that world's rows right
    // after deleting another. Close the panel on confirm only; a cancel
    // returns to the panel Delete was pressed in.
    void deleteWorld(bool confirmed, int_t index) override;
    void drawLegacyScene(float_t partialTick);
    void drawEntryIcons();
    void drawScrollIndicators();
    void drawMenuControlHints();
    void syncSelectedButton();
    void activateSelection();
    void selectControl(int_t index);
    void moveSelection(int_t direction);
    int_t maxVisibleWorlds() const;
    int_t maxPage() const;

    LegacyOptionsLayout layout;
    LegacyOptionsPanel panelRenderer;
    int_t page;
    int_t firstWorldIndex;
    int_t visibleWorldCount;
    int_t selectedControlIndex;
    int_t hoveredControlIndex;
    // World-action mode: while set, the panel shows the action rows for
    // saveList[actionWorldIndex] instead of the world list.
    int_t actionWorldIndex;
    bool inWorldActions;
    int_t tutorialMessageTicks;
    std::string tutorialMessage;
    int_t lastMouseX;
    int_t lastMouseY;
    bool panoramaAvailable;
};
