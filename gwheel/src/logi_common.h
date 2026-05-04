#pragma once

// Shared helpers used by both Logitech wheel backends (LogitechLegacy and
// LogitechTrueForce). Lives between the backend implementations and the
// vendor-specific SDKs so the two backends never duplicate the same plumbing.
//
// Future non-Logitech backends (Thrustmaster, Moza) get their own
// `<vendor>_common.{h,cpp}` siblings and never call into this header.
//
// Nothing in here calls into `LogitechSteeringWheelLib.h` or the TrueForce
// SDK directly. SDK-specific calls stay inside the backend implementation
// files; this header only holds vendor-neutral DInput plumbing and signal
// normalisation that Logitech wheels happen to share.

#include "wheel.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

namespace gwheel::logi_common
{
    // ---- Axis normalisation ------------------------------------------------
    //
    // Logitech wheels report a DIJOYSTATE2 LONG axis in [-32768, +32767]
    // (verified empirically on a G923 2026-04-21 via diagnostic logging).
    // Both backends consume DInput, so this lives here.
    //
    // NormalizeSteer  : center=0, full-left=-1.0, full-right=+1.0
    // NormalizePedal  : idle=0.0, fully-pressed=1.0 (DInput's signed convention
    //                   is inverted internally so 1.0 always means "fully
    //                   pressed" regardless of which axis the wheel uses).

    float NormalizeSteer(LONG v);
    float NormalizePedal(LONG v);

    // ---- Game-window discovery ---------------------------------------------
    //
    // DirectInput requires a top-level HWND owned by the calling process for
    // exclusive-mode acquisition. Returns nullptr if no usable window exists
    // yet (called from the pump thread; the game window may not have been
    // created at plugin-load time, so callers retry).

    HWND FindOwnGameWindow();

    // ---- DIJOYSTATE2 → wheel::Snapshot conversion --------------------------
    //
    // Translates the raw DInput state into the canonical Snapshot the rest
    // of the plugin consumes. Connection state is the caller's responsibility.

    void PopulateSnapshotFromDInput(const DIJOYSTATE2& raw, wheel::Snapshot& out);
}
