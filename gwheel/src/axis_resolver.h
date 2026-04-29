#pragma once

#include "wheel.h"

#include <string>

// gwheel::axis_resolver — resolves named action bindings into per-tick axis
// values driven by the current Snapshot.
//
// Storage model: each action (e.g. "SteerAxis", "ThrottleAxis", "BrakeAxis")
// holds a single delimited string of tagged tokens, identical to the format
// the Mod Settings InputBinding primitive emits:
//
//   "wheel-axis:lY|wheel-axis:slider0"   <- two sources, max() combines
//   "wheel-axis:lX"                      <- one source
//   ""                                    <- unbound; Resolve returns 0
//
// Combination rules:
//   - Throttle / brake / clutch class actions: unsigned max() across all
//     bound `wheel-axis:NAME` tokens.
//   - Steer class actions: first signed source wins (the steer axis lives
//     in [-1..+1] and combining steer sources by max() would be wrong).
//   - Tokens that aren't `wheel-axis:` are ignored at this layer (the
//     button-side handler is in input_bindings).
//
// Phase 5 ships the resolver as a self-contained module with its own
// binding storage. Phase 6 will hand over storage to config (action
// bindings persist in config.json) and the resolver becomes a pure
// stateless function over that map. Until then, the SetBinding API is
// the testing seam.

namespace gwheel::axis_resolver
{
    enum class Combine
    {
        UnsignedMax,  // throttle / brake / clutch — max() across sources
        SignedFirst,  // steer — first non-zero signed source wins
    };

    // Set the binding string for `actionName`. Empty string clears.
    // Thread-safe; stored bindings are read by Resolve from any thread.
    void SetBinding(const char* actionName, const char* value);

    // Read the current binding string. Returns empty string if unset.
    std::string GetBinding(const char* actionName);

    // Resolve the action's bound axes against `snap`, combined per `mode`.
    // Returns 0 when unbound, when no `wheel-axis:` tokens are present, or
    // when `snap.connected` is false.
    float Resolve(const char* actionName, const wheel::Snapshot& snap, Combine mode);
}
