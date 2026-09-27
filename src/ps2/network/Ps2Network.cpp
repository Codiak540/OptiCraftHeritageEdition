#ifdef PS2_PLATFORM

#include "ps2/network/Ps2Network.h"

#include "platform/Log.h"
#include "platform/Mutex.h"
#include "ps2/system/Ps2IrxLoader.h"

#include <delaythread.h>
#include <kernel.h>

extern "C" {
#include <ps2ip.h>
}

#include <atomic>
#include <mutex>
#include <string>

namespace
{
PlatformMutex s_initMutex;
std::atomic_bool s_ready{false};
bool s_stackInitialized = false;
std::string s_localAddress;

void loadNetworkModules()
{
    // Modern EE ps2ip keeps lwIP on the EE. Only the Ethernet driver and
    // NETMAN bridge are required on the IOP; ps2ip-nm.irx is intentionally not
    // loaded because that is the alternative IOP-side TCP/IP stack.
    const int dev9 = Ps2IrxLoader::load("irx/ps2dev9.irx", "host:ps2dev9.irx");
    const int netman = Ps2IrxLoader::load("irx/netman.irx", "host:netman.irx");
    const int smap = Ps2IrxLoader::load("irx/smap.irx", "host:smap.irx");

    // A loader such as ps2link/OPL may already have one or more of these IRXs
    // resident, in which case a duplicate load can return an error. Do not fail
    // solely on the loader return values; the sm0 interface check below is the
    // definitive test that the Ethernet path came up.
    MC_LOG_INFO("network", "[PS2] IRX network load: dev9=%d netman=%d smap=%d\n",
                dev9, netman, smap);
}

bool startStackAndDhcp()
{
    if (s_stackInitialized)
        return true;

    loadNetworkModules();

    // ps2ipInit initializes NETMAN itself, then registers the EE-side lwIP
    // stack as sm0. Calling NetManInit a second time here is unnecessary and
    // can make ownership/deinit semantics ambiguous.
    ip4_addr ip{};
    ip4_addr mask{};
    ip4_addr gateway{};
    IP4_ADDR(&ip, 169, 254, 0, 1);
    IP4_ADDR(&mask, 255, 255, 0, 0);
    IP4_ADDR(&gateway, 0, 0, 0, 0);
    MC_LOG_INFO("network", "[PS2] calling ps2ipInit\n");
    McLog::flush();
    if (ps2ipInit(&ip, &mask, &gateway) < 0)
    {
        MC_LOG_ERROR("network", "[PS2] ps2ipInit failed\n");
        return false;
    }

    MC_LOG_INFO("network", "[PS2] ps2ipInit OK\n");
    McLog::flush();

    t_ip_info info{};
    if (!ps2ip_getconfig(const_cast<char *>("sm0"), &info))
    {
        MC_LOG_ERROR("network", "[PS2] network interface sm0 was not registered\n");
        ps2ipDeinit();
        return false;
    }

    MC_LOG_INFO("network", "[PS2] sm0 registered, enabling DHCP\n");
    McLog::flush();
    info.dhcp_enabled = 1;
    if (!ps2ip_setconfig(&info))
    {
        MC_LOG_ERROR("network", "[PS2] failed to enable DHCP on sm0\n");
        ps2ipDeinit();
        return false;
    }

    // Keep the stack resident even if the first DHCP wait times out. DHCP can
    // finish later (for example after a cable is connected), and the next
    // connection attempt can then succeed without reinitializing lwIP.
    s_stackInitialized = true;
    MC_LOG_INFO("network", "[PS2] DHCP client started\n");
    McLog::flush();
    return true;
}
}

namespace Ps2Network
{
bool initialize()
{
#ifdef NO_NETWORK
    return false;
#else
    if (s_ready.load(std::memory_order_acquire))
        return true;

    MC_LOG_INFO("network", "[PS2] network initialize entered\n");
    McLog::flush();
    std::lock_guard<PlatformMutex> guard(s_initMutex);
    MC_LOG_INFO("network", "[PS2] network init lock acquired\n");
    McLog::flush();
    if (s_ready.load(std::memory_order_relaxed))
        return true;
    if (!startStackAndDhcp())
        return false;

    // Leave DHCP alone while lwIP negotiates the lease. Repeated EE-side
    // ps2ip_getconfig()/NETMAN RPCs during this window have caused intermittent
    // IOP stalls on real hardware, including loss of PAD input. The network
    // workers can safely wait here without blocking the render/input thread.
    // After the grace period, let the socket operation determine whether DHCP
    // has finished; a failed connection can be retried while DHCP keeps running.
    constexpr int kDhcpGracePeriodUs = 10000000;
    DelayThread(kDhcpGracePeriodUs);

    s_ready.store(true, std::memory_order_release);
    MC_LOG_INFO("network", "[PS2] DHCP grace period complete; enabling socket attempts\n");
    return true;
#endif
}

bool isReady()
{
    return s_ready.load(std::memory_order_acquire);
}

std::string localAddress()
{
    std::lock_guard<PlatformMutex> guard(s_initMutex);
    return s_localAddress;
}
}

#endif
