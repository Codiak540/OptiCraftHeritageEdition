#pragma once

#include "mods/IMod.h"
#include <string>

class Minecraft;

// Auto-Login: answers server auth prompts automatically. When the server
// sends a chat line that asks the player to /register or /login (AuthMe and
// friends), the mod replies with the password saved in its settings screen,
// rate-limited to one automatic reply every 5 seconds.
//
// Portable .ochpack mod: ModManager maps the "autologin" pack id to this
// class, so the mod only materializes when autologin.ochpack is installed
// in the game's mods directory and stays toggleable/removable from the
// Mods menu like every other pack.
class AutoLoginMod : public IMod
{
public:
	AutoLoginMod();

	std::string getId() const override { return "autologin"; }
	std::string getName() const override { return "Auto-Login"; }
	std::string getVersion() const override { return "1.0"; }
	std::string getDescription() const override;
	std::string getAuthor() const override { return "Nuvteix"; }

	bool isEnabled() const override { return m_enabled; }
	void setEnabled(bool enabled) override { m_enabled = enabled; }

	void onInit(Minecraft *mc) override;
	void onChatMessageReceived(const std::string &message) override;

	bool hasSettings() const override { return true; }
	void openSettings(Minecraft *mc) override;

	// Password access for the settings screen (GuiAutoLoginSettings).
	const std::string &getPassword() const { return m_password; }
	void setPassword(const std::string &password);

private:
	bool m_enabled = true;
	Minecraft *m_mc = nullptr;
	std::string m_password;
	long long m_lastSendUs = 0; // PlatformCompat::getMonotonicMicros() of the last automatic reply

	void loadConfig();
	void saveConfig();
};
