// In-game settings page for the gwheel mod.
//
// Single GWheelSettings class. Categories (`ModSettings.category` +
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
//      gwheel.dll's RED4ext OnLoad — see gwheel/src/mod_settings_seed.cpp.
//      Plugin OnLoads complete before script-data processing, so the
//      ordering is safe regardless of plugin load order.
//
//   4. Important consequence: the UI reflects the wheel state captured at
//      the moment of process start. Plug or unplug the wheel = restart the
//      game once for the section list to catch up.
//
// Phase 7 binding model: every player-facing input is a String holding a
// `|`-delimited list of tagged tokens emitted by the InputBinding primitive
// (e.g. "wheel-axis:lY", "wheel:5|keyboard:KEY_H"). Empty string = unbound.
// The C++ side resolves axes via gwheel::axis_resolver against the action
// name; buttons reach C++ via the same SetActionBinding native and will
// drive dispatch once the button-side action-keyed dispatcher lands.
//
// Tabula-rasa: every default is empty. New users bind from scratch via the
// Mod Settings UI; the in-vehicle banner (gwheel_mount.reds, future phase)
// nags them when SteerAxis/ThrottleAxis/BrakeAxis are unbound.

public class GWheelSettings extends IScriptable {

  // ---- Hidden capability flags (auto-set in OnGameAttached) --------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.hidden", "true")
  let hasFfbHardware: Bool = true;

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.hidden", "true")
  let hasRevLeds: Bool = true;

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.hidden", "true")
  let hasRightCluster: Bool = true;

  // ---- Wheel input --------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Wheel input")
  @runtimeProperty("ModSettings.category.order", "100")
  @runtimeProperty("ModSettings.displayName", "Enable wheel input")
  let inputEnabled: Bool = true;

  // ---- Force feedback -----------------------------------------------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Force feedback")
  @runtimeProperty("ModSettings.category.order", "200")
  @runtimeProperty("ModSettings.displayName", "Enable force feedback")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let ffbEnabled: Bool = true;

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Force feedback")
  @runtimeProperty("ModSettings.category.order", "200")
  @runtimeProperty("ModSettings.displayName", "FFB strength (%)")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "5")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let ffbTorquePct: Int32 = 100;

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Force feedback")
  @runtimeProperty("ModSettings.category.order", "200")
  @runtimeProperty("ModSettings.displayName", "Cornering feedback (%)")
  @runtimeProperty("ModSettings.description", "Adds spring stiffness while the car is rotating.")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "5")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let yawFeedbackPct: Int32 = 50;

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Force feedback")
  @runtimeProperty("ModSettings.category.order", "200")
  @runtimeProperty("ModSettings.displayName", "Active torque (%)")
  @runtimeProperty("ModSettings.description", "How hard the wheel pushes toward center, scaled by speed and steering angle.")
  @runtimeProperty("ModSettings.min", "0")
  @runtimeProperty("ModSettings.max", "100")
  @runtimeProperty("ModSettings.step", "5")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let activeTorqueStrengthPct: Int32 = 100;

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Force feedback")
  @runtimeProperty("ModSettings.category.order", "200")
  @runtimeProperty("ModSettings.displayName", "Stationary threshold (m/s)")
  @runtimeProperty("ModSettings.description", "Below this speed the wheel has no centering force.")
  @runtimeProperty("ModSettings.min", "0.0")
  @runtimeProperty("ModSettings.max", "5.0")
  @runtimeProperty("ModSettings.step", "0.1")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let stationaryThresholdMps: Float = 0.5;

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Force feedback")
  @runtimeProperty("ModSettings.category.order", "200")
  @runtimeProperty("ModSettings.displayName", "Invert FFB direction")
  @runtimeProperty("ModSettings.description", "Flip the sign of all restoring forces. Enable if your wheel pushes the wrong way under cornering.")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let ffbInvertDirection: Bool = false;

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Force feedback")
  @runtimeProperty("ModSettings.category.order", "200")
  @runtimeProperty("ModSettings.displayName", "Reverse hardware FFB direction")
  @runtimeProperty("ModSettings.description", "If your wheel snaps to full lock instead of centering when you start driving, enable this. Common on Logitech G HUB wheels (verified on G923) where the SDK's constant-force direction is reported opposite to documentation.")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let ffbOutputSignFlip: Bool = false;

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Force feedback")
  @runtimeProperty("ModSettings.category.order", "200")
  @runtimeProperty("ModSettings.displayName", "Speed-sensitive stiffness")
  @runtimeProperty("ModSettings.description", "On: wheel firms up at speed (sportscar-style steering assist). Off: pure tire-force feedback only, like BeamNG. The wheel goes light at rest and feels more raw at speed.")
  @runtimeProperty("ModSettings.dependency", "hasFfbHardware")
  let ffbUseSpring: Bool = true;

  // ---- Rev-strip LEDs -----------------------------------------------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Rev-strip LEDs")
  @runtimeProperty("ModSettings.category.order", "300")
  @runtimeProperty("ModSettings.displayName", "Enable rev-strip LEDs")
  @runtimeProperty("ModSettings.dependency", "hasRevLeds")
  let ledEnabled: Bool = true;

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Rev-strip LEDs")
  @runtimeProperty("ModSettings.category.order", "300")
  @runtimeProperty("ModSettings.displayName", "Rev strip as visualizer while music is playing")
  @runtimeProperty("ModSettings.dependency", "hasRevLeds")
  let ledVisualizerWhileMusic: Bool = true;

  // ---- Steering & pedals (axis bindings) ---------------------------------
  //
  // The three axes the car cannot drive without. Defaults intentionally
  // empty (tabula rasa); first-run UX prompts the user to bind them.

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Steering & pedals")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "Steering")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "axis")
  let bindSteerAxis: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Steering & pedals")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "Throttle")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "axis")
  let bindThrottleAxis: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Steering & pedals")
  @runtimeProperty("ModSettings.category.order", "400")
  @runtimeProperty("ModSettings.displayName", "Brake")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "axis")
  let bindBrakeAxis: String = "";

  // ---- Driving ------------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Driving")
  @runtimeProperty("ModSettings.category.order", "500")
  @runtimeProperty("ModSettings.displayName", "Horn")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindHorn: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Driving")
  @runtimeProperty("ModSettings.category.order", "500")
  @runtimeProperty("ModSettings.displayName", "Headlights")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindHeadlights: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Driving")
  @runtimeProperty("ModSettings.category.order", "500")
  @runtimeProperty("ModSettings.displayName", "Handbrake")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindHandbrake: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Driving")
  @runtimeProperty("ModSettings.category.order", "500")
  @runtimeProperty("ModSettings.displayName", "Autodrive")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindAutodrive: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Driving")
  @runtimeProperty("ModSettings.category.order", "500")
  @runtimeProperty("ModSettings.displayName", "Exit vehicle")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindExitVehicle: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Driving")
  @runtimeProperty("ModSettings.category.order", "500")
  @runtimeProperty("ModSettings.displayName", "Call vehicle")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindCallVehicle: String = "";

  // ---- Camera -------------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Camera")
  @runtimeProperty("ModSettings.category.order", "600")
  @runtimeProperty("ModSettings.displayName", "Cycle camera forward")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindCameraCycleForward: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Camera")
  @runtimeProperty("ModSettings.category.order", "600")
  @runtimeProperty("ModSettings.displayName", "Rear-view camera")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindRearViewCamera: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Camera")
  @runtimeProperty("ModSettings.category.order", "600")
  @runtimeProperty("ModSettings.displayName", "Reset camera")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindCameraReset: String = "";

  // ---- Combat -------------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Combat")
  @runtimeProperty("ModSettings.category.order", "700")
  @runtimeProperty("ModSettings.displayName", "Shoot primary")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindShootPrimary: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Combat")
  @runtimeProperty("ModSettings.category.order", "700")
  @runtimeProperty("ModSettings.displayName", "Shoot secondary")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindShootSecondary: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Combat")
  @runtimeProperty("ModSettings.category.order", "700")
  @runtimeProperty("ModSettings.displayName", "Shoot tertiary")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindShootTertiary: String = "";

  // ---- Weapons ------------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Weapons")
  @runtimeProperty("ModSettings.category.order", "800")
  @runtimeProperty("ModSettings.displayName", "Next weapon")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindNextWeapon: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Weapons")
  @runtimeProperty("ModSettings.category.order", "800")
  @runtimeProperty("ModSettings.displayName", "Previous weapon")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindPrevWeapon: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Weapons")
  @runtimeProperty("ModSettings.category.order", "800")
  @runtimeProperty("ModSettings.displayName", "Weapon slot 1")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindWeaponSlot1: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Weapons")
  @runtimeProperty("ModSettings.category.order", "800")
  @runtimeProperty("ModSettings.displayName", "Weapon slot 2")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindWeaponSlot2: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Weapons")
  @runtimeProperty("ModSettings.category.order", "800")
  @runtimeProperty("ModSettings.displayName", "Switch weapons")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindSwitchWeapons: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Weapons")
  @runtimeProperty("ModSettings.category.order", "800")
  @runtimeProperty("ModSettings.displayName", "Holster weapon")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindHolsterWeapon: String = "";

  // ---- Radio --------------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Radio")
  @runtimeProperty("ModSettings.category.order", "900")
  @runtimeProperty("ModSettings.displayName", "Radio menu")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindRadioMenu: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Radio")
  @runtimeProperty("ModSettings.category.order", "900")
  @runtimeProperty("ModSettings.displayName", "Next radio station")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindRadioNext: String = "";

  // ---- Gameplay -----------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Gameplay")
  @runtimeProperty("ModSettings.category.order", "1000")
  @runtimeProperty("ModSettings.displayName", "Use consumable")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindUseConsumable: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Gameplay")
  @runtimeProperty("ModSettings.category.order", "1000")
  @runtimeProperty("ModSettings.displayName", "Iconic cyberware")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindIconicCyberware: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Gameplay")
  @runtimeProperty("ModSettings.category.order", "1000")
  @runtimeProperty("ModSettings.displayName", "Quick save")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindQuickSave: String = "";

  // ---- Menus --------------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Menus")
  @runtimeProperty("ModSettings.category.order", "1100")
  @runtimeProperty("ModSettings.displayName", "Open map")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindOpenMap: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Menus")
  @runtimeProperty("ModSettings.category.order", "1100")
  @runtimeProperty("ModSettings.displayName", "Open journal")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindOpenJournal: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Menus")
  @runtimeProperty("ModSettings.category.order", "1100")
  @runtimeProperty("ModSettings.displayName", "Open inventory")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindOpenInventory: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Menus")
  @runtimeProperty("ModSettings.category.order", "1100")
  @runtimeProperty("ModSettings.displayName", "Open phone")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindOpenPhone: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Menus")
  @runtimeProperty("ModSettings.category.order", "1100")
  @runtimeProperty("ModSettings.displayName", "Open perks")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindOpenPerks: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Menus")
  @runtimeProperty("ModSettings.category.order", "1100")
  @runtimeProperty("ModSettings.displayName", "Open crafting")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindOpenCrafting: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Menus")
  @runtimeProperty("ModSettings.category.order", "1100")
  @runtimeProperty("ModSettings.displayName", "Pause")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindPause: String = "";

  // ---- Menu navigation ----------------------------------------------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Menu navigation")
  @runtimeProperty("ModSettings.category.order", "1200")
  @runtimeProperty("ModSettings.displayName", "Confirm")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindMenuConfirm: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Menu navigation")
  @runtimeProperty("ModSettings.category.order", "1200")
  @runtimeProperty("ModSettings.displayName", "Cancel")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindMenuCancel: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Menu navigation")
  @runtimeProperty("ModSettings.category.order", "1200")
  @runtimeProperty("ModSettings.displayName", "Up")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindMenuUp: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Menu navigation")
  @runtimeProperty("ModSettings.category.order", "1200")
  @runtimeProperty("ModSettings.displayName", "Down")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindMenuDown: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Menu navigation")
  @runtimeProperty("ModSettings.category.order", "1200")
  @runtimeProperty("ModSettings.displayName", "Left")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindMenuLeft: String = "";

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Menu navigation")
  @runtimeProperty("ModSettings.category.order", "1200")
  @runtimeProperty("ModSettings.displayName", "Right")
  @runtimeProperty("ModSettings.description", "Bind.\n\n")
  @runtimeProperty("ModSettings.inputBinding", "true")
  @runtimeProperty("ModSettings.inputBinding.type", "button")
  @runtimeProperty("ModSettings.inputBinding.pair", "true")
  let bindMenuRight: String = "";

  // ---- Startup ------------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Startup")
  @runtimeProperty("ModSettings.category.order", "1300")
  @runtimeProperty("ModSettings.displayName", "Ponpon Shit greeting")
  @runtimeProperty("ModSettings.description", "Make the wheel dance when the game starts.")
  let handshakePlayOnStart: Bool = false;

  // ---- Debug --------------------------------------------------------------

  @runtimeProperty("ModSettings.mod", "G-series Wheel")
  @runtimeProperty("ModSettings.category", "Debug")
  @runtimeProperty("ModSettings.category.order", "1400")
  @runtimeProperty("ModSettings.displayName", "Debug logging")
  @runtimeProperty("ModSettings.description", "Logs to red4ext/logs/gwheel-*.log.")
  let ffbDebugLogging: Bool = false;

  // ---- Listener callbacks (invoked by Mod Settings, NOT cb funcs) --------

  public func OnModSettingsChange() -> Void {
    this.Push();
  }

  public func Push() -> Void {
    GWheel_SetInputEnabled(this.inputEnabled);

    GWheel_SetFfbEnabled(this.ffbEnabled);
    GWheel_SetFfbDebugLogging(this.ffbDebugLogging);
    GWheel_SetFfbTorquePct(this.ffbTorquePct);

    GWheel_SetStationaryThresholdMps(this.stationaryThresholdMps);
    GWheel_SetYawFeedbackPct(this.yawFeedbackPct);
    GWheel_SetActiveTorqueStrengthPct(this.activeTorqueStrengthPct);
    GWheel_SetFfbInvertDirection(this.ffbInvertDirection);
    GWheel_SetFfbOutputSignFlip(this.ffbOutputSignFlip);
    GWheel_SetFfbUseSpring(this.ffbUseSpring);

    GWheel_SetHandshakePlayOnStart(this.handshakePlayOnStart);

    GWheel_SetLedEnabled(this.ledEnabled);
    GWheel_SetLedVisualizerWhileMusic(this.ledVisualizerWhileMusic);

    // Action bindings. Names must match the keys gwheel::axis_resolver
    // and the future button-action dispatcher look up. Values are
    // pipe-delimited tagged-token strings emitted by the InputBinding
    // primitive — empty string means unbound, which the C++ side treats
    // as "no axis source" (zero) for axes and "no dispatch" for buttons.
    GWheel_SetActionBinding("SteerAxis",          this.bindSteerAxis);
    GWheel_SetActionBinding("ThrottleAxis",       this.bindThrottleAxis);
    GWheel_SetActionBinding("BrakeAxis",          this.bindBrakeAxis);

    GWheel_SetActionBinding("Horn",               this.bindHorn);
    GWheel_SetActionBinding("Headlights",         this.bindHeadlights);
    GWheel_SetActionBinding("Handbrake",          this.bindHandbrake);
    GWheel_SetActionBinding("Autodrive",          this.bindAutodrive);
    GWheel_SetActionBinding("ExitVehicle",        this.bindExitVehicle);
    GWheel_SetActionBinding("CallVehicle",        this.bindCallVehicle);

    GWheel_SetActionBinding("CameraCycleForward", this.bindCameraCycleForward);
    GWheel_SetActionBinding("RearViewCamera",     this.bindRearViewCamera);
    GWheel_SetActionBinding("CameraReset",        this.bindCameraReset);

    GWheel_SetActionBinding("ShootPrimary",       this.bindShootPrimary);
    GWheel_SetActionBinding("ShootSecondary",     this.bindShootSecondary);
    GWheel_SetActionBinding("ShootTertiary",      this.bindShootTertiary);

    GWheel_SetActionBinding("NextWeapon",         this.bindNextWeapon);
    GWheel_SetActionBinding("PrevWeapon",         this.bindPrevWeapon);
    GWheel_SetActionBinding("WeaponSlot1",        this.bindWeaponSlot1);
    GWheel_SetActionBinding("WeaponSlot2",        this.bindWeaponSlot2);
    GWheel_SetActionBinding("SwitchWeapons",      this.bindSwitchWeapons);
    GWheel_SetActionBinding("HolsterWeapon",      this.bindHolsterWeapon);

    GWheel_SetActionBinding("RadioMenu",          this.bindRadioMenu);
    GWheel_SetActionBinding("RadioNext",          this.bindRadioNext);

    GWheel_SetActionBinding("UseConsumable",      this.bindUseConsumable);
    GWheel_SetActionBinding("IconicCyberware",    this.bindIconicCyberware);
    GWheel_SetActionBinding("QuickSave",          this.bindQuickSave);

    GWheel_SetActionBinding("OpenMap",            this.bindOpenMap);
    GWheel_SetActionBinding("OpenJournal",        this.bindOpenJournal);
    GWheel_SetActionBinding("OpenInventory",      this.bindOpenInventory);
    GWheel_SetActionBinding("OpenPhone",          this.bindOpenPhone);
    GWheel_SetActionBinding("OpenPerks",          this.bindOpenPerks);
    GWheel_SetActionBinding("OpenCrafting",       this.bindOpenCrafting);
    GWheel_SetActionBinding("Pause",              this.bindPause);

    GWheel_SetActionBinding("MenuConfirm",        this.bindMenuConfirm);
    GWheel_SetActionBinding("MenuCancel",         this.bindMenuCancel);
    GWheel_SetActionBinding("MenuUp",             this.bindMenuUp);
    GWheel_SetActionBinding("MenuDown",           this.bindMenuDown);
    GWheel_SetActionBinding("MenuLeft",           this.bindMenuLeft);
    GWheel_SetActionBinding("MenuRight",          this.bindMenuRight);
  }
}

// Attach our settings instance to the player puppet so it lives for the
// session and register it as a listener with Mod Settings. The listener
// receives OnModSettingsChange callbacks when the user clicks Apply in the
// settings menu — we use that to push the new values into the C++ plugin
// via the GWheel_Set* natives.
//
// The hidden capability flags (hasFfbHardware / hasRevLeds /
// hasRightCluster) are not seeded here. Mod Settings ignores writes to the
// listener instance for dependency / visibility purposes. Those flags are
// driven directly into mod_settings/user.ini from the C++ side at plugin
// load (see gwheel/src/mod_settings_seed.cpp).

@addField(PlayerPuppet)
public let m_gwheelSettings: ref<GWheelSettings>;

@wrapMethod(PlayerPuppet)
protected cb func OnGameAttached() -> Bool {
  let result: Bool = wrappedMethod();
  if !IsDefined(this.m_gwheelSettings) {
    this.m_gwheelSettings = new GWheelSettings();
    ModSettings.RegisterListenerToClass(this.m_gwheelSettings);
    ModSettings.RegisterListenerToModifications(this.m_gwheelSettings);
    this.m_gwheelSettings.Push();
  }
  // Register the wheel-input provider so the Mod Settings InputBinding
  // listening UI can reach the gwheel capture natives via
  // ModSettingsInputProviderRegistry.Get(). The registry is process-wide
  // (backed by ModSettings.GetInstance(), a native singleton); a single
  // provider slot, subsequent registrations overwrite.
  ModSettingsInputProviderRegistry.Set(new GWheelInputProvider());
  // Stash a fallback GameInstance for the listening UI's poll scheduler.
  // ModSettingsMainGameController.OnInitialize already stashes one when
  // a player puppet exists; this covers the case where user opened the
  // Mod Settings menu from the main menu (no player) but had previously
  // loaded a save (this OnGameAttached fired and recorded a valid GI).
  ModSettings.GetInstance().inputProviderGameInstance = this.GetGame();
  return result;
}
