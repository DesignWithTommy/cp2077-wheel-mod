# Cyberpunk 2077 — Moza & Direct Drive Wheel Support

**v3.2.0** for Cyberpunk 2077 game patch 2.31.

Full racing wheel support for Cyberpunk 2077. Drive with a **Moza** or any other **Direct Drive** steering wheel! Your wheel handles steering, throttle, brake, and clutch. Force feedback fires from in-game physics (centering as you turn, cornering load, surface texture, collision jolts, slip when you lose grip). 

There is no virtual gamepad to install, no XInput shim, no third-party driver. The mod hooks the game directly via DirectInput/Logitech SDK.

## Supported wheels

The mod was designed to work with DirectInput-compatible wheels. Tested and confirmed on:

**Moza Racing:**
- Moza R5
- Moza R3
*(Map your axes manually if needed in `config.json`)*

**Logitech:**
- G923 (Xbox, PS, PC variants)
- G920, G29, G27, G25
- Momo Force, Driving Force GT

The mod auto-detects what hardware your Logitech wheel actually has (force feedback motor, rev-strip LEDs, lower-cluster buttons) and only shows the relevant settings.

## New Features
- **Automatic Steering Linearization:** The game applies a quadratic curve to steering (actual_steer ≈ input²), which kills small-angle precision — at 10° wheel turn you only get 3.6% of the expected response. The mod applies the mathematical inverse (√x) before the game processes it, so √x → game squares it → x. Your steering is perfectly linear across the full wheel range, no manual tuning needed.
- **Steering Deadzone:** Added a slider to configure a deadzone (0 to 5 degrees) in the center of the wheel.
- **Spring Non-Linearity:** Added a slider to make the centering spring force ramp up geometrically at small steering angles. Perfect for weaker wheels to feel the center better.
- **Speed Steering Boost:** Compensates steering at speed by amplifying the linearized signal up to a 2x multiplier at 100 mph, so you can still turn perfectly at high speeds.
- **Invert Pedals:** Separate settings to invert throttle and brake pedals directly in the mod menu. (Note: For Logitech pedals, you should check both boxes as they are physically inverted!)
- **Device & Axis Mapping:** Select wheel and pedal devices independently (for split-device setups), and pick throttle/brake axes from a dropdown — no more editing `config.json`. Use the included `input_probe.exe` to identify your device numbers and axes.

## Recommended mods
- [Steering Speed Configurator](https://www.nexusmods.com/cyberpunk2077/mods/29868)
- [Cyberpunk 2077 Telemetry Mod SimHub](https://www.nexusmods.com/cyberpunk2077/mods/29767)
- [Let There Be Flight](https://www.nexusmods.com/cyberpunk2077/mods/5208) — fly any car in Cyberpunk 2077

## ✈️ Let There Be Flight — Wheel Support

This mod includes a built-in bridge for [Let There Be Flight](https://www.nexusmods.com/cyberpunk2077/mods/5208). If you have Let There Be Flight installed, you can fly cars using your racing wheel with no extra setup.

**How it works:**
- **Steering wheel** controls yaw (turn left/right in flight)
- **Throttle & Brake pedals** control surge (forward/backward thrust)
- **Left paddle** — descend (lift down)
- **Right paddle** — ascend (lift up)
- **D-pad Up/Down** — pitch forward/backward (tilt the nose)
- **D-pad Left/Right** — roll left/right (bank the vehicle)
- **Moza FL button** — toggle flight mode on/off
- **Moza S1/S2** — cycle flight modes forward/backward
- **Moza BOX** — linear brake
- **Moza PL** — angular brake

All flight bindings are fully customizable in **Mod Settings → Wheel Mod → Button bindings**.

When you release a button, the input resets to zero immediately — no stuck controls.

**Just install both mods and go fly!**

## Required mods

1. **[RED4ext](https://www.nexusmods.com/cyberpunk2077/mods/2380)**
1. **[redscript](https://www.nexusmods.com/cyberpunk2077/mods/1511)**
1. **[ArchiveXL](https://www.nexusmods.com/cyberpunk2077/mods/4198)**
1. **[Mod Settings](https://www.nexusmods.com/cyberpunk2077/mods/4885)** v0.2.21 or later

## Required drivers

- **[Logitech G HUB](https://www.logitech.com/innovation/g-hub.html)**

## Install

1. Click the **Vortex** download button on this page (the green button in the Files tab).

*(Manual Download is also available if you don't use Vortex. Just extract the zip directly into your Cyberpunk 2077 game folder).*

## G HUB

This mod respects G HUB. Wheel rotation range, sensitivity, and centering spring stay with G HUB; the mod reads the rotation range and scales its force feedback to match. G HUB and this mod can both be running at the same time.

If you have wheel buttons configured in your G HUB Cyberpunk profile that overlap with the bindings here, you may get doubled keypresses. Clear the overlapping bindings from your G HUB Cyberpunk profile.

I highly recommend setting your operating range / rotation angle to **720 degrees** in your wheel software for the best driving experience and steering response.

## Troubleshooting

- **Wheel detected but the car doesn't move.** Make sure RED4ext is installed and current.
- **Settings page is missing.** Mod Settings or ArchiveXL aren't installed correctly. The wheel will still work on defaults.
- **Doubled keypresses on D-pad or A/B/X/Y.** G HUB and this mod are both binding the same keys. Reset the wheel-button entries in your G HUB Cyberpunk profile back to their default bindings.

## Credits

- **Grant Perdue ([clevergrant](https://github.com/clevergrant))**: the original [cp2077-wheel-mod](https://github.com/clevergrant/cp2077-wheel-mod) (Logitech G-series wheel support, FFB, LEDs, Mod Settings page) and the patched Mod Settings build
- **SERG ([natpoh](https://github.com/natpoh))**: [cp2077-wheel-mod-moza](https://github.com/natpoh/cp2077-wheel-mod-moza), Moza / direct-drive support, axis mapping, device selector, steering linearization and speed boost, Let There Be Flight bridge
- **[DesignWithTommy](https://github.com/DesignWithTommy)**: the Cyberpunk 2077 2.5 update (3.4.0) and the burnout fix
- [RED4ext](https://github.com/WopsS/RED4ext) and the [RED4ext SDK](https://github.com/WopsS/RED4ext.SDK) by WopsS and contributors (2.5 types)
- [redscript](https://github.com/jac3km4/redscript) by jac3km4
- [Mod Settings](https://github.com/jackhumbert/mod_settings) and [Let There Be Flight](https://www.nexusmods.com/cyberpunk2077/mods/5208) by jackhumbert
- [ArchiveXL](https://github.com/psiberx/cp2077-archive-xl) by psiberx
- [Cyber Engine Tweaks](https://github.com/maximegmd/CyberEngineTweaks) by yamashi / maximegmd
- Logitech Steering Wheel SDK by Logitech
- [NativeDB](https://nativedb.red4ext.com) and the [REDmodding wiki](https://wiki.redmodding.org) communities, for game type references
- Everyone who tested wheels and sent logs

## Source Code

Source code is available on [GitHub](https://github.com/DesignWithTommy/cp2077-wheel-mod) (2.5 build), forked from [natpoh/cp2077-wheel-mod-moza](https://github.com/natpoh/cp2077-wheel-mod-moza) and [clevergrant/cp2077-wheel-mod](https://github.com/clevergrant/cp2077-wheel-mod). Pull requests and feedback are welcome!

## Reporting issues

Include your wheel model, Cyberpunk patch version, RED4ext version, and your most recent `red4ext/logs/direct_wheel-*.log` file.



