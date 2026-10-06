#include "mods/autologin/AutoLoginMod.h"

#include "Minecraft.h"
#include "net/minecraft/src/EntityClientPlayerMP.h"
#include "net/minecraft/src/GuiButton.h"
#include "net/minecraft/src/GuiScreen.h"
#include "net/minecraft/src/GuiTextField.h"
#include "pc/lwjgl/Keyboard.h"
#include "platform/Log.h"
#include "platform/PlatformCompat.h"
#include "platform/Storage.h"
#include "mods/ModManager.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace
{

std::string toLower(const std::string &str)
{
	std::string lower = str;
	std::transform(lower.begin(), lower.end(), lower.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return lower;
}

// <game mods dir>/autologin/config.txt
std::string autoLoginConfigDir()
{
	return PlatformStorage::join(ModManager::getInstance().getGameModsDir(), "autologin");
}

std::string autoLoginConfigPath()
{
	return PlatformStorage::join(autoLoginConfigDir(), "config.txt");
}

} // namespace

// Settings screen for the mod. Modeled on GuiScreenAddServer so every
// platform's text entry works through the standard GuiTextField focus
// notification (PC keyboard; 3DS swkbd + bottom-screen panel; PS2/Wii
// on-screen keyboard -- see VirtualKeyboard).
class GuiAutoLoginSettings : public GuiScreen
{
public:
	GuiAutoLoginSettings(GuiScreen *parent, AutoLoginMod *mod)
		: m_parent(parent), m_mod(mod)
	{
	}

	~GuiAutoLoginSettings() override
	{
		delete m_passwordField;
	}

	void initGui() override
	{
#if !defined(PS2_PLATFORM) && !defined(WII_PLATFORM)
		lwjgl::Keyboard::enableRepeatEvents(true);
#endif
		// Keep text typed before a re-init (window resize on desktop).
		std::string current = m_mod->getPassword();
		if (m_passwordField != nullptr)
			current = m_passwordField->getText();

		// clearControlList deletes the old buttons as it drops them -- a raw
		// controlList.clear() would orphan them for good, since only the
		// destructor gets another chance and it only walks what is still in
		// the list (the port's re-init contract; see every legacy screen).
		clearControlList();
		delete m_passwordField;
		m_passwordField = new GuiTextField(this, fontRenderer, width / 2 - 100, height / 4 + 40, 200, 20, current);
		m_passwordField->setMaxStringLength(64);
		m_passwordField->setFocused(true);

		controlList.push_back(new GuiButton(0, width / 2 - 100, height / 4 + 96, 200, 20, "Done"));
	}

	void updateScreen() override
	{
		GuiScreen::updateScreen();
		if (m_passwordField != nullptr)
			m_passwordField->updateCursorCounter();
	}

	void onGuiClosed() override
	{
#if !defined(PS2_PLATFORM) && !defined(WII_PLATFORM)
		lwjgl::Keyboard::enableRepeatEvents(false);
#endif
		if (m_passwordField != nullptr)
			m_passwordField->setFocused(false);
		if (!m_closing)
		{
			// Backing out (Esc / console Back) still keeps what was typed.
			m_closing = true;
			m_mod->setPassword(m_passwordField != nullptr ? m_passwordField->getText() : m_mod->getPassword());
		}
	}

	void actionPerformed(GuiButton *button) override
	{
		if (button == nullptr || !button->enabled)
			return;
		if (button->id == 0) // Done
			close();
	}

	void keyTyped(char_t c, int_t key) override
	{
#if defined(PS2_PLATFORM) || defined(WII_PLATFORM) || defined(CTR_PLATFORM)
		// The software keyboards submit with Enter; the field keeps focus the
		// whole time, so treat that as confirming the password.
		if (c == '\r' || key == lwjgl::Keyboard::KEY_RETURN)
		{
			if (m_passwordField != nullptr && m_passwordField->getFocused())
			{
				close();
				return;
			}
		}
#endif
		if (key == lwjgl::Keyboard::KEY_ESCAPE)
		{
			close();
			return;
		}
		if (m_passwordField != nullptr)
			m_passwordField->textboxKeyTyped(c, key);
#if !defined(PS2_PLATFORM) && !defined(WII_PLATFORM) && !defined(CTR_PLATFORM)
		if (c == '\r' || key == lwjgl::Keyboard::KEY_RETURN)
			close();
#endif
	}

	void mouseClicked(int_t x, int_t y, int_t button) override
	{
		GuiScreen::mouseClicked(x, y, button);
		if (m_passwordField != nullptr)
			m_passwordField->mouseClicked(x, y, button);
	}

	void drawScreen(int_t mouseX, int_t mouseY, float_t partialTick) override
	{
		drawDefaultBackground();
		drawCenteredString(fontRenderer, "Auto-Login Settings", width / 2, height / 4 - 40, 0xFFFFFF);
		drawString(fontRenderer, "Server password:", width / 2 - 100, height / 4 + 28, 0xA0A0A0);
		if (m_passwordField != nullptr)
			m_passwordField->drawTextBox();
		drawCenteredString(fontRenderer, std::string("\xc2\xa7") + "7Sent as /login and /register when the server asks",
		                   width / 2, height / 4 + 70, 0x888888);
		GuiScreen::drawScreen(mouseX, mouseY, partialTick);
	}

private:
	void close()
	{
		if (m_closing)
			return;
		m_closing = true;
		if (m_passwordField != nullptr)
			m_mod->setPassword(m_passwordField->getText());
		if (mc != nullptr)
			mc->displayGuiScreen(m_parent);
	}

	GuiScreen *m_parent;
	AutoLoginMod *m_mod;
	GuiTextField *m_passwordField = nullptr;
	bool m_closing = false;
};

AutoLoginMod::AutoLoginMod()
{
}

std::string AutoLoginMod::getDescription() const
{
	return "Replies to /register and /login prompts automatically";
}

void AutoLoginMod::onInit(Minecraft *mc)
{
	m_mc = mc;
	loadConfig();
}

void AutoLoginMod::openSettings(Minecraft *mc)
{
	if (mc != nullptr)
		mc->displayGuiScreen(new GuiAutoLoginSettings(mc->currentScreen, this));
}

void AutoLoginMod::setPassword(const std::string &password)
{
	m_password = password;
	saveConfig();
}

void AutoLoginMod::onChatMessageReceived(const std::string &message)
{
	if (!m_enabled || m_password.empty() || m_mc == nullptr || m_mc->thePlayer == nullptr)
		return;

	// Only the multiplayer player can send chat commands.
	EntityClientPlayerMP *player = dynamic_cast<EntityClientPlayerMP *>(m_mc->thePlayer);
	if (player == nullptr || player->sendQueue == nullptr)
		return;

	const long long nowUs = static_cast<long long>(PlatformCompat::getMonotonicMicros());
	if (nowUs - m_lastSendUs < 5000000LL) // at most one automatic reply every 5 seconds
		return;

	// AuthMe-family prompts all name the command itself ("Please login
	// with \"/login password\"", "register with /register") in English and
	// in most translations -- and every match below sends the SAVED
	// PASSWORD to the server, so the bar is "the line tells the player to
	// run the auth command", not merely "the word appears anywhere". A
	// chat line like "how do I login?" must not make the mod answer it
	// with the password; the 5 s rate limit bounds even a real prompt.
	const std::string lower = toLower(message);
	if (lower.find("/register") != std::string::npos ||
	    lower.find("register with") != std::string::npos)
	{
		m_lastSendUs = nowUs;
		MC_LOG_INFO("autologin", "Register prompt detected, sending /register\n");
		player->sendChatMessage("/register " + m_password + " " + m_password);
	}
	else if (lower.find("/login") != std::string::npos ||
	         lower.find("login with") != std::string::npos)
	{
		m_lastSendUs = nowUs;
		MC_LOG_INFO("autologin", "Login prompt detected, sending /login\n");
		player->sendChatMessage("/login " + m_password);
	}
}

void AutoLoginMod::loadConfig()
{
	std::vector<unsigned char> bytes;
	if (!PlatformStorage::readFile(autoLoginConfigPath(), bytes) || bytes.empty())
		return;

	std::string content(bytes.begin(), bytes.end());
	const std::string prefix = "password=";
	if (content.rfind(prefix, 0) == 0)
		content = content.substr(prefix.size());

	while (!content.empty() && (content.back() == '\r' || content.back() == '\n' || content.back() == ' ' || content.back() == '\t'))
		content.pop_back();
	while (!content.empty() && (content.front() == ' ' || content.front() == '\t'))
		content.erase(content.begin());

	m_password = content;
}

void AutoLoginMod::saveConfig()
{
	PlatformStorage::mkdirs(autoLoginConfigDir());
	const std::string content = "password=" + m_password + "\n";
	PlatformStorage::writeFile(autoLoginConfigPath(), content.data(), content.size());
	MC_LOG_INFO("autologin", "Saved config\n");
}
