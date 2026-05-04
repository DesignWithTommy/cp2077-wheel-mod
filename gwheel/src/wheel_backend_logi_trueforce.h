#pragma once

// Logitech wheel backend driving the TrueForce SDK
// (trueforce_sdk_x64.dll, deployed by G HUB to %ProgramFiles%\Logi\Trueforce).
//
// Preferred over the LegacyBackend on G923 / G PRO when:
//   - config.trueForce.enabled is true (FOMOD opt-in / config edit)
//   - trueforce::IsActive() is true (DLL loaded, exports bound)
//   - logiTrueForceSupportedByDirectInputW returns true for the connected wheel
//
// Implementation lives entirely inside the .cpp file.

#include "wheel_backend.h"

namespace gwheel::wheel_backend
{
    std::unique_ptr<IWheelBackend> CreateLogitechTrueForceBackend();
}
