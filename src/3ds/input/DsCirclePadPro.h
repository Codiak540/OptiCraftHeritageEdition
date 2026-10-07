// DsCirclePadPro.h -- Circle Pad Pro accessory support for Old 3DS/XL.
//
// The CPP is an IR accessory: the console's C-stick-shaped right pad plus
// ZL/ZR (and a mirrored R) report through the infrared port on the back of
// the console, not through HID. New 3DS internal hardware has nothing to do
// with it -- its C-stick flows through the ir:rst shared memory, which is
// why libctru's stock hidShouldUseIrrst() gate only answers true there.
//
// On Old hardware the accessory needs the title itself to drive the raw
// ir:USER protocol (3dbrew, "Circle Pad Pro"): initialize the IRNOP shared
// ring, request a connection to device 1, read the accessory's calibration
// data (CRC-8 validated), then poll "Read Input" requests at the response
// period the accessory asks for and decode 12-bit pad positions, the
// battery level and the active-low ZL/ZR/R bits from the responses. That is
// the same path retail CPP-compatible cartridges and every hardware-proven
// homebrew implementation drive; ir:rst does not carry the accessory there
// (see DsInput.cpp's hidShouldUseIrrst note).
//
// The worker lives on its own thread and retries forever, so the accessory
// can be clipped on, powered on, put to sleep or unclipped at any time:
// the game never blocks, and input simply resumes when the IR link does.

#pragma once

#if defined(CTR_PLATFORM)

#include <cstdint>

namespace DsCirclePadPro
{

// The worker's last published state. Cheap to sample: every field is an
// atomic load, so dsInputPoll() can fold this in beside HID's snapshot.
struct Sample
{
    // True once the IR link is up AND calibration succeeded -- the only
    // state in which the axes and buttons below are meaningful.
    bool connected = false;

    // Calibrated right-pad position in the left circle pad's range
    // (roughly -0x9C..0x9C), the axes convention hidCircleRead uses.
    std::int16_t dx = 0;
    std::int16_t dy = 0;

    // The accessory's own buttons, in HID bit positions so dsInputPoll can
    // OR them straight into the held mask: KEY_R | KEY_ZL | KEY_ZR subset.
    std::uint32_t held = 0;

    // The accessory's raw 5-bit battery reading (0..31, the flag byte's
    // low bits per 3dbrew); 0 with nothing connected.
    std::uint8_t battery = 0;
};

// Start the accessory worker. Idempotent (Display re-creating the input
// layer must not spawn a second thread), and a no-op on New 3DS -- there
// the internal C-stick arrives through ir:rst and no accessory exists -- or
// when the ir:USER service cannot be reached, in which case the console
// simply has no right-pad input at all, like any Old 3DS sans CPP.
void init();

// Signal the worker to stop, join it, and release its IR resources.
// Idempotent, and safe to call even when init() never started anything:
// both the normal shutdown path and the atexit crash-path handler call
// this, in either order.
void shutdown();

// The latest worker state; a zeroed Sample when the worker is absent.
Sample sample();

// Whether the worker actually runs (an Old 3DS/XL with ir:USER reachable).
// Diagnostic surface only -- sample() already reports everything gameplay
// needs, and a New 3DS answers false by design.
bool available();

} // namespace DsCirclePadPro

#endif // CTR_PLATFORM
