#pragma once

// GuiQrDownload.h -- the "Descarga QR" screen (main menu, 3DS only).
//
// Flow: the back camera scans a QR code containing a http(s):// URL of a
// skin PNG, a texture pack zip or a .ochpack mod -> the player confirms ->
// the file downloads over httpc with a progress bar (B cancels) -> the
// download is converted to this console's asset orientation (a Java-
// orientation skin or pack would render vertically mirrored here -- see
// src/3ds/assets/DsAssetConvert.h, the runtime counterpart of
// scripts/texturepack_flip_3ds.py) -> it is installed through the same
// entry points the device/USB loaders use:
//
//   skin            SkinManager::installCustomSkin   (also selects it)
//   texture pack    copied into .minecraft/texturepacks, list refreshed
//   mod (.ochpack)  ModManager::installModPack
//
// The camera is a console-wide resource: it is released in onGuiClosed so
// every exit path (B, HOME, a screen swap) gives it back.

#include "3ds/qr/DsHttpDownload.h"

#include <string>

#include "GuiScreen.h"

class GuiQrDownload final : public GuiScreen
{
public:
	explicit GuiQrDownload(GuiScreen *parent);
	~GuiQrDownload() override;

	void initGui() override;
	void updateScreen() override;
	void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override;
	void keyTyped(char_t c, int_t key) override;
	void actionPerformed(GuiButton *button) override;
	void onGuiClosed() override;

private:
	enum class State
	{
		Scanning,
		Confirm,
		Downloading,
		Installing,
		Done,
		Failed,
	};

	enum class Kind
	{
		Unknown,
		Skin,
		TexturePack,
		Mod,
	};

	void rebuildButtons();
	void closeAndReturn();
	void beginConfirm(const std::string &url);
	void startDownload();
	void runInstall();
	Kind classifyDownload();
	void drawPreview(int_t x, int_t y, int_t w, int_t h);
	void drawProgressBar(int_t x, int_t y, int_t w, int_t h);
	static std::string kindLabel(Kind kind);
	static std::string nameFromUrl(const std::string &url);

	GuiScreen *parentScreen;
	State state = State::Scanning;
	std::string scannedUrl;
	Kind scannedKind = Kind::Unknown;
	Kind installKind = Kind::Unknown;
	std::string downloadPath;
	std::string installName;
	DsHttpDownload download;
	std::string message;
	std::string detail;
	bool messageIsError = false;
	int previewTexture = -1;
	bool installing = false;
};
