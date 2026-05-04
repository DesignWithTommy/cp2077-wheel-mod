#pragma once

// Logitech wheel backend driving the legacy Logitech Steering Wheel SDK
// (vendor/LogitechSDK_unpacked/Lib/x64/LogitechSteeringWheelLib.lib).
//
// This is the fallback path: any G-series wheel that's recognised by
// G HUB but doesn't have the TrueForce extension installed (or wheels
// older than G923) ends up here. The LogitechTrueForce backend takes
// precedence when its DLL is loadable.
//
// Implementation lives entirely inside the .cpp file — the class shape
// is hidden so call sites only see the IWheelBackend interface.

#include "wheel_backend.h"

namespace gwheel::wheel_backend
{
    // Constructed by the factory in wheel_backend.cpp.
    std::unique_ptr<IWheelBackend> CreateLogitechLegacyBackend();
}
