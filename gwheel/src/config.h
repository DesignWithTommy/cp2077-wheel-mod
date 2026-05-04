#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace gwheel::config
{
    struct Input
    {
        bool        enabled = true;

        // Center deadzone applied only to signed-axis bindings (steer).
        // Wheel hardware reports a small non-zero center offset at rest
        // (1-3% on most Logitech models), which without filtering causes
        // a constant tiny steering input -> the car drifts even with hands
        // off the wheel. Stored as a percent (0..15) so the mod settings
        // slider can show 0%-15%; converted to a 0..0.15 fraction inside
        // axis_resolver. Zero disables the deadzone entirely.
        int32_t     steerDeadzonePct = 2;

        // Response curve applied to signed-axis bindings (steer) AFTER
        // the deadzone:
        //   "default"  : linear pass-through.
        //   "subdued"  : convex (|v|^1.7), softer near center, full at lock.
        //               Helps drift-prone or low-speed cruising.
        //   "sharp"    : concave (|v|^0.6), sharper near center, full at lock.
        //               Helps tight handling at the cost of twitchiness.
        std::string responseCurve = "default";
    };

    struct Ffb
    {
        bool    enabled = true;
        bool    debugLogging = false;

        // Wheel torque, pushed to G HUB via LogiSetPreferredControllerProperties
        // (overallGain). When "Apply Settings from Game" is checked in G HUB
        // (the default), G HUB greys out its own Torque slider and expects
        // the game to drive this value. When unchecked, G HUB ignores our
        // Set and uses its slider value. 100 = full, 0 = off.
        int32_t torquePct = 100;

        // Physics-model self-centering. Wheel is free at rest; spring engages
        // and active torque builds with speed². Shape + cruise speed + spring
        // baseline are derived per-car from WheeledPhysics. The knobs below
        // are the user-facing taste scalars layered on top:
        //
        // stationaryThresholdMps: below this, all centering forces are off
        //   so the wheel rests wherever the driver leaves it.
        // yawFeedbackPct: additive spring-stiffness bonus during rotation,
        //   normalised against the car's own turnRate. Pure preference.
        // activeTorqueStrengthPct: 0..100 gain on the directional push-back
        //   constant force. Peak is shaped (humped sqrt curve over deflection,
        //   lateral-accel proxy, grip-factor lightening past the yaw limit).
        float   stationaryThresholdMps  = 0.5f;
        int32_t yawFeedbackPct          = 50;
        int32_t activeTorqueStrengthPct = 100;

        // Flip the sign of all restoring forces (centering spring + active
        // torque + slip-angle countersteer). Some wheels invert their
        // FFB-direction convention against the SDK's; this is the user
        // escape hatch for that case. Off = standard SDK convention.
        bool    invertDirection = false;

        // Per-wheel hardware-direction quirk: some Logitech wheels (verified
        // on a G923 2026-05-01) interpret `LogiPlayConstantForce(idx, +pct)`
        // as a counter-clockwise push, opposite to the SDK's typically-
        // documented convention (positive = CW). Symptom: with the spring
        // disabled, constForce alone drives the wheel to full lock instead
        // of pulling it to center. Enable this toggle if your wheel exhibits
        // that behavior. Default off so wheels with the documented convention
        // are unaffected.
        bool    outputSignFlip = false;

        // User-facing label: "Speed-sensitive stiffness" (a sportscar-style
        // steering-assist feel). Mechanism: toggles the LogiPlaySpringForce
        // centering layer on top of the SAT-driven LogiPlayConstantForce.
        //
        // On (default): the spring saturates with speed², adding the firm
        // "steering tightens at highway speed" feel. This is what cars with
        // electronic power steering do (Lexus, BMW Servotronic, etc.).
        //
        // Off: BeamNG-style pure tire-force pipeline — only LogiPlayConstantForce
        // drives the wheel. Matches what physics-honest sims do. Wheel goes
        // light at rest and feels more raw at speed; some Logitech hardware
        // also exhibits a "stuck at deflection" quirk where the SDK spring
        // cancels the constant-force centering, which this option avoids.
        bool    useSpring = true;
    };

    struct PerVehicle
    {
        float steeringMultiplier = 1.0f;
        int32_t responseDelayMs = 20;
    };

    struct Handshake
    {
        // Play the gwheel handshake (LED sweep → 4 triplets + synced LED
        // flashes → centering spring with LED breath pulse) on wheel
        // connect. Installer-level choice; persisted to config.json so
        // it survives restarts until changed.
        bool playOnStart = true;
    };

    struct Led
    {
        // Master toggle for the wheel's rev-strip LEDs (G29/G920/G923 etc.).
        // When off, the plugin never calls LogiPlayLeds — G HUB's own
        // profile drives them (or they stay dark).
        bool enabled = true;

        // When true AND system audio is playing, the LED bar tracks a
        // dynamic-range-normalised audio envelope instead of vehicle
        // speed. Silence (below the WASAPI loopback noise gate) falls
        // back to rev-strip automatically.
        bool visualizerWhileMusic = true;
    };

    struct TrueForce
    {
        // Master enable for the TrueForce vibrotactile channel
        // (G923 / G Pro and successors). Off by default; the FOMOD
        // installer ships a config.json with this set to true for
        // Vortex users, manual installers opt in by editing the file.
        //
        // Even when true, the runtime probe still has to find a
        // usable trueforce_sdk_x64.dll on the user's machine
        // (provided by their existing G HUB install). If the probe
        // fails, the feature silently disables itself and emits a
        // single explanatory line to gwheel-*.log. We never bundle
        // or redistribute the DLL; the user's licensed copy is the
        // only one in play.
        bool enabled = false;

        // Independent opt-in for the TF (vibrotactile) audio-stream
        // channel. Default false even when `enabled` is true, so users
        // can validate the TrueForce backend's kinesthetic path
        // (torque + damping + LEDs through the modern SDK) before
        // committing to the higher-risk TF synthesis. When true and the
        // active backend reports caps.vibrotactile, the TF pipeline
        // (wheel_tf_pipeline.cpp) feeds engine RPM + suspension impacts
        // + skid texture into logiTrueForceSetTorqueTFfloat each tick.
        bool feedSignals = false;
    };

    struct Music
    {
        // Target process name for per-process audio capture. Empty =
        // capture the game's own process tree (gwheel.dll lives
        // inside Cyberpunk2077.exe, so we use GetCurrentProcessId).
        // This isolates the visualizer to just the game's audio
        // (radio + engine + SFX) and rejects anything else on the
        // system — Spotify tabs, Discord, browser audio — that
        // otherwise contaminated the system-wide loopback.
        //
        // Non-empty = an advanced override that points the loopback
        // at some OTHER app instead, for setups where the user runs
        // a separate music source alongside the game.
        // Examples: "Spotify.exe", "firefox.exe", "chrome.exe".
        // Case-insensitive match on the executable filename. If the
        // named process isn't running when the plugin starts, we
        // fall back to system-wide loopback with a warning.
        std::string processName;
    };

    struct Config
    {
        int32_t     version = 6;
        Input       input;
        Ffb         ffb;
        Handshake   handshake;
        Led         led;
        Music       music;
        TrueForce   trueForce;
        PerVehicle  car        = { 1.0f, 20 };
        PerVehicle  motorcycle = { 1.2f, 10 };
        PerVehicle  truck      = { 0.8f, 40 };
        PerVehicle  van        = { 0.9f, 30 };

        // Action-keyed binding strings. Key is an action name
        // ("SteerAxis", "ThrottleAxis", "BrakeAxis", "Horn", ...);
        // value is a `|`-delimited list of tagged tokens emitted by the
        // Mod Settings InputBinding primitive (e.g. "wheel-axis:lY",
        // "wheel:5|keyboard:KEY_H"). Empty string = unbound. Tabula-rasa
        // on first launch — no migration of v4 `bindings[]`.
        std::unordered_map<std::string, std::string> actionBindings;
    };

    // Read the published snapshot. Non-blocking; safe from any thread.
    Config Current();

    // Load config.json from the plugin's install dir. Falls back to defaults
    // if the file doesn't exist or fails to parse (a warning is logged in
    // that case). Idempotent; safe to call multiple times.
    void Load();

    // Serialize the current snapshot as a JSON string. Used by redscript at
    // Settings-page init time to hydrate the page with the persisted values.
    std::string ReadAsJson();

    // Per-field setters. Each one swaps the snapshot atomically and writes
    // the updated config back to disk.
    void SetInputEnabled(bool v);
    void SetSteerDeadzonePct(int32_t v);
    void SetResponseCurve(std::string_view v);

    void SetFfbEnabled(bool v);
    void SetFfbDebugLogging(bool v);
    void SetFfbTorquePct(int32_t v);

    void SetStationaryThresholdMps(float v);
    void SetYawFeedbackPct(int32_t v);
    void SetActiveTorqueStrengthPct(int32_t v);
    void SetFfbInvertDirection(bool v);
    void SetFfbOutputSignFlip(bool v);
    void SetFfbUseSpring(bool v);

    void SetHandshakePlayOnStart(bool v);

    void SetLedEnabled(bool v);
    void SetLedVisualizerWhileMusic(bool v);

    void SetMusicProcessName(std::string_view v);

    void SetTrueForceEnabled(bool v);
    void SetTrueForceFeedSignals(bool v);

    // Action binding setter. `name` is the action key (e.g. "SteerAxis");
    // `value` is the delimited tagged-token string from the InputBinding
    // primitive. Empty value erases the entry. Persists to config.json
    // and pushes the new binding into axis_resolver immediately.
    void SetActionBinding(std::string_view name, std::string_view value);
}
