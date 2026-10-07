#pragma once

#include <string>

namespace DsNetwork
{

// Starts the 3DS soc:U socket service over the console's configured network
// (the same settings the HOME menu uses). Safe to call more than once; failed
// attempts may be retried. Retries socInit() a few times like the Wii
// backend's net_init() loop. Stays fast (no DHCP wait here -- the game
// thread shares this through the QR downloader); worker connects use
// waitForInterfaceAddress() for the PS2-style DHCP grace instead.
bool initialize();
bool isReady();
// Bounded wait for a usable interface address (PS2-style DHCP grace after
// a fresh socInit or a just re-enabled radio). Worker threads only: it
// sleeps, so the game thread (which shares initialize() via the QR
// downloader) must never call it. Returns true when an address is up; on
// false the connect attempt still runs and reports the real problem.
bool waitForInterfaceAddress();
bool isReady();
const std::string &localAddress();
void shutdown();

// Pre-flight the wireless radio before a server connect is attempted.
// Returns an empty string when the radio is on (or unknowable -- a loader
// that will not answer ac:u never blocks the attempt), and a
// player-readable, actionable error when the console's Wi-Fi is off, which
// is what "servers never connect" on a New 3DS almost always is: the New
// models replaced the Old 3DS's physical wireless switch with a software
// toggle in the HOME menu, so the radio stays off after a reboot or a
// settings visit, and until now the game only ever answered with the generic
// "Connection refused". On kernel 2.55+ (system 11.4+) it first tries to
// switch the radio on itself (svcSetWifiEnabled): the player is explicitly
// asking to join a server, so enabling the radio matches their intent, and
// on success the pending join proceeds (see initialize()) instead of
// asking for a second press.
//
// Deliberately kept past Wii parity, not regressed to it: TCP_NODELAY stays
// set, the write path keeps its no-progress budget and serialization lock,
// reads stay recv-first, and server-list polls stay on detached threads
// (the Wii blocks its draw thread per row). Socket buffer sizes stay at the
// SOC default -- SO_RCVBUF/SO_SNDBUF were tried and reverted after hardware
// showed joins dying while the emulator stayed fine (see JavaNetwork_3ds);
// re-add only with a hardware A/B, never from an emulator reading.
std::string wifiPreflightError();

}
