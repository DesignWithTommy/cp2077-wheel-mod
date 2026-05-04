#pragma once

// Vendor-neutral TrueForce / vibrotactile signal pipeline.
//
// Reads game state (engine RPM + throttle + suspension envelope + skid)
// and produces a single TF sample per tick. The sample is fed to the
// active wheel backend's TF channel via g_backend->TfFeedSample(). Only
// fires when:
//   - g_backend != nullptr
//   - g_backend->GetCapabilities().vibrotactile is true
//   - config.trueForce.feedSignals is true
//
// The pipeline is called from the tail of ApplyFfbFromPhysics in wheel.cpp,
// at the same cadence as the kinesthetic FFB updates (vehicle detour rate,
// roughly 60 Hz). The TF channel proper runs at the wheel's haptic rate
// (~4000 Hz on G923 / G PRO); the SDK is responsible for any internal
// interpolation between our 60-Hz envelope updates.
//
// Sample value range is best-guess [-1, +1] (audio-sample semantics) until
// the official Logitech headers confirm. Implementation caps output magnitude
// well below 1.0 to avoid driving the wheel hard while signatures are
// unverified.

namespace gwheel::wheel_tf
{
    // Inputs gathered by the caller (wheel.cpp's ApplyFfbFromPhysics).
    // All fields are vendor-neutral, normalised values.
    struct Inputs
    {
        float rpmNorm           = 0.f; // engine RPM as fraction of redline (0..1)
        float throttle          = 0.f; // 0..1 driver input
        float surfaceEnvelope   = 0.f; // 0..1 LPF'd suspension activity (already computed in wheel.cpp)
        float skidNorm          = 0.f; // 0..1 |slipSignal|/slipSat from the SAT countersteer term
        bool  isOnGround        = true;
        bool  isMoving          = true;
        bool  isReversing       = false;
    };

    // Feeds one sample. No-op when caps.vibrotactile is false or the
    // feedSignals flag is off in config. Safe to call every tick.
    void Feed(const Inputs& in);

    // Cleared at session boundaries (mount/unmount/pause) to reset the
    // engine oscillator's phase accumulator and the impact transient
    // memory so the next mount starts from silence.
    void Reset();
}
