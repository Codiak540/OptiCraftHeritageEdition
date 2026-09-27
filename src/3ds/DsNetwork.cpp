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

	const Result result = socInit(contextBuffer, contextSize);
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
	// interface is up, which is fine -- the connect() attempt then reports
	// the real problem.
	struct in_addr ip = {};
	struct in_addr netmask = {};
	struct in_addr broadcast = {};
	if (SOCU_GetIPInfo(&ip, &netmask, &broadcast) == 0)
		address = inet_ntoa(ip);
	else
		address = "0.0.0.0";

	ready = true;
	MC_LOG_INFO("3ds", "network: ready, ip=%s\n", address.c_str());
	return true;
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
	address.clear();
}

}
