// In-game settings page for the direct_wheel mod.
//
// Single DirectWheelSettings class. Categories (`ModSettings.category` +
// `ModSettings.category.order`) provide visual grouping on the scrolling
// settings panel.
//
// Capability auto-detection (how the section-hide actually works):
//
//   1. Three hidden Bool fields (hasFfbHardware, hasRevLeds, hasRightCluster)
//      carry the runtime-property `ModSettings.hidden = "true"`. With our
//      patched mod_settings build they are excluded from the UI list but
//      still registered as variables.
//
//   2. Other settings reference these fields via `ModSettings.dependency`.
//      mod_settings hides the dependent setting whenever the dependency
//      target's stored value is not "true".
//
//   3. mod_settings reads its stored values from
//      `red4ext/plugins/mod_settings/user.ini` once at process start. So to
//      drive the three capability flags from real hardware state we have to
//      write that file BEFORE mod_settings reads it. That happens in
//      direct_wheel.dll's RED4ext OnLoad - see direct_wheel/src/mod_settings_seed.cpp.
//      Plugin OnLoads complete before script-data processing, so the
//      ordering is safe regardless of plugin load order.
//
//   4. Important consequence: the UI reflects the wheel state captured at
//      the moment of process start. Plug or unplug the wheel = restart the
//      game once for the section list to catch up.
//
// `ModSettings.hidden` is an experimental property. Upstream mod_settings
// currently ignores it and the three capability flags will render as visible
// Bool toggles, but this property is kept in case future versions support it.
//
// mod_settings dependency evaluation reads from its OWN internal
// RuntimeVariable<T> storage, NOT from the instance passed to
// RegisterListenerToClass. Setting capability flags on the listener
// instance from script does nothing for visibility - the only way to
// drive them is via user.ini (above). Verified against mod_settings
// source 2026-04-25.

public class DirectWheelSettings extends IScriptable {

  // ---- Hidden capability flags (auto-set in OnGameAttached) --------------
  //
  // ModSettings.hidden = "true" is meant to make these invisible in the settings UI.
  // Plain upstream mod_settings ignores this, so these render as visible toggles.
  // Other fields reference these as `ModSettings.dependency` targets to hide
  // entire sections on wheels without the relevant hardware.

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.hidden", "true")
  let hasFfbHardware: Bool = true;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.hidden", "true")
  let hasRevLeds: Bool = true;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.hidden", "true")
  let hasRightCluster: Bool = true;

  // ---- Wheel input --------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Wheel input")
  @runtimeProperty("ModSettings.category.order", "100")
  @runtimeProperty("ModSettings.displayName", "Enable wheel input")
  let inputEnabled: Bool = true;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Wheel input")
  @runtimeProperty("ModSettings.category.order", "100")
  @runtimeProperty("ModSettings.displayName", "Treat clutch as brake")
  @runtimeProperty("ModSettings.description", "The clutch acts just like the brake pedal. Useful for stiff brake pedals, since there's no gear system in the game.")
  @runtimeProperty("ModSettings.dependency", "inputEnabled")
  let clutchAsBrake: Bool = true;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Wheel input")
  @runtimeProperty("ModSettings.category.order", "100")
  @runtimeProperty("ModSettings.displayName", "Invert steering")
  @runtimeProperty("ModSettings.dependency", "inputEnabled")
  let invertSteering: Bool = false;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Wheel input")
  @runtimeProperty("ModSettings.category.order", "100")
  @runtimeProperty("ModSettings.displayName", "Invert throttle pedal")
  @runtimeProperty("ModSettings.dependency", "inputEnabled")
  let invertThrottle: Bool = false;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Wheel input")
  @runtimeProperty("ModSettings.category.order", "100")
  @runtimeProperty("ModSettings.displayName", "Invert brake pedal")
  @runtimeProperty("ModSettings.dependency", "inputEnabled")
  let invertBrake: Bool = false;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Wheel input")
  @runtimeProperty("ModSettings.category.order", "100")
  @runtimeProperty("ModSettings.displayName", "Invert clutch pedal")
  @runtimeProperty("ModSettings.dependency", "inputEnabled")
  let invertClutch: Bool = false;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Wheel input")
  @runtimeProperty("ModSettings.category.order", "100")
  @runtimeProperty("ModSettings.displayName", "Steering deadzone (degrees)")
  @runtimeProperty("ModSettings.description", "Dead zone in the center of the wheel (0-45°). On a 900° wheel, 10° ≈ 2% deadzone, 45° ≈ 10%.")
  @runtimeProperty("ModSettings.min", "0.0")
  @runtimeProperty("ModSettings.max", "45.0")
  @runtimeProperty("ModSettings.step", "1.0")
  @runtimeProperty("ModSettings.dependency", "inputEnabled")
  let steeringDeadzoneDegrees: Float = 0.0;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Wheel input")
  @runtimeProperty("ModSettings.category.order", "100")
  @runtimeProperty("ModSettings.displayName", "Speed steering boost (%)")
  @runtimeProperty("ModSettings.description", "Compensates for reduced steering at high speed by acting on the Equalizer curve. 0 = off, 50 = 1.5x at 100 mph, 100 = 2x max.")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "1")
  @runtimeProperty("ModSettings.dependency", "inputEnabled")
  let speedSensitiveSteeringPct: Int32 = 45;





  // Legacy equalizer control points — replaced by automatic sqrt curve
  // (v2.35.0+). The sqrt inverse perfectly linearises the game's quadratic
  // steering response, making manual tuning unnecessary. Hidden from the UI
  // but kept for backward compat with saved user.ini files.

  let steeringCurve25: Int32 = 50;
  let steeringCurve50: Int32 = 71;
  let steeringCurve75: Int32 = 86;

  // ---- Axis mapping -------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Axis mapping")
  @runtimeProperty("ModSettings.category.order", "150")
  @runtimeProperty("ModSettings.displayName", "Wheel device (0 = Auto)")
  @runtimeProperty("ModSettings.description", "Which controller to use for steering + FFB. 0 = auto-detect, 1..N = specific device. Check the log for device numbers.")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "8")
  @runtimeProperty("ModSettings.step", "1")
  let wheelDeviceIndex: Int32 = 0;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Axis mapping")
  @runtimeProperty("ModSettings.category.order", "150")
  @runtimeProperty("ModSettings.displayName", "Pedal device (0 = Same as wheel)")
  @runtimeProperty("ModSettings.description", "Separate USB pedal device for throttle/brake. 0 = use wheel device, 1..N = specific device. Check the log for device numbers.")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "8")
  @runtimeProperty("ModSettings.step", "1")
  let pedalDeviceIndex: Int32 = 0;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Axis mapping")
  @runtimeProperty("ModSettings.category.order", "150")
  @runtimeProperty("ModSettings.displayName", "Throttle axis")
  @runtimeProperty("ModSettings.description", "Which DirectInput axis to read as throttle. Default: lZ (Logitech). Moza pedals often use lY.")
  let throttleAxis: DirectWheelAxis = DirectWheelAxis.lZ;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Axis mapping")
  @runtimeProperty("ModSettings.category.order", "150")
  @runtimeProperty("ModSettings.displayName", "Brake axis")
  @runtimeProperty("ModSettings.description", "Which DirectInput axis to read as brake. Default: lRz (Logitech). Some pedals use lRx.")
  let brakeAxis: DirectWheelAxis = DirectWheelAxis.lRz;

  // ---- Force feedback - General -------------------------------------------

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Force feedback")
  @runtimeProperty("ModSettings.category.order", "200")
  @runtimeProperty("ModSettings.displayName", "Enable force feedback")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let ffbEnabled: Bool = true;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Force feedback")
  @runtimeProperty("ModSettings.category.order", "200")
  @runtimeProperty("ModSettings.displayName", "FFB strength (%)")
  @runtimeProperty("ModSettings.description", "Master gain applied to ALL force feedback effects.")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "1")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let ffbTorquePct: Int32 = 100;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Force feedback")
  @runtimeProperty("ModSettings.category.order", "200")
  @runtimeProperty("ModSettings.displayName", "Stationary threshold (m/s)")
  @runtimeProperty("ModSettings.description", "Below this speed, all centering forces are OFF so the wheel rests wherever you leave it.")
  @runtimeProperty("ModSettings.min", "0.0")
  @runtimeProperty("ModSettings.max", "5.0")
  @runtimeProperty("ModSettings.step", "0.1")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let stationaryThresholdMps: Float = 0.5;

  // ---- FFB - Centering (Spring) ------------------------------------------
  //
  // These three sliders all feed into the DirectInput Spring effect.
  // Together they control how strongly the wheel pulls back to center.

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "FFB - Centering (Spring)")
  @runtimeProperty("ModSettings.category.order", "210")
  @runtimeProperty("ModSettings.displayName", "Spring Force (%)")
  @runtimeProperty("ModSettings.description", "Base centering spring - pulls the wheel back to center. The foundation of self-aligning feel.")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "1")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let springForcePct: Int32 = 60;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "FFB - Centering (Spring)")
  @runtimeProperty("ModSettings.category.order", "210")
  @runtimeProperty("ModSettings.displayName", "Constant Force - centering addition (%)")
  @runtimeProperty("ModSettings.description", "Smooth push-back force that adds to the spring. Grows with speed and steering angle - simulates self-aligning torque (SAT).")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "1")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let constantForcePct: Int32 = 50;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "FFB - Centering (Spring)")
  @runtimeProperty("ModSettings.category.order", "210")
  @runtimeProperty("ModSettings.displayName", "Cornering feedback - spring stiffness (%)")
  @runtimeProperty("ModSettings.description", "Makes the spring stiffer while the car is rotating (yaw). More = heavier wheel in corners.")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "1")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let yawFeedbackPct: Int32 = 15;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "FFB - Centering (Spring)")
  @runtimeProperty("ModSettings.category.order", "210")
  @runtimeProperty("ModSettings.displayName", "Spring Non-Linearity (%)")
  @runtimeProperty("ModSettings.description", "Makes the spring force ramp up faster at small steering angles. Useful for weak wheels to feel the center better. 0 = linear, 100 = highly non-linear.")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "1")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let springNonLinearityPct: Int32 = 0;

  // ---- FFB - Steering feel ------------------------------------------------

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "FFB - Steering feel")
  @runtimeProperty("ModSettings.category.order", "220")
  @runtimeProperty("ModSettings.displayName", "Damper - rotation resistance (%)")
  @runtimeProperty("ModSettings.description", "Viscous damping - resists wheel rotation speed. More = heavier, slower steering.")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "1")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let damperForcePct: Int32 = 45;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "FFB - Steering feel")
  @runtimeProperty("ModSettings.category.order", "220")
  @runtimeProperty("ModSettings.displayName", "Active Torque - yaw push (%)")
  @runtimeProperty("ModSettings.description", "Directional push during turns - simulates lateral force feedback. Uses Constant Force (short pulses).")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "1")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let activeTorqueStrengthPct: Int32 = 45;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "FFB - Steering feel")
  @runtimeProperty("ModSettings.category.order", "220")
  @runtimeProperty("ModSettings.displayName", "Friction - road texture (%)")
  @runtimeProperty("ModSettings.description", "Static friction that simulates road grip. More = more textured feel on different surfaces.")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "1")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let frictionForcePct: Int32 = 25;

  // ---- FFB - Road effects -------------------------------------------------

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "FFB - Road effects")
  @runtimeProperty("ModSettings.category.order", "230")
  @runtimeProperty("ModSettings.displayName", "Road vibration - surface buzz (%)")
  @runtimeProperty("ModSettings.description", "25 Hz periodic vibration from road roughness. Always present at speed with a baseline hum.")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "1")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let sineForcePct: Int32 = 10;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "FFB - Road effects")
  @runtimeProperty("ModSettings.category.order", "230")
  @runtimeProperty("ModSettings.displayName", "Collision jolt - impact kick (%)")
  @runtimeProperty("ModSettings.description", "Short high-magnitude pulse on crashes and obstacle hits. Uses Constant Force (80-300ms burst).")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "1")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let joltForcePct: Int32 = 40;

  // ---- Rev-strip LEDs -----------------------------------------------------

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Rev-strip LEDs")
  @runtimeProperty("ModSettings.category.order", "300")
  @runtimeProperty("ModSettings.displayName", "Enable rev-strip LEDs")
  @runtimeProperty("ModSettings.dependency", "hasRevLeds")
  let ledEnabled: Bool = true;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Rev-strip LEDs")
  @runtimeProperty("ModSettings.category.order", "300")
  @runtimeProperty("ModSettings.displayName", "Rev strip as visualizer while music is playing")
  @runtimeProperty("ModSettings.dependency", "hasRevLeds")
  let ledVisualizerWhileMusic: Bool = true;

  // ---- Button bindings (15 controls present on every G-series wheel) -----
  //
  // Every binding here is user-controlled, with no hidden overrides. The
  // D-pad + A defaults are Menu-nav (Up/Down/Left/Right arrow keys and
  // Enter) so the wheel navigates pause/map/inventory menus like a
  // controller out of the box. If you'd rather those wheel controls stay
  // inert while driving, set them to None - CP2077's arrow keys are
  // secondary vehicle controls (Up/Down = accelerate/decelerate, Left/Right
  // = steer), so binding the D-pad to Menu-nav and then pressing the D-pad
  // while driving will nudge the car.
  //
  // IMPORTANT: clear the D-pad and A/B/X/Y keyboard bindings in G HUB's
  // Cyberpunk profile. Otherwise G HUB + plugin both fire keyboard
  // events and you'll get doubled keypresses.

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "Left paddle shifter")
  let bindPaddleLeft: DirectWheelAction = DirectWheelAction.Flight_LiftDown;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "Right paddle shifter")
  let bindPaddleRight: DirectWheelAction = DirectWheelAction.Flight_LiftUp;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "D-pad Up")
  let bindDpadUp: DirectWheelAction = DirectWheelAction.Flight_PitchForward;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "D-pad Down")
  let bindDpadDown: DirectWheelAction = DirectWheelAction.Flight_PitchBackward;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "D-pad Left")
  let bindDpadLeft: DirectWheelAction = DirectWheelAction.Flight_RollLeft;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "D-pad Right")
  let bindDpadRight: DirectWheelAction = DirectWheelAction.Flight_RollRight;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "A button")
  let bindButtonA: DirectWheelAction = DirectWheelAction.MenuConfirm;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "B button (in-vehicle)")
  let bindButtonB: DirectWheelAction = DirectWheelAction.Handbrake;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "X button (in-vehicle)")
  let bindButtonX: DirectWheelAction = DirectWheelAction.Horn;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "Y button (in-vehicle)")
  let bindButtonY: DirectWheelAction = DirectWheelAction.Autodrive;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "Start button")
  let bindStart: DirectWheelAction = DirectWheelAction.Pause;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "Select / View button")
  let bindSelect: DirectWheelAction = DirectWheelAction.OpenMap;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "LSB (left stick click)")
  let bindLSB: DirectWheelAction = DirectWheelAction.OpenPhone;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "RSB (right stick click)")
  let bindRSB: DirectWheelAction = DirectWheelAction.RadioMenu;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Button bindings")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "Xbox / Guide button")
  let bindXbox: DirectWheelAction = DirectWheelAction.ExitVehicle;

  // ---- Lower-cluster bindings (G29 / G923 / G PRO; absent on G920) -------

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Lower-cluster bindings")
  @runtimeProperty("ModSettings.category.order", "500")
  @runtimeProperty("ModSettings.displayName", "Plus (+) button")
  @runtimeProperty("ModSettings.dependency", "hasRightCluster")
  let bindPlus: DirectWheelAction = DirectWheelAction.CameraCycleForward;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Lower-cluster bindings")
  @runtimeProperty("ModSettings.category.order", "500")
  @runtimeProperty("ModSettings.displayName", "Minus (-) button")
  @runtimeProperty("ModSettings.dependency", "hasRightCluster")
  let bindMinus: DirectWheelAction = DirectWheelAction.Headlights;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Lower-cluster bindings")
  @runtimeProperty("ModSettings.category.order", "500")
  @runtimeProperty("ModSettings.displayName", "Scroll click (Return)")
  @runtimeProperty("ModSettings.dependency", "hasRightCluster")
  let bindScrollClick: DirectWheelAction = DirectWheelAction.HolsterWeapon;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Lower-cluster bindings")
  @runtimeProperty("ModSettings.category.order", "500")
  @runtimeProperty("ModSettings.displayName", "Scroll clockwise")
  @runtimeProperty("ModSettings.dependency", "hasRightCluster")
  let bindScrollCW: DirectWheelAction = DirectWheelAction.NextWeapon;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Lower-cluster bindings")
  @runtimeProperty("ModSettings.category.order", "500")
  @runtimeProperty("ModSettings.displayName", "Scroll counter-clockwise")
  @runtimeProperty("ModSettings.dependency", "hasRightCluster")
  let bindScrollCCW: DirectWheelAction = DirectWheelAction.PrevWeapon;
  // ---- Extra Generic Buttons (Moza, Fanatec, Simagic, etc) ---------------

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Extra Buttons (Moza)")
  @runtimeProperty("ModSettings.category.order", "550")
  @runtimeProperty("ModSettings.displayName", "Extra 1 (Moza N)")
  let bindExtra1: DirectWheelAction = DirectWheelAction.None;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Extra Buttons (Moza)")
  @runtimeProperty("ModSettings.category.order", "550")
  @runtimeProperty("ModSettings.displayName", "Extra 2 (Moza WIP)")
  let bindExtra2: DirectWheelAction = DirectWheelAction.None;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Extra Buttons (Moza)")
  @runtimeProperty("ModSettings.category.order", "550")
  @runtimeProperty("ModSettings.displayName", "Extra 3 (Moza FL)")
  let bindExtra3: DirectWheelAction = DirectWheelAction.Flight_Toggle;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Extra Buttons (Moza)")
  @runtimeProperty("ModSettings.category.order", "550")
  @runtimeProperty("ModSettings.displayName", "Extra 4 (Moza CAM)")
  let bindExtra4: DirectWheelAction = DirectWheelAction.None;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Extra Buttons (Moza)")
  @runtimeProperty("ModSettings.category.order", "550")
  @runtimeProperty("ModSettings.displayName", "Extra 5 (Moza S1)")
  let bindExtra5: DirectWheelAction = DirectWheelAction.Flight_ModeSwitchForward;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Extra Buttons (Moza)")
  @runtimeProperty("ModSettings.category.order", "550")
  @runtimeProperty("ModSettings.displayName", "Extra 6 (Moza S2)")
  let bindExtra6: DirectWheelAction = DirectWheelAction.Flight_ModeSwitchBackward;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Extra Buttons (Moza)")
  @runtimeProperty("ModSettings.category.order", "550")
  @runtimeProperty("ModSettings.displayName", "Extra 7 (Moza P)")
  let bindExtra7: DirectWheelAction = DirectWheelAction.Flight_PitchForward;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Extra Buttons (Moza)")
  @runtimeProperty("ModSettings.category.order", "550")
  @runtimeProperty("ModSettings.displayName", "Extra 8 (Moza BOX)")
  let bindExtra8: DirectWheelAction = DirectWheelAction.Flight_LinearBrake;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Extra Buttons (Moza)")
  @runtimeProperty("ModSettings.category.order", "550")
  @runtimeProperty("ModSettings.displayName", "Extra 9 (Moza PL)")
  let bindExtra9: DirectWheelAction = DirectWheelAction.Flight_AngularBrake;

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Extra Buttons (Moza)")
  @runtimeProperty("ModSettings.category.order", "550")
  @runtimeProperty("ModSettings.displayName", "Extra 10")
  let bindExtra10: DirectWheelAction = DirectWheelAction.Flight_PitchBackward;


  // ---- Startup ------------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Startup")
  @runtimeProperty("ModSettings.category.order", "600")
  @runtimeProperty("ModSettings.displayName", "Pon pon shi greeting")
  @runtimeProperty("ModSettings.description", "Make the wheel dance when the game starts.")
  let handshakePlayOnStart: Bool = false;

  // ---- Debug --------------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "Wheel Mod")
  @runtimeProperty("ModSettings.category", "Debug")
  @runtimeProperty("ModSettings.category.order", "700")
  @runtimeProperty("ModSettings.displayName", "Debug logging")
  @runtimeProperty("ModSettings.description", "Logs to red4ext/logs/direct_wheel-*.log.")
  let ffbDebugLogging: Bool = false;

  // ---- Listener callbacks (invoked by Mod Settings, NOT cb funcs) --------

  public func OnModSettingsChange() -> Void {
    this.Push();
  }

  public func Push() -> Void {
    DirectWheel_SetInputEnabled(this.inputEnabled);
    DirectWheel_SetClutchAsBrake(this.clutchAsBrake);
    DirectWheel_SetInvertSteering(this.invertSteering);
    DirectWheel_SetInvertThrottle(this.invertThrottle);
    DirectWheel_SetInvertBrake(this.invertBrake);
    DirectWheel_SetInvertClutch(this.invertClutch);
    DirectWheel_SetSteeringDeadzoneDegrees(this.steeringDeadzoneDegrees);

    DirectWheel_SetFfbEnabled(this.ffbEnabled);
    DirectWheel_SetFfbDebugLogging(this.ffbDebugLogging);
    DirectWheel_SetFfbTorquePct(this.ffbTorquePct);

    DirectWheel_SetStationaryThresholdMps(this.stationaryThresholdMps);
    DirectWheel_SetYawFeedbackPct(this.yawFeedbackPct);
    DirectWheel_SetSpringNonLinearityPct(this.springNonLinearityPct);
    DirectWheel_SetActiveTorqueStrengthPct(this.activeTorqueStrengthPct);
    DirectWheel_SetConstantForcePct(this.constantForcePct);
    DirectWheel_SetSpringForcePct(this.springForcePct);
    DirectWheel_SetDamperForcePct(this.damperForcePct);
    DirectWheel_SetFrictionForcePct(this.frictionForcePct);
    DirectWheel_SetSineForcePct(this.sineForcePct);
    DirectWheel_SetJoltForcePct(this.joltForcePct);
    DirectWheel_SetSpeedSensitiveSteeringPct(this.speedSensitiveSteeringPct);
    DirectWheel_SetSteeringCurve25(this.steeringCurve25);
    DirectWheel_SetSteeringCurve50(this.steeringCurve50);
    DirectWheel_SetSteeringCurve75(this.steeringCurve75);


    DirectWheel_SetHandshakePlayOnStart(this.handshakePlayOnStart);

    DirectWheel_SetLedEnabled(this.ledEnabled);
    DirectWheel_SetLedVisualizerWhileMusic(this.ledVisualizerWhileMusic);

    DirectWheel_SetWheelDeviceIndex(this.wheelDeviceIndex);
    DirectWheel_SetPedalDeviceIndex(this.pedalDeviceIndex);
    DirectWheel_SetAxisThrottle(DirectWheelAxisToString(this.throttleAxis));
    DirectWheel_SetAxisBrake(DirectWheelAxisToString(this.brakeAxis));

    // Input IDs match the PhysicalInput enum in
    // direct_wheel/src/input_bindings.h. D-pad + A/B/X/Y (ids 2-9) are
    // in-vehicle bindings here; the plugin overrides them with
    // gamepad-nav whenever a menu is open.
    DirectWheel_SetInputBinding(0,  EnumInt(this.bindPaddleLeft));
    DirectWheel_SetInputBinding(1,  EnumInt(this.bindPaddleRight));
    DirectWheel_SetInputBinding(2,  EnumInt(this.bindDpadUp));
    DirectWheel_SetInputBinding(3,  EnumInt(this.bindDpadDown));
    DirectWheel_SetInputBinding(4,  EnumInt(this.bindDpadLeft));
    DirectWheel_SetInputBinding(5,  EnumInt(this.bindDpadRight));
    DirectWheel_SetInputBinding(6,  EnumInt(this.bindButtonA));
    DirectWheel_SetInputBinding(7,  EnumInt(this.bindButtonB));
    DirectWheel_SetInputBinding(8,  EnumInt(this.bindButtonX));
    DirectWheel_SetInputBinding(9,  EnumInt(this.bindButtonY));
    DirectWheel_SetInputBinding(10, EnumInt(this.bindStart));
    DirectWheel_SetInputBinding(11, EnumInt(this.bindSelect));
    DirectWheel_SetInputBinding(12, EnumInt(this.bindLSB));
    DirectWheel_SetInputBinding(13, EnumInt(this.bindRSB));
    DirectWheel_SetInputBinding(14, EnumInt(this.bindPlus));
    DirectWheel_SetInputBinding(15, EnumInt(this.bindMinus));
    DirectWheel_SetInputBinding(16, EnumInt(this.bindScrollClick));
    DirectWheel_SetInputBinding(17, EnumInt(this.bindScrollCW));
    DirectWheel_SetInputBinding(18, EnumInt(this.bindScrollCCW));
    DirectWheel_SetInputBinding(19, EnumInt(this.bindXbox));
    DirectWheel_SetInputBinding(20, EnumInt(this.bindExtra1));
    DirectWheel_SetInputBinding(21, EnumInt(this.bindExtra2));
    DirectWheel_SetInputBinding(22, EnumInt(this.bindExtra3));
    DirectWheel_SetInputBinding(23, EnumInt(this.bindExtra4));
    DirectWheel_SetInputBinding(24, EnumInt(this.bindExtra5));
    DirectWheel_SetInputBinding(25, EnumInt(this.bindExtra6));
    DirectWheel_SetInputBinding(26, EnumInt(this.bindExtra7));
    DirectWheel_SetInputBinding(27, EnumInt(this.bindExtra8));
    DirectWheel_SetInputBinding(28, EnumInt(this.bindExtra9));
    DirectWheel_SetInputBinding(29, EnumInt(this.bindExtra10));

    // Trigger a safe device re-enumeration so device index changes take effect.
    // ResetDevices only sets a flag; the pump thread handles the actual reset.
    DirectWheel_ResetDevices();
  }
}

// Actions the plugin knows how to dispatch. Indices must match the Action
// enum in direct_wheel/src/input_bindings.h - same values, same order.
// Grouped by category (driving / camera / combat / weapons / radio /
// gameplay / menus / menu-nav) so Mod Settings' scroll-list shows similar
// actions adjacent. MUST stay in lockstep with the C++ `Action` enum in
// direct_wheel/src/input_bindings.h - same names, same integer values, same
// order. Mod Settings persists each binding by integer value, so any
// reorder shifts users' saved bindings (one-time forced re-bind).
//
// `RearViewCamera` is the redscript-side spelling of CameraCycleBackward;
// the dropdown label inherits the redscript identifier so we use the
// gameplay-meaningful name here even though the C++ side keeps the older
// "Backward" naming for historical reasons.

// DirectInput axis identifiers. These correspond to DIJOYSTATE2 fields.
// The dropdown in Mod Settings will show these names directly.
enum DirectWheelAxis {
  lX = 0,
  lY = 1,
  lZ = 2,
  lRx = 3,
  lRy = 4,
  lRz = 5,
  Slider0 = 6,
  Slider1 = 7,
}

public static func DirectWheelAxisToString(axis: DirectWheelAxis) -> String {
  switch axis {
    case DirectWheelAxis.lX:      return "lX";
    case DirectWheelAxis.lY:      return "lY";
    case DirectWheelAxis.lZ:      return "lZ";
    case DirectWheelAxis.lRx:     return "lRx";
    case DirectWheelAxis.lRy:     return "lRy";
    case DirectWheelAxis.lRz:     return "lRz";
    case DirectWheelAxis.Slider0: return "slider0";
    case DirectWheelAxis.Slider1: return "slider1";
    default:                      return "lX";
  }
}

enum DirectWheelAction {
  None = 0,

  // Driving
  Horn = 1,
  Headlights = 2,
  Handbrake = 3,
  Autodrive = 4,
  ExitVehicle = 5,
  CallVehicle = 6,

  // Camera
  CameraCycleForward = 7,
  RearViewCamera = 8,
  CameraReset = 9,

  // Combat
  ShootPrimary = 10,
  ShootSecondary = 11,
  ShootTertiary = 12,

  // Weapons
  NextWeapon = 13,
  PrevWeapon = 14,
  WeaponSlot1 = 15,
  WeaponSlot2 = 16,
  SwitchWeapons = 17,
  HolsterWeapon = 18,

  // Radio
  RadioMenu = 19,
  RadioNext = 20,

  // Gameplay misc
  UseConsumable = 21,
  IconicCyberware = 22,
  QuickSave = 23,

  // Menus
  OpenMap = 24,
  OpenJournal = 25,
  OpenInventory = 26,
  OpenPhone = 27,
  OpenPerks = 28,
  OpenCrafting = 29,
  Pause = 30,

  // Menu navigation
  MenuConfirm = 31,
  MenuCancel = 32,
  MenuUp = 33,
  MenuDown = 34,
  MenuLeft = 35,
  MenuRight = 36,

  // Flight Controls
  Flight_Toggle = 37,
  Flight_ModeSwitchForward = 38,
  Flight_ModeSwitchBackward = 39,
  Flight_LiftUp = 40,
  Flight_LiftDown = 41,
  Flight_LinearBrake = 42,
  Flight_AngularBrake = 43,
  Flight_PitchForward = 44,
  Flight_PitchBackward = 45,
  Flight_RollLeft = 46,
  Flight_RollRight = 47,
}

// Attach our settings instance to the player puppet so it lives for the
// session and register it as a listener with Mod Settings. The listener
// receives OnModSettingsChange callbacks when the user clicks Apply in the
// settings menu - we use that to push the new values into the C++ plugin
// via the DirectWheel_Set* natives.
//
// The hidden capability flags (hasFfbHardware / hasRevLeds /
// hasRightCluster) are not seeded here. Mod Settings ignores writes to the
// listener instance for dependency / visibility purposes. Those flags are
// driven directly into mod_settings/user.ini from the C++ side at plugin
// load (see direct_wheel/src/mod_settings_seed.cpp).

@addField(PlayerPuppet)
public let m_direct_wheelSettings: ref<DirectWheelSettings>;

@wrapMethod(PlayerPuppet)
protected cb func OnGameAttached() -> Bool {
  let result: Bool = wrappedMethod();
  if !IsDefined(this.m_direct_wheelSettings) {
    this.m_direct_wheelSettings = new DirectWheelSettings();
    ModSettings.RegisterListenerToClass(this.m_direct_wheelSettings);
    ModSettings.RegisterListenerToModifications(this.m_direct_wheelSettings);
    this.m_direct_wheelSettings.Push();
  }
  return result;
}


