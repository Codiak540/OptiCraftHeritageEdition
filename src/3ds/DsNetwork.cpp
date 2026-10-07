#include "platform/Log.h"
#include "3ds/DsNetwork.h"

#include <malloc.h>
#include <mutex>

#include <3ds.h>
#include <arpa/inet.h>
#include <netinet/in.h>

namespace DsNetwork
{
namespace
{
std::mutex stateMutex;
bool ready = false;
std::string address;
// Set once the bounded DHCP wait below has run (success or not); reset by
// shutdown(). The wait then happens at most once per session instead of
// once per server-list row.
bool interfaceWaitDone = false;

// Same shape devkitPro's own sockets example uses
// (examples/3ds/network/sockets): page aligned, a multiple of 0x1000. After
// socInit() the SOC kernel owns the block and maps it no-access to the app,
// so it must stay untouched and allocated until socExit() hands it back --
// hence the raw memalign() allocation living here instead of inside one
// call. It is created on the first connect attempt, so a session that never
// joins a server never pays the megabyte.
constexpr unsigned int contextAlign = 0x1000;
constexpr unsigned int contextSize = 0x100000;
u32 *contextBuffer = nullptr;
}

bool initialize()
{
	std::lock_guard<std::mutex> guard(stateMutex);
	if (ready)
		return true;

	contextBuffer = static_cast<u32 *>(memalign(contextAlign, contextSize));
	if (contextBuffer == nullptr)
	{
		MC_LOG_ERROR("3ds", "network: soc context allocation failed (%u bytes)\n", contextSize);
		return false;
	}

	// Like the Wii backend's net_init() retry loop: socInit can fail
	// transiently while the SOC service is still settling (notably right
	// after the radio was switched back on), so retry a few times before
	// failing the join.
	Result result = -1;
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		result = socInit(contextBuffer, contextSize);
		if (result == 0)
			break;
		MC_LOG_WARN("3ds", "network: socInit attempt %d failed (0x%08lX), retrying\n",
		            attempt + 1, static_cast<unsigned long>(result));
		svcSleepThread(100 * 1000 * 1000LL);
	}
	if (result != 0)
	{
		MC_LOG_ERROR("3ds", "network: socInit failed (0x%08lX)\n",
		             static_cast<unsigned long>(result));
		free(contextBuffer);
		contextBuffer = nullptr;
		return false;
	}

	// Informational only: only the log line consumes it today (no screen
	// prints the local IP yet). The SOC service answers 0.0.0.0 while no
	// interface is up; waitForInterfaceAddress() (worker threads only)
	// gives DHCP a bounded grace period before the connect() attempt
	// reports the real problem.
	struct in_addr ip = {};
	struct in_addr netmask = {};
	struct in_addr broadcast = {};
	if (SOCU_GetIPInfo(&ip, &netmask, &broadcast) == 0 && ip.s_addr != 0)
		address = inet_ntoa(ip);
	else
		address = "0.0.0.0";

	ready = true;
	MC_LOG_INFO("3ds", "network: ready, ip=%s\n", address.c_str());
	return true;
}

bool waitForInterfaceAddress()
{
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		if (!ready)
			return false;
		// Fast path: the interface is already up, or a previous call
		// already spent the wait budget for this session.
		struct in_addr ip = {};
		struct in_addr netmask = {};
		struct in_addr broadcast = {};
		if (SOCU_GetIPInfo(&ip, &netmask, &broadcast) == 0 && ip.s_addr != 0)
		{
			address = inet_ntoa(ip);
			interfaceWaitDone = true;
			return true;
		}
		if (interfaceWaitDone)
			return false;
		interfaceWaitDone = true;
	}

	// PS2-analog DHCP grace: after a fresh socInit (or a just re-enabled
	// radio) the interface can still be coming up. Poll without holding
	// stateMutex so concurrent rows are not serialized behind the wait;
	// SOCU_* calls are plain IPC and thread-safe. Worker threads only
	// (DsSocket::connect) -- never the game thread, which shares
	// initialize() through the QR downloader.
	for (int attempt = 0; attempt < 20; ++attempt)
	{
		svcSleepThread(250 * 1000 * 1000LL);
		struct in_addr ip = {};
		struct in_addr netmask = {};
		struct in_addr broadcast = {};
		if (SOCU_GetIPInfo(&ip, &netmask, &broadcast) == 0 && ip.s_addr != 0)
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			address = inet_ntoa(ip);
			MC_LOG_INFO("3ds", "network: interface up after DHCP wait, ip=%s\n",
			            address.c_str());
			return true;
		}
	}
	MC_LOG_WARN("3ds", "network: no interface address after DHCP wait; connect will report the failure\n");
	return false;
}

bool isReady()
{
	std::lock_guard<std::mutex> guard(stateMutex);
	return ready;
}

const std::string &localAddress()
{
	return address;
}

void shutdown()
{
	std::lock_guard<std::mutex> guard(stateMutex);
	if (!ready)
		return;
	socExit();
	free(contextBuffer);
	contextBuffer = nullptr;
	ready = false;
	interfaceWaitDone = false;
	address.clear();
}

// One ACU_GetWifiStatus round-trip through the ac:u service (ref-counted by
// libctru, so an init/exit pair per query is cheap and safe even beside
// another user). 0 is the service's "wireless disabled" answer.
namespace
{
// Serialises callers: the body can sleep up to a second probing the radio
// after svcSetWifiEnabled, and up to five poll threads plus the join worker
// plus a QR download may enter it at once. Two concurrent auto-enable
// attempts would race on the radio itself, and a second acExit while the
// first caller still holds a ref is how a ref-counted service gets torn
// down under its user.
std::mutex wifiProbeMutex;

enum class WifiRadioState
{
	Unknown,
	Off,
	On,
};

WifiRadioState queryWifiRadioState()
{
	u32 wifiStatus = 0;
	if (R_FAILED(acInit()))
		return WifiRadioState::Unknown;
	const Result status = ACU_GetWifiStatus(&wifiStatus);
	acExit();
	if (R_FAILED(status))
		return WifiRadioState::Unknown;
	return wifiStatus != 0 ? WifiRadioState::On : WifiRadioState::Off;
}
} // namespace

std::string wifiPreflightError()
{
	std::lock_guard<std::mutex> probeGuard(wifiProbeMutex);
	const WifiRadioState radio = queryWifiRadioState();
	if (radio != WifiRadioState::Off)
		return std::string(); // on, or unknowable -- never block the attempt

	MC_LOG_WARN("3ds", "network: Wi-Fi is switched off\n");

	// svcSetWifiEnabled exists from kernel 2.55 (system 11.4). Older kernels
	// simply keep the message below, which is all they can do.
	if (osGetKernelVersion() >= SYSTEM_VERSION(2, 55, 0))
	{
		const Result enabledResult = svcSetWifiEnabled(true);
		if (R_SUCCEEDED(enabledResult))
		{
			// Give the radio a moment to come up before believing it. On
			// success let the pending join proceed: initialize() waits for
			// the DHCP address and the connect attempt reports any further
			// failure itself, so the first press joins instead of asking
			// for a second one.
			for (int attempt = 0; attempt < 10; ++attempt)
			{
				if (queryWifiRadioState() == WifiRadioState::On)
				{
					MC_LOG_INFO("3ds", "network: Wi-Fi re-enabled by the game\n");
					return std::string();
				}
				svcSleepThread(100 * 1000 * 1000LL);
			}
		}
		else
		{
			MC_LOG_WARN("3ds", "network: svcSetWifiEnabled failed (%08lX)\n",
			            static_cast<unsigned long>(enabledResult));
		}
	}

	return "Wi-Fi is switched off. Enable wireless (HOME menu toggle on a New 3DS, "
	       "the side switch on an Old 3DS) and connect again.";
}

}
