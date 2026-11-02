# Changelog

## 3.4.1
- **Keyboard works with the wheel again:** 3.4.0 let only the wheel drive gas/brake; wheel and keyboard/gamepad now merge (the larger wins), as before.
- **Wheel sending no data:** if every axis reads exactly 32767 (G HUB connected but no reports), the wheel adds no input instead of half gas plus half brake, and the log warns `wheel sends no data` every 30 s.

## 3.4.0
- **Cyberpunk 2077 2.5:** built on the 2.5 RED4ext SDK; throttle/brake now write the 2.5 input fields (0x264 accel, 0x268 decel, 0x270 and 0x280 accel-minus-decel), measured from a keyboard W/S test. The old offsets froze the car or sent the brake into the wrong field.
- **Burnout fix:** pedal and steering inverts were applied twice (wheel read, then frame build), so a G920 with Invert on sent full gas plus full brake at rest. They are applied once now.
- The wheel owns gas/brake while connected, so G HUB-injected keys can't add phantom throttle.
- Bind map no longer re-logs on every unchanged settings push; pedal, input-field and applied-settings debug lines (capped ~10 min) for diagnosing axis setups.

## 3.2.0 — 2026-05-25
- **Steering Deadzone:** Added a new slider (0 to 5 degrees) to configure a deadzone in the center of the wheel.
- **Spring Non-Linearity:** Added a new slider (0 to 100%) to make the centering spring force ramp up geometrically at small steering angles. Perfect for weaker wheels to feel the center better.

## 3.1.0 — 2026-05-25
- **Let There Be Flight — full wheel support:** Built-in bridge for the [Let There Be Flight](https://www.nexusmods.com/cyberpunk2077/mods/5208) mod. Steer, throttle, brake, pitch, roll, lift, and toggle flight mode — all from your racing wheel. No extra mods needed.
- **New flight actions:** Flight_PitchForward, Flight_PitchBackward, Flight_RollLeft, Flight_RollRight added to the bindable action list (total: 48 actions).
- **Sticky input fix:** All flight axis inputs (lift, pitch, roll, brakes) now properly reset to zero when the button is released. Previously they could get stuck at full deflection.
- **Default bindings updated for Moza:** D-pad maps to pitch/roll, paddles to lift up/down, extra buttons to flight toggle/mode switch/brakes.

## 3.0.2 — 2026-05-23
- Added native support for Let There Be Flight mode toggling and lift controls directly from the steering wheel.

## 3.0.1 — 2026-05-21

- **Separated Steering Speed:** The steering acceleration logic has been entirely removed from the C++ DLL and moved to a standalone Redscript mod (`cp2077-steering-speed-mod`) powered by TweakXL. This makes the main wheel mod lighter and more robust.

## 3.0.0 — 2026-05-20

- **Major Packaging Fix:** Restructured the release zip so that manual extraction works correctly out of the box (`r6` and `red4ext` folders are now properly populated).
- Fixed a critical bug in the FOMOD installer where Vortex was unable to find the mod files due to outdated file paths.
- Fixed an issue where the patched `mod_settings.dll` was sometimes missing from the zip.

## 2.34.0 — 2026-04-30

- **Device Selector:** Added in-game device index sliders for wheel and pedals. Users with split-device setups (e.g. Moza wheel + separate USB pedals) can now select each device independently without editing config files. 0 = auto-detect, 1..N = specific device.
- **Axis Mapping:** Throttle and brake axes are now selectable from a dropdown in Mod Settings (lX, lY, lZ, lRx, lRy, lRz, Slider0, Slider1). No more editing `config.json` for non-Logitech wheels.
- **input_probe:** Updated to show all connected devices simultaneously with real-time axis values, including device numbering that matches the in-game selector.
- **Thread-safe device reset:** `ResetDevices()` now uses a deferred flag instead of immediately releasing DirectInput handles, preventing race conditions and crashes when changing settings.

## 2.33.0 — 2026-04-30

- Scaled up the base centering-spring multipliers by ~3.3x in the physics engine. Weak belt-driven wheels (like Thrustmaster T300RS) can now reach the hardware's 100% saturation limit. Users with powerful Direct Drive bases should lower the Spring Force slider in Mod Settings to 30-40% to compensate.

## 2.32.0 — 2026-04-28

- Added separate "Invert throttle" and "Invert brake" toggles to Mod Settings. Inverts the raw input from the corresponding axes before applying any logic (like "Clutch as brake"). This makes the mod plug-and-play for inverted pedal hardware (like some Moza/Fanatec setups) that report 1.0 when physically released and 0.0 when pressed.

## 2.31.0 — 2026-04-25

First public release. Tested against Cyberpunk 2077 patch 2.31 (build 5294808).

### Wheel I/O

- Wheel input and force feedback go through the official Logitech Steering Wheel SDK (vendored under `direct_wheel/vendor/LogitechSDK_unpacked/`). DirectInput enumeration, the ViGEmBus virtual-pad bridge, and the XInput-hook routing layer are all gone.
- Hardware capability detection: the plugin reads which features each wheel actually has (FFB motor, rev-strip LEDs, lower-cluster cluster) and seeds `red4ext/plugins/mod_settings/user.ini` at load time so the in-game Settings page only shows sections relevant to the connected hardware.

### Vehicle input injection

- Vehicle input is delivered via a detour on `vehicle::BaseObject::UpdateVehicleCameraInput`, resolved through RED4ext's `UniversalRelocBase::Resolve(501486464u)`. Function-address drift across game patches is now RED4ext's concern; their address database ships per-patch.
- Injection is gated on a cached player-vehicle pointer published from `direct_wheel_mount.reds` via `DirectWheel_SetPlayerVehicle` / `DirectWheel_ClearPlayerVehicle`. The detour fires for every visible vehicle each tick; without this gate the plugin would remote-drive parked cars and AI traffic.
- Empirical struct field offsets (CP2077 v2.31): steer +0x278, throttle +0x264, brake +0x268.

### Force feedback model

- Per-car physics-driven FFB derived from the game's `WheeledPhysics` (turn rate, wheelbase) and per-tick chassis state. Layered SDK effects: passive spring (speed² + yaw bonus), active alignment torque (humped curve × load × grip × weight), damper, road-surface SINE, airborne shake.
- Road-texture envelope: signal is `|Δω_roll| + |Δω_pitch| + 0.8·|Δvz|`. Asymmetric envelope (slow attack, fast decay) plays curbs as one-shot jolts and rough pavement as continuous hum without 5 Hz drone artifacts.
- Collision jolts via `@wrapMethod(VehicleObject) OnVehicleBumpEvent`. Kick is computed in the player's world-right frame regardless of which side of the impact received the event, so the wheel kicks AWAY from impacts.
- Slip-angle countersteer from car-local lateral velocity dotted against world-right; gated on `(1 - gripFactor)` so it only activates when the car is actually sliding.
- Per-effect SDK magnitudes scaled by user FFB strength via `wheel::SetGlobalStrength` (the Logitech Properties API is broken on most G HUB builds; per-effect multipliers are the workaround).
- 200 ms watchdog in the pump thread tears down all effects when the vehicle detour stops firing (pause menu, unmount, etc.).

### Rev-strip LEDs and music visualizer

- LED rev strip driven by real engine RPM via Blackboard listener on `VehicleDef.RPMValue`, normalized against `VehicleEngineData.MaxRPM` per-vehicle.
- When the in-car radio is on (`VehicleDef.VehRadioState`), the rev strip switches to a music visualizer driven by WASAPI loopback capture (game-process audio only, configurable).
- Save-load-with-radio-on recovery via three delayed reseeds (0.5 s, 1.5 s, 3 s post-attach) since the radio component takes time to spin up.
- Belt-and-suspenders `@wrapMethod(VehicleComponent) OnRadioToggleEvent` catches any toggle that bypasses the Blackboard listener.

### Wheel button bindings

- 20 physical wheel controls (paddles, D-pad, A/B/X/Y, Start/Select, LSB/RSB, Plus/Minus, scroll click + CW + CCW, Xbox/Guide) each bindable to one of 36 in-game actions through a single native: `DirectWheel_SetInputBinding(inputId: Int32, action: Int32)`.
- Per-device DirectInput-button-index layouts; G923 Xbox layout empirically verified via the new `tools/input_probe` tool. G923 PS/PC, G920, G29, G27 fall back to the G923 Xbox layout until each is probed.
- D-pad and A default to MenuUp/Down/Left/Right/Confirm so the wheel navigates pause/map/inventory menus out of the box. Bindings are user-overridable; "None" truly disables the input. (CP2077's arrow keys are secondary vehicle controls, so a hardcoded menu-state-aware override would steer the car when the user pressed the D-pad with binding=None — that override was deliberately removed.)
- Low-level keyboard hook (`kbd_hook`) suppresses G HUB's synthetic vehicle-key events while V is on foot, while letting the plugin's own `SendInput` events through (tagged with `dwExtraInfo = 'dWHL'`).

### In-game settings (Mod Settings)

- Single `DirectWheelSettings` class drives the in-game settings page. Categories: Wheel input, Force feedback, Rev-strip LEDs, Button bindings, Lower-cluster bindings, Startup, Debug.
- Hardware capability flags (`hasFfbHardware`, `hasRevLeds`, `hasRightCluster`) hide irrelevant sections on wheels without that hardware.
- This release bundles a patched build of `jackhumbert/mod_settings` v0.2.21 that adds a `ModSettings.hidden` runtime property. Without the patch, the three capability flags would render as visible Bool toggles in the settings UI. The patch is API-compatible with upstream; the FOMOD installer overwrites the user's stock Mod Settings DLL on install.

### Distribution

- Single FOMOD step at install time: Pon pon shi greeting on / off (writes `red4ext/plugins/direct_wheel/config.json`). All other prerequisites (RED4ext, ArchiveXL, Mod Settings, G HUB) are declared as Nexus required-mods rather than nagged through the installer.
- New dev tool: `tools/input_probe` — empirical button / POV / axis edge logger for building per-device layouts. Replaces the removed `tools/sigfinder` (obsolete now that vehicle hook drift is RED4ext's problem).
- New dev tool: `tools/wheel_reset` — pushes G HUB's controller properties back to the wheel; useful when firmware is stuck in an SDK-managed state.

### What's NOT here

- No surface-material FFB mapping yet. The raycast and material-CName plumbing in `direct_wheel_surface.reds` works end-to-end, but mapping CNames to FFB baselines is dormant. Vanilla CP2077 doesn't appear to assign physmats to off-road geometry, so dirt / sand / gravel never surface; on-road materials by themselves don't produce a useful signal. See `direct_wheel/src/wheel.cpp::SurfaceBaselineForMaterial` for where to wire it in if you can capture useful CNames.
- No handbrake-axis injection. The wheel's handbrake position is read but the corresponding game struct field offset hasn't been identified.
- No menu-state-aware binding override (see Wheel button bindings above).

