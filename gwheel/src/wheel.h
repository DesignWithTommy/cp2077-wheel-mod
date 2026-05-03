#pragma once

#include <cstdint>

namespace gwheel::wheel
{
    // Stable index into Snapshot::axes for each DInput axis. Order is
    // append-only; saved binding strings reference these names ("wheel-axis:lY"
    // etc.) so renumbering breaks user configs. The string form is the
    // user-facing identifier; the enum value is the in-process index.
    enum Axis : int32_t
    {
        Axis_lX = 0,
        Axis_lY,
        Axis_lZ,
        Axis_lRx,
        Axis_lRy,
        Axis_lRz,
        Axis_slider0,
        Axis_slider1,
        Axis_kCount
    };

    // Map "lY" / "slider0" / etc. -> Axis index. Returns Axis_kCount when
    // the name doesn't match any known axis. Lookup is case-sensitive
    // because the binding-string vocabulary is fixed.
    Axis AxisFromName(const char* name);
    const char* AxisName(Axis a);

    // Controller-axis / button / POV snapshot, published each pump tick.
    //
    // The eight DInput axes are exposed by Axis index in `axes[]`. Consumers
    // (axis_resolver, the binding-driven button dispatcher) read them by
    // name via the user's bindings — there are no hardcoded per-device
    // mappings.
    //
    // Axis sign convention in `axes[]`:
    //   - Steer-class axes (lX): -1..+1, negative = left.
    //   - Pedal-class axes (lY, lZ, lRx, lRy, lRz, slider0, slider1):
    //     0..1, raw DInput pedals are inverted at this layer so 1 = full
    //     press regardless of which axis a wheel maps the pedal to.
    struct Snapshot
    {
        float    axes[Axis_kCount] = {0};
        uint32_t buttons   = 0;   // bit per button, low 32
        uint16_t pov       = 0xFFFF;
        bool     connected = false;
    };

    struct Caps
    {
        uint16_t vid = 0;
        uint16_t pid = 0;
        char     productName[256] = {};
        bool     hasFFB = false;
        int      operatingRangeDeg = 0;
        int      sdkMajor = 0;
        int      sdkMinor = 0;
        int      sdkBuild = 0;
    };

    bool Init();            // verify SDK version + schedule deferred LogiSteeringInitialize
    void Shutdown();
    bool IsReady();
    void Pump();            // LogiUpdate + publish snapshot; driven by plugin pump thread
    Snapshot CurrentSnapshot();
    const Caps& GetCaps();

    // FFB. Constant takes -1..+1 (sign is direction); others are 0..1.
    // Percentages -100..+100 are derived from the float inputs.
    void PlayConstant(float magnitude);
    void StopConstant();
    void PlayDamper(float coefficient);
    void StopDamper();
    void PlaySpring(float coefficient);
    void StopSpring();
    void PlayRoadSurface(float magnitude, int periodMs);  // magnitude 0..1, period in ms (SINE)
    void StopRoadSurface();
    void PlayCarAirborne();
    void StopCarAirborne();
    void SetGlobalStrength(float mul);   // 0..1 multiplier applied to all effect magnitudes
    void StopAll();

    // Drive the wheel's rev-strip LEDs to a normalised 0..1 level. 0 = all
    // dark, 1 = full bar (green → amber → red). Safe to call at any
    // rate; the SDK rate-limits internally. No-op if the SDK isn't bound
    // or no wheel is connected.
    void PlayLeds(float level);

    // Pass-through to LogiPlayLeds in raw RPM units. firstLedRpm is the
    // RPM at which the first green LED lights; shiftRpm is where the bar
    // saturates (last red blink). currentRpm is the live engine RPM.
    // The SDK accepts any unit as long as all three values share it.
    // Used by the LED rev-strip path to drive a per-gear shift indicator
    // (currentRpm reaches shiftRpm at the AI's actual upshift, not at
    // global redline).
    void PlayLedsRaw(float currentRpm, float firstLedRpm, float shiftRpm);

    // Force the LED bar dark. Called on LED-feature disable and on
    // plugin shutdown so the bar doesn't latch at its last value.
    void ClearLeds();

    // True while the startup handshake thread is driving the wheel. The
    // LED controller yields the bar during this window so the handshake
    // sweep / flashes / breath pulse aren't fighting 30 Hz rev-strip or
    // visualizer writes.
    bool IsHandshakeActive();

    // Fire a one-shot directional jolt on the wheel. `lateralKick` is signed
    // in [-1..+1] — negative = kick toward the left, positive = toward the
    // right. `durationMs` controls the decay window; the effect overlays on
    // the normal active torque for that many ms with a linear falloff.
    //
    // Called from redscript collision / bump event wrappers (see
    // gwheel_reds/gwheel_events.reds). Jolts only play during active
    // driving; they're ignored when FFB is disabled, airborne, or stopped.
    void TriggerJolt(float lateralKick, int durationMs);

    // Set the baseline road-surface SINE magnitude (0..1) driven by the
    // current material under the player vehicle (redscript raycast
    // polling). This is combined via max() with the suspension-activity
    // envelope inside UpdateCenteringSpring — smooth asphalt has a
    // baseline of 0 (suspension activity drives everything), textured
    // surfaces (gravel / dirt / metal grates) maintain a constant hum
    // even when the chassis isn't being physically agitated.
    void SetSurfaceBaselineMag(float mag);

    // suspensionActivity drives the road-surface SINE effect. It's the
    // magnitude of the roll+pitch components of the vehicle's angular-
    // velocity derivative |Δω_x|+|Δω_y| (yaw excluded — that comes from
    // steering input, not the road). Smooth asphalt ≈ 0 so the wheel goes
    // silent; cobbles / offroad / speedbumps spike the signal and the
    // wheel vibrates proportionally. Caller computes the derivative each
    // tick by diffing against the previous angular-velocity read.
    //
    // Per-tick FFB output, composed from authored TweakDB physics + live
    // state. Replaces the legacy heuristic UpdateCenteringSpring. The
    // PerCarPhysics snapshot supplies authored values (slipAngleCurveScale,
    // forwardWeightTransferFactor, turnUpdate* curve, friction multipliers,
    // chassis mass, maxWheelTurnDeg, etc.); the FfbDriverState supplies
    // per-tick driver-input + computed state (merged steer, throttle/brake,
    // ground / reverse flags, suspension activity, lateral velocity).
    //
    // The function bails (tearing down all effects) when:
    //   - scalars.ffbEnabled == false (master toggle)
    //   - car.kind == ChassisKind::Unknown (out-of-scope chassis like tanks)
    //   - car.chainResolved == false (mount race; retry next tick)
    //   - car.liveStateValid == false
    //   - speed < scalars.stationaryMps (parked / stationary)
    // Airborne (!isOnGround) plays the SDK's airborne effect and tears down
    // contact forces. ffbInvertDirection multiplies all restoring forces by
    // -1 at the final stage so users with reverse-polarity wheel hardware
    // can flip with one toggle.
    struct FfbUserScalars
    {
        bool  ffbEnabled         = false;
        bool  ffbInvertDirection = false;
        bool  ffbOutputSignFlip  = false; // hardware-quirk flip; gates the final constForce sign at the SDK boundary
        bool  ffbUseSpring       = true;  // when false, skips LogiPlaySpringForce (BeamNG-style pipeline)
        int   torquePct          = 100;   // 0..100; reserved for future global multiplier hook
        int   yawFeedbackPct     = 100;   // 0..100; multiplier on the spring component
        int   activeTorquePct    = 100;   // 0..100; multiplier on the SAT + countersteer
        float stationaryMps      = 0.5f;
        bool  debugLogging       = false;
    };

    struct FfbDriverState
    {
        float steerNormalized    = 0.f;
        float throttle           = 0.f;
        float brake              = 0.f;
        bool  isReversing        = false;
        bool  isOnGround         = true;
        float suspensionActivity = 0.f;   // rad-per-tick proxy (existing pipeline)
        float lateralVelocityMps = 0.f;
    };
}

namespace gwheel::tweakdb_physics { struct PerCarPhysics; }

namespace gwheel::wheel
{
    void ApplyFfbFromPhysics(const gwheel::tweakdb_physics::PerCarPhysics& car,
                             const FfbDriverState&                         driver,
                             const FfbUserScalars&                         scalars);
}
