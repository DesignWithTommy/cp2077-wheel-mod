#pragma once

#include "sources.h"

#include <cstdint>
#include <string>

namespace gwheel::input_bindings
{
    // Replace one action's binding string. `actionName` is the action key
    // ("Horn" / "Headlights" / etc., matching the names redscript pushes
    // through GWheel_SetActionBinding). `value` is a tagged token from the
    // Mod Settings InputBinding primitive — e.g. "wheel:5", "wheel-pov:18000".
    // Empty string erases the entry. Tokens that don't represent an
    // edge-detected source on the wheel (axis tokens, keyboard tokens) are
    // ignored at this layer; axis_resolver handles axes, and keyboard
    // sources don't need re-injection (the user is already pressing the
    // physical key).
    void SetActionBinding(const std::string& actionName, const std::string& value);

    // Drop every stored binding. Called by config::ApplyDerived ahead of a
    // full repopulate so deletions in the on-disk schema take effect.
    void Clear();

    // Called once per pump tick with the latest input frame. Edge-detects
    // each bound action's wheel-button / POV sources and dispatches the
    // matching DispatchEntry via SendInput. Gated internally on
    // config.input.enabled. Vehicle-only actions are suppressed when V is
    // on foot (sources::InVehicle).
    void OnTick(const sources::Frame& frame);

    // ---------- Capture mode ------------------------------------------------
    //
    // Mod Settings' InputBinding primitive calls these from the listening
    // UI to capture wheel buttons / axis movement as tagged-string bindings.
    // While capture is armed, OnTick suppresses normal action dispatch —
    // pressing a paddle to bind it must NOT also fire the paddle's
    // currently-bound action.
    //
    // Lifecycle (caller-driven):
    //   1. UI calls BeginCapture("button") or BeginCapture("axis").
    //   2. Each frame the UI polls PollLatestCapture(); empty string =
    //      nothing captured yet, non-empty = tagged token (e.g. "wheel:5",
    //      "wheel-axis:lY", "wheel-pov:18000"). The poll consumes the
    //      slot atomically.
    //   3. UI calls CancelCapture() either after PollLatestCapture
    //      returned a value (cleanup), or because the user pressed cancel
    //      / clicked elsewhere.
    //
    // typeHint values:
    //   "button" : capture first rising-edge DInput button or POV-direction
    //              transition. Axis movements are ignored.
    //   "axis"   : capture first axis whose value deviates from its
    //              BeginCapture baseline by more than ~0.25. Buttons / POV
    //              are ignored.
    void BeginCapture(const char* typeHint);
    std::string PollLatestCapture();   // empty = nothing yet
    void CancelCapture();
    bool IsCapturing();
}
