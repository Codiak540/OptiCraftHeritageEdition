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
// The worker lives on its own thread behind a bounded probe: while budget
// remains, the accessory can be clipped on, powered on, woken or unclipped
// at any time and input simply resumes with the link; once the budget is
// spent with nothing calibrated the worker tears the IRNOP session down
// and parks (no IR traffic), and the next probe rides a wake, a HOME
// return, a closed system dialog (the dsInputPoll re-arm) or a fresh
// boot -- no retail title cycles the IR machinery forever (see the .cpp
// for the hardware report behind that bound). The game never blocks
// either way.

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
//
// A failed start may be retried: a later call begins a fresh attempt (the
// previous one's resources were already released), so dsInputPoll() re-arms
// it every couple of seconds until the worker runs. That covers loaders
// whose services are not all reachable on the very first attempt -- the
// 2026-10 report where the .cia linked the accessory but the HBL .3dsx of
// the same build never did.
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

// Whether init() got past the New-3DS early-out and really tried to start
// the worker. Stays false on New 3DS by design, so dsInputPoll() can tell
// "nothing to do" apart from "tried and failed" when deciding to retry.
bool startAttempted();

// The stage that stopped the last start attempt, or 0 when the worker runs
// (or nothing was attempted yet). A libctru Result where the stage has one
// (ir:USER handle, memory block, exit event), 1 for the shared-memory
// allocation and 2 for the worker-thread creation, which have none.
// Survives MC_LOG_LEVEL=0, where init()'s MC_LOG_WARN lines are compiled
// out -- dsInputDebugLine() renders a nonzero code into the overlay, so a
// ".3dsx links nothing, .cia links fine" report can be told apart from a
// dead accessory or an unseated clip without rebuilding with logs on.
std::uint32_t startError();

} // namespace DsCirclePadPro

#endif // CTR_PLATFORM
