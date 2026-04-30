#pragma once

#include <cstdint>

// gwheel::sources — the canonical per-tick view of wheel state + active
// control context. Sits between the hardware readers (Logi SDK today,
// RawInput tomorrow) and the consumers (vehicle_hook for axis injection,
// input_bindings for button→action dispatch).
//
// The hardware readers *publish* into this namespace and the consumers
// *read* from it. Nobody outside this namespace should know which reader
// filled which field; that's the entire point of the seam.

namespace gwheel::sources
{
    struct AxisFrame
    {
        float steer    = 0.f;  // -1..+1, negative = left
        float throttle = 0.f;  //  0..1
        float brake    = 0.f;  //  0..1
    };

    struct DigitalFrame
    {
        uint32_t buttons = 0;       // bit per DInput button, low 32
        uint16_t pov     = 0xFFFF;  // POV[0] raw value, 0xFFFF = center
    };

    struct Frame
    {
        AxisFrame    axes;
        DigitalFrame digital;
        bool         connected = false;
    };

    // Snapshot read. Safe to call from any thread. Returns the most recent
    // published frame. Today filled from wheel.cpp (Logi SDK) via Publish();
    // future raw_input.cpp will publish into the same slot.
    Frame Current();

    // Called once per pump tick by the hardware reader.
    void Publish(const Frame& f);

    // ---------- control context ---------------------------------------------
    //
    // Context is consulted by input_bindings::Dispatch to decide whether
    // a vehicle-centric action should fire (suppressed on-foot, since the
    // bound keyboard keys mean different things when V is walking).

    // Flipped by the redscript mount/unmount wrappers via
    // GWheel_Set/ClearPlayerVehicle; nullptr = not in a vehicle.
    void SetInVehicle(bool v);
    bool InVehicle();

    // ---------- vehicle telemetry pushed from redscript ---------------------
    //
    // Populated by Blackboard listeners in gwheel_vehicle_signals.reds.
    // Cleared to 0/false on vehicle unmount. The LED controller reads
    // these to decide whether to show the rev strip (real RPM) or the
    // music visualizer (radio-driven). Reads are atomic and safe from
    // any thread.

    // Normalized engine RPM in [0..1] = VehicleDef.RPMValue / MaxRPM.
    // 0 when not in a vehicle.
    void  SetEngineRpmNormalized(float v);
    float EngineRpmNormalized();

    // Vehicle radio receiver state — true when the in-car radio is on
    // AND a station is playing. false when radio off or not in a
    // vehicle. Mirrors VehicleDef.VehRadioState.
    void SetRadioActive(bool v);
    bool RadioActive();
}
