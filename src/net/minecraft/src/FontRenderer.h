#pragma once

#include <map>
#include <tuple>
#include <string>
#include <vector>

#include "java/Type.h"
#include "java/Random.h"
#include "platform/PlatformConfig.h"

class GameSettings;
class RenderEngine;
struct RenderCapturedMesh;

// net.minecraft.src.FontRenderer
class FontRenderer {
public:
	FontRenderer(GameSettings *gamesettings, const std::string &s, RenderEngine *renderengine);
	~FontRenderer();
	void refresh(RenderEngine *renderEngine);

	int_t drawStringWithShadow(const std::string &s, int_t i, int_t j, int_t k);
	void drawString(const std::string &s, int_t i, int_t j, int_t k);
	void drawStringScaled(const std::string &s, float_t x, float_t y, float_t scale, int_t color);
	int_t renderString(const std::string &s, int_t i, int_t j, int_t k, bool flag);
	void beginTextBatch();
	void endTextBatch();
	bool captureTextBatch(RenderCapturedMesh &out);
	bool drawCapturedText(const RenderCapturedMesh &mesh);
	unsigned int getTextCacheRevision() const { return textCacheRevision; }
	int_t getStringWidth(const std::string &s);
	std::string trimStringToWidth(const std::string &s, int_t width, bool reverse = false);
	static std::string stripFormattingCodes(const std::string &s);
	void drawSplitStringInternal(const std::string &s, int_t i, int_t j, int_t k, int_t l);
	int_t splitStringWidthInternal(const std::string &s, int_t i);
	void setUnicodeFlag(bool unicode);
	void setBidiFlag(bool bidi);
	bool getUnicodeFlag() const { return unicodeFlag; }
	bool getBidiFlag() const { return bidiFlag; }

	void drawSplitString(const std::string &s, int_t i, int_t j, int_t k, int_t l) { drawSplitStringInternal(s, i, j, k, l); }
	int_t splitStringWidth(const std::string &s, int_t i) { return splitStringWidthInternal(s, i); }

private:
	GameSettings *gameSettings;
	std::string fontTexturePath;

	float_t renderStringScaled(const std::string &s, float_t x, float_t y, int_t color, bool shadow, float_t scale);
	std::vector<std::string> split(const std::string &s, char delimiter);
	int_t getCharIndex(char_t c);
	void readCustomCharWidths(const std::string &textureFile, RenderEngine *renderEngine);

	float_t charWidth[256];

public:
	int_t fontTextureName;
	Random fontRandom;

private:
	struct DecorationRect
	{
		float_t x0;
		float_t y0;
		float_t x1;
		float_t y1;
		float_t r;
		float_t g;
		float_t b;
		float_t a;
	};

	void flushTextDecorations();

	int_t fontDisplayLists;
	unsigned int textCacheRevision;
	bool textBatchActive;
	bool unicodeFlag;
	bool bidiFlag;
	std::vector<int_t> buffer;
	std::vector<DecorationRect> pendingDecorations;

	void clearStringCache();
	static bool hasVolatileFormatCode(const std::vector<char_t> &units);

#if !PLATFORM_FONT_IMMEDIATE
	// ImmediatelyFast-style string cache for the display-list text path
	// (desktop only; the immediate backends batch whole HUD groups through
	// beginTextBatch and need no per-string lists). A string drawn more
	// than once with the same (text, color, shadow) replays one compiled
	// display list instead of walking its glyphs -- chat lines, GUI labels
	// and item counts are drawn every frame, and each replayed list turns
	// one glCallList into what used to be one per glyph.
	//
	// Strings carrying volatile format codes are never cached: obfuscated
	// (section-k) re-randomizes its glyph every frame, and strikethrough /
	// underline (section-m/n) queue screen-space decorations outside the
	// captured list. Everything else -- color codes, bold, italic, reset --
	// is captured verbatim, including the base-color restore of section-r,
	// which is why the base color is part of the key.
	//
	// Invalidated wholesale when the font texture reloads (textCacheRevision
	// bumps on every readFontTexture) and flushed when the table reaches
	// kStringCacheMaxEntries, so rotating chat cannot grow it unbounded.
	struct StringCacheEntry
	{
		int_t displayList = 0;
		float_t width = 0.0f;
	};

	std::map<std::tuple<std::string, int_t, bool>, StringCacheEntry> stringCache;
	unsigned int stringCacheRevision = 0;
#endif
};