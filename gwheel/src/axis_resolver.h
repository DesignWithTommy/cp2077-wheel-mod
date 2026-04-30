#pragma once

#include "wheel.h"

#include <string>

// gwheel::axis_resolver — resolves named action bindings into per-tick axis
// values driven by the current Snapshot.
//
// Storage model: each action ("SteerAxis" / "ThrottleAxis" / "BrakeAxis")
// holds a `|`-delimited list of tagged tokens (1 or 2 in practice), each
// identical to what the Mod Settings InputBinding primitive emits:
//
//   "wheel-axis:lY"                       <- single source
//   "wheel-axis:lRz|wheel-axis:slider0"   <- two pedals route into BrakeAxis
//   ""                                     <- unbound; Resolve returns 0
//
// Combination rules:
//   - Pedal-class actions (throttle / brake): max() across all bound
//     `wheel-axis:NAME` tokens, unsigned-clamped 0..1.
//   - Steer-class actions: first bound source wins, signed [-1..+1].
//   - Tokens that aren't `wheel-axis:` resolve to 0 here. (Button-side
//     dispatch lives in input_bindings, not this module.)

namespace gwheel::axis_resolver
{
    enum class Combine
    {
        UnsignedMax,  // throttle / brake — clamp(axis, 0, 1)
        SignedFirst,  // steer — clamp(axis, -1, +1)
    };

    // Set the binding string for `actionName`. Empty string clears.
    // Thread-safe; stored bindings are read by Resolve from any thread.
    void SetBinding(const char* actionName, const char* value);

    // Drop every stored binding. Used by config::ApplyDerived to do a
    // clean clear-then-repopulate when actionBindings changes.
    void Clear();

    // Read the current binding string. Returns empty string if unset.
    std::string GetBinding(const char* actionName);

    // Resolve the action's bound axis against `snap`, signed/unsigned per
    // `mode`. Returns 0 when unbound, when the token isn't a `wheel-axis:`
    // form, or when `snap.connected` is false.
    float Resolve(const char* actionName, const wheel::Snapshot& snap, Combine mode);
}
