#pragma once

// TrueForce runtime loader.
//
// Probes the user's existing G HUB installation for trueforce_sdk_x64.dll
// (deployed by trueforce_manager.exe to %ProgramFiles%\Logi\Trueforce\<ver>\),
// dynamically loads it if found, and binds the export vtable that the
// wheel backend uses to drive both kinesthetic (KF) and vibrotactile
// (TF) force channels.
//
// We never bundle the DLL — the user's licensed copy (deployed by G HUB)
// is the only one ever in play. If the probe doesn't find a usable DLL,
// IsActive() stays false and the wheel backend falls back to the legacy
// Logitech Steering Wheel SDK.
//
// Initialize() respects config.trueForce.enabled — if the user (or the
// FOMOD installer / their hand-edited config.json) has not opted in, we
// skip the probe entirely.

#include "../include/trueforce_api.h"

namespace gwheel::trueforce
{
    // Run the path probe + LoadLibrary + dllOpen. Idempotent; safe to
    // call once at plugin start. Logs the probe outcome to gwheel-*.log.
    void Initialize();

    // dllClose + FreeLibrary. Safe to call regardless of Initialize state.
    void Shutdown();

    // True iff the DLL was found, loaded, all required exports resolved,
    // and dllOpen returned successfully. The wheel backend uses this as
    // its "TrueForce path available" flag.
    bool IsActive();

    // Direct access to the bound vtable. Returns nullptr when IsActive()
    // is false. Caller (the TrueForce wheel backend) is responsible for
    // gating its own calls; nothing in the vtable is a no-op safety net.
    const api::Vtable* Vtable();
}
