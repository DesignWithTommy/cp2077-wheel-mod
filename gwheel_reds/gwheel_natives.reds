// Declarations for native functions registered by gwheel.dll.
// Keep this file in sync with gwheel/src/rtti.cpp::PostRegisterTypes.

public static native func GWheel_GetVersion() -> String;
public static native func GWheel_IsPluginReady() -> Bool;
public static native func GWheel_GetDeviceInfo() -> String;
public static native func GWheel_HasFFB() -> Bool;
public static native func GWheel_ReadConfig() -> String;

// Wheel-model auto-discovery. Read at OnGameAttached to populate dependency
// targets that hide controls the bound wheel doesn't physically have.
// Permissive default (true / "(no wheel bound)") when the SDK hasn't bound
// yet, so the settings page shows everything until detection is ready.
public static native func GWheel_DetectedHasRightCluster() -> Bool;
public static native func GWheel_DetectedHasFfbHardware() -> Bool;
public static native func GWheel_DetectedHasRevLeds() -> Bool;
public static native func GWheel_DetectedModelName() -> String;

public static native func GWheel_SetInputEnabled(v: Bool) -> Bool;

// Steer-only input shaping. Deadzone is a percent in [0..15] applied to
// the centered wheel value before any curve; the wheel range is rescaled
// so a small deadzone doesn't lose you any sensitivity at full lock.
// ResponseCurve is "default" (linear), "subdued" (convex, soft center),
// or "sharp" (concave, sharp center). Pedals are unaffected.
public static native func GWheel_SetSteerDeadzonePct(pct: Int32) -> Bool;
public static native func GWheel_SetResponseCurve(v: String) -> Bool;

public static native func GWheel_SetFfbEnabled(v: Bool) -> Bool;
public static native func GWheel_SetFfbDebugLogging(v: Bool) -> Bool;
public static native func GWheel_SetFfbTorquePct(pct: Int32) -> Bool;

// Startup handshake (LED sweep + 4 triplets + centering breath) on wheel bind.
// Effective at next wheel bind — toggling mid-session does not retroactively
// play or cancel a handshake that already fired this session.
public static native func GWheel_SetHandshakePlayOnStart(v: Bool) -> Bool;

// Phase-1 physics FFB: speed-gated self-centering spring with yaw-rate bonus.
public static native func GWheel_SetStationaryThresholdMps(mps: Float) -> Bool;
public static native func GWheel_SetYawFeedbackPct(pct: Int32) -> Bool;
public static native func GWheel_SetActiveTorqueStrengthPct(pct: Int32) -> Bool;
public static native func GWheel_SetFfbInvertDirection(v: Bool) -> Bool;
public static native func GWheel_SetFfbOutputSignFlip(v: Bool) -> Bool;
public static native func GWheel_SetFfbUseSpring(v: Bool) -> Bool;

// Rev-strip LED bar on top of the wheel (G29/G920/G923). VisualizerWhileMusic
// swaps the speed-driven rev bar for a WASAPI-loopback audio visualizer
// whenever system audio is playing; falls back to rev-strip on silence.
public static native func GWheel_SetLedEnabled(v: Bool) -> Bool;
public static native func GWheel_SetLedVisualizerWhileMusic(v: Bool) -> Bool;

// Forwarder from redscript to the gwheel-*.log file. CP2077's native
// LogChannel writes to a debug channel that has no sink in retail builds;
// this native bridges through gwheel's spdlog logger so reds-side
// diagnostics actually show up on disk. Called from anywhere in
// redscript that needs runtime-traceable output.
public static native func GWheel_Log(msg: String) -> Bool;

// Action-keyed binding setter (Phase 6 schema). `name` is an action key
// like "SteerAxis" / "ThrottleAxis" / "BrakeAxis" / "Horn"; `value` is a
// `|`-delimited list of tagged tokens emitted by the InputBinding
// primitive (e.g. "wheel-axis:lY", "wheel:5|keyboard:KEY_H"). Empty
// value erases the entry. Persists to config.json and updates the
// runtime axis resolver immediately.
public static native func GWheel_SetActionBinding(name: String, value: String) -> Bool;

// Tracks the player's currently-mounted vehicle. The plugin's vehicle-
// input detour fires for every visible vehicle each tick; without this
// filter, our steering/throttle/brake writes would propagate to all of
// them (remote-driving parked cars, etc.). Call SetPlayerVehicle on
// mount and ClearPlayerVehicle on dismount from VehicleComponent event
// wrappers.
public static native func GWheel_SetPlayerVehicle(v: ref<VehicleObject>) -> Bool;
public static native func GWheel_ClearPlayerVehicle() -> Bool;

// Vehicle telemetry pushed from Blackboard listeners (gwheel_vehicle_signals.reds).
// Engine RPM is sent in raw vehicle units (typically 800..7100 for cars);
// the LED rev strip combines it with a per-gear shift point pushed via
// GWheel_SetEngineProfile so the bar saturates at the AI's actual upshift
// instead of at global redline. Radio state mirrors VehicleDef.VehRadioState.
public static native func GWheel_SetEngineRpmRaw(v: Float) -> Bool;
public static native func GWheel_SetRadioActive(v: Bool) -> Bool;

// Per-mount engine profile, pushed once from GWheelAttach after walking
// VehicleEngineData_Record. idleRpm = MinRPM(), redlineRpm = MaxRPM(),
// perGearShiftRpms[i] = gears[i].MaxEngineRPM() in forward-gear order
// (index 0 = first gear). The LED tick reads ShiftRpmForGear(currentGear)
// every frame; out-of-range gears (neutral / reverse) fall back to redline.
public static native func GWheel_SetEngineProfile(idleRpm: Float, redlineRpm: Float, perGearShiftRpms: array<Float>) -> Bool;
public static native func GWheel_ClearEngineProfile() -> Bool;

// Current transmission gear from VehicleDef.GearValue. 0=neutral,
// positive=forward gear N, negative=reverse.
public static native func GWheel_SetCurrentGear(gear: Int32) -> Bool;

// Collision / bump feedback. lateralKick is the world-space hit direction
// dotted with the vehicle's right vector, signed in [-1..+1] (negative =
// struck on left). The plugin filters out events from non-player vehicles
// using the handle; we still forward every event for simplicity.
public static native func GWheel_OnVehicleBump(v: ref<VehicleObject>, lateralKick: Float) -> Bool;
public static native func GWheel_OnVehicleHit(v: ref<VehicleObject>, lateralKick: Float) -> Bool;

// Per-wheel road material report. Called from the redscript raycast
// poller (see gwheel_surface.reds) when a wheel's material CName
// changes. wheelIdx is 0..3, material is the physics material CName
// (e.g. `asphalt`, `dirt`, `metal`). C++ logs the transition and
// eventually drives surface-aware FFB off the category.
public static native func GWheel_OnWheelMaterial(wheelIdx: Int32, material: CName) -> Bool;

// Capture-mode bridge for Mod Settings' InputBinding listening UI.
// BeginCapture arms the plugin to watch wheel inputs; PollCapturedInput
// returns the empty string until a qualifying input arrives, then returns
// a tagged token like "wheel:5" / "wheel-axis:lY" / "wheel-pov:18000" and
// disarms in the same call. CancelCapture aborts without committing.
// typeHint = "button" or "axis"; unknown values default to "button".
public static native func GWheel_BeginCapture(typeHint: String) -> Bool;
public static native func GWheel_PollCapturedInput() -> String;
public static native func GWheel_CancelCapture() -> Bool;
