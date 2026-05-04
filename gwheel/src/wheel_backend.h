#pragma once

// Vendor-neutral wheel backend interface.
//
// The wheel:: namespace (wheel.h) keeps its existing public API; under
// the hood it delegates to one of these backends, picked at Init() time.
// New vendors (Thrustmaster, Moza, ...) plug in by adding a sibling
// implementation file and registering with the factory below.
//
// Capability-gated: every backend declares what it can do via Capabilities.
// The wheel pipeline checks the relevant flag before emitting an effect.
// A primitive that isn't supported is a no-op on that backend, never a
// crash; but the call site is expected to gate itself off the cap flag
// rather than spraying calls and relying on the no-op.
//
// Vendor-specific files in this project use the suffix convention:
//   wheel_backend_logi_legacy.{h,cpp}     — Logitech Steering Wheel SDK
//   wheel_backend_logi_trueforce.{h,cpp}  — Logitech TrueForce SDK
//   wheel_backend_<vendor>_*.{h,cpp}      — future vendors

#include "wheel.h"

#include <memory>

namespace gwheel::wheel_backend
{
    enum class Vendor : int
    {
        Unknown      = 0,
        Logitech     = 1,
        Thrustmaster = 2,  // reserved; no implementation yet
        Moza         = 3,  // reserved; no implementation yet
    };

    enum class BackendKind : int
    {
        Unknown          = 0,
        LogitechLegacy   = 1,  // Logitech Steering Wheel SDK
        LogitechTrueForce = 2, // Logitech TrueForce SDK (full superset)
    };

    // What a backend can do. Each flag gates a primitive at the call
    // site. A backend MUST set a flag to true only if the corresponding
    // primitive is implemented and tested for the connected hardware.
    struct Capabilities
    {
        // ---- Force feedback primitives ----
        bool ffbConstant = false;   // signed directional constant force
        bool ffbSpring   = false;   // centering / position spring
        bool ffbDamper   = false;   // velocity-proportional damper
        bool ffbPeriodic = false;   // periodic surface vibration (sine/square/triangle)
        bool ffbAirborne = false;   // dedicated "wheel disconnects from road" effect
        bool ffbJolt     = false;   // one-shot directional kick (collision / bump)

        // ---- Vibrotactile / TrueForce-style channel ----
        // High-frequency audio-stream haptics, separate from the kinesthetic
        // FFB primitives above. Today only LogitechTrueForce sets this true.
        bool vibrotactile = false;

        // ---- LEDs ----
        bool revLeds = false;       // can drive a rev-strip LED bar

        // ---- Wheel state readback ----
        bool angleReadback           = false;  // current angle in degrees
        bool angularVelocityReadback = false;  // angular velocity in deg/s

        // ---- Configuration ----
        bool operatingRangeSet = false;        // can change steering range
        bool globalGainSet     = false;        // SDK exposes a master gain knob

        // ---- Hardware shape (used for UI gating) ----
        bool hasRightCluster = false;          // G923-class right-side button cluster
    };

    // What a backend reports about the connected wheel after Open().
    // Vendor-neutral; populated from the backend's own SDK probes.
    struct DeviceInfo
    {
        Vendor      vendor       = Vendor::Unknown;
        uint16_t    vid          = 0;
        uint16_t    pid          = 0;
        char        productName[256] = {};
        int         operatingRangeDeg = 0;
        int         sdkMajor     = 0;
        int         sdkMinor     = 0;
        int         sdkBuild     = 0;
    };

    // Backend interface. Methods that are gated by a capability flag
    // are documented as such; calling them when the flag is false is a
    // documented no-op (so call sites can elide the cap check during
    // hot-paths where they know the cap, but should NOT rely on this
    // for behavior — a no-op produces zero force, not a fallback).
    class IWheelBackend
    {
    public:
        virtual ~IWheelBackend() = default;

        // ---- Identification ------------------------------------------------

        virtual Vendor      GetVendor()      const = 0;
        virtual BackendKind GetKind()        const = 0;
        virtual const char* GetBackendName() const = 0;  // human-readable, for logs

        // ---- Lifecycle -----------------------------------------------------

        // Initialise the backend's SDK. May return false if prerequisites
        // (G HUB / TrueForce DLL / vendor service) aren't available.
        virtual bool Init() = 0;

        // Detect + open a wheel. Must run after Init. Sets DeviceInfo and
        // Capabilities for the lifetime of this open session.
        virtual bool Open() = 0;

        virtual void Close() = 0;
        virtual bool IsOpen() const = 0;

        // ---- State pump ----------------------------------------------------

        // Refresh DInput state and publish a new wheel::Snapshot.
        // Driven by the plugin's 250 Hz pump thread.
        virtual void Pump() = 0;

        virtual const wheel::Snapshot& CurrentSnapshot() const = 0;
        virtual const DeviceInfo&      GetDeviceInfo()   const = 0;
        virtual const Capabilities&    GetCapabilities() const = 0;

        // ---- FFB primitives ------------------------------------------------
        // All magnitudes / coefficients are normalised; the backend
        // converts to its SDK's native units. Sign on PlayConstant
        // indicates direction (negative = CCW push, positive = CW).

        virtual void PlayConstant(float magnitudeSigned01) = 0;  // gate: caps.ffbConstant
        virtual void StopConstant() = 0;
        virtual void PlaySpring(float coefficient01)       = 0;  // gate: caps.ffbSpring
        virtual void StopSpring() = 0;
        virtual void PlayDamper(float coefficient01)       = 0;  // gate: caps.ffbDamper
        virtual void StopDamper() = 0;
        virtual void PlayPeriodic(float magnitude01, int periodMs) = 0;  // gate: caps.ffbPeriodic
        virtual void StopPeriodic() = 0;
        virtual void SetAirborne(bool airborne)            = 0;  // gate: caps.ffbAirborne
        virtual void StopAll() = 0;

        // ---- Vibrotactile / TF channel -------------------------------------
        // Gate: caps.vibrotactile. Single-sample feed at the haptic rate
        // (queryable via GetHapticRateHz). Backend is responsible for
        // any internal buffering / smoothing.

        virtual void TfFeedSample(float sample)  = 0;
        virtual void TfClearStream()             = 0;
        virtual void TfSetGain(float gain01)     = 0;
        virtual int  GetHapticRateHz() const     = 0;  // returns 0 if unsupported

        // ---- LEDs ----------------------------------------------------------
        // Gate: caps.revLeds. RPM units must be consistent across the
        // three params (the backend doesn't care about the unit).

        virtual void PlayLeds(float currentRpm, float firstLedRpm, float redlineRpm) = 0;
        virtual void ClearLeds() = 0;

        // Direct 0..1 LED bar level. Used by the handshake's LED breath
        // pulse where RPM-style framing doesn't fit. Backends apply any
        // hardware-specific quirk handling internally (e.g. the G923
        // idle-indicator workaround). Gate: caps.revLeds.
        virtual void SetLedLevel(float level01) = 0;

        // ---- Configuration -------------------------------------------------

        // Gate: caps.operatingRangeSet
        virtual void SetOperatingRangeDeg(int degrees) = 0;

        // Gate: caps.globalGainSet. 0..1; 0 = silent, 1 = full SDK output.
        virtual void SetGlobalGain(float gain01)       = 0;

        // ---- State readback ------------------------------------------------

        // Gate: caps.angleReadback. Returns NaN if unsupported.
        virtual float GetWheelAngleDeg() const = 0;
        // Gate: caps.angularVelocityReadback. Returns NaN if unsupported.
        virtual float GetWheelAngularVelocityDeg() const = 0;

        // Cached operating range (degrees), refreshed inside Pump() from the
        // backend's SDK. Used by ApplyFfbFromPhysics for range scaling.
        // Returns 0 if not yet known.
        virtual int GetOperatingRangeDeg() const = 0;

        // True iff the user has FFB hardware (capability subset, but the
        // existing FFB pipeline reads it as a top-level gate). Equivalent
        // to GetCapabilities().ffbConstant.
        virtual bool HasFFB() const = 0;
    };

    // Factory: instantiate the highest-priority backend whose Init()
    // succeeds and whose Open() finds a connected wheel.
    //
    // Priority order today (highest first):
    //   1. LogitechTrueForce  — preferred when trueforce::IsActive() is true
    //                           AND the connected wheel reports
    //                           logiTrueForceSupportedByDirectInputW.
    //   2. LogitechLegacy     — fallback for Logitech wheels without
    //                           TrueForce support, or when the TrueForce
    //                           DLL isn't installed.
    //
    // Returns nullptr if no backend can claim a wheel; callers treat
    // this as "no wheel attached" (UI gates everything off).
    std::unique_ptr<IWheelBackend> CreateActive();
}
