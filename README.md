# Cyberpunk 2077 — Logitech G-series Steering Wheel Mod

**v2.31.2** for Cyberpunk 2077 game patch 2.31.

Drive Cyberpunk 2077 with a Logitech G-series steering wheel. Your wheel handles steering, throttle, and brake. Force feedback fires from in-game physics (centering as you turn, cornering load, surface texture, collision jolts, slip when you lose grip). Rev-strip LEDs follow real engine RPM. When the in-car radio is on, the rev strip becomes a music visualizer.

There is no virtual gamepad to install, no XInput shim, no third-party driver. The wheel talks to the game via the official Logitech Steering Wheel SDK (the same one driving sims use).

## Supported wheels

The mod was designed to work with any wheel that is compatible with Logitech G HUB.

I tested with:

- G923 (Xbox variant)
- G920

But theoretically these should all be compatible:

- WingMan Formula Force, Formula Force GP
- Driving Force, Driving Force Pro, Driving Force GT
- Momo Force, Momo Racing
- G25, G27, G29, G920
- G923 (Xbox, PS, PC variants)
- Formula Vibration Feedback

The mod auto-detects what hardware your wheel actually has (force feedback motor, rev-strip LEDs, lower-cluster buttons) and only shows the relevant settings.

Non-Logitech wheels (Thrustmaster, Fanatec, Moza) are not supported.

## Required mods

1. **[RED4ext](https://www.nexusmods.com/cyberpunk2077/mods/2380)**
1. **[redscript](https://www.nexusmods.com/cyberpunk2077/mods/1511)**
1. **[ArchiveXL](https://www.nexusmods.com/cyberpunk2077/mods/4198)**
1. **[Mod Settings](https://www.nexusmods.com/cyberpunk2077/mods/4885)** v0.2.21 or later

## Required drivers

- **[Logitech G HUB](https://www.logitech.com/innovation/g-hub.html)**

## Install

1. Click the **Vortex** download button on this page (the green button in the Files tab).
1. The installer's first page explains the file conflict. Read it, then click Next.
1. Pick whether you want the startup pulse animation, then finish the installer.
1. Vortex will then prompt you about a **file conflict on four files** belonging to Mod Settings:
   - `mod_settings.dll`
   - `packed.reds`
   - `ModSettings.archive`
   - `ModSettings.archive.xl`

   For each one, choose **this mod** to win the conflict (load *after* Mod Settings). The included files are a small fork of Mod Settings that adds one extra UI primitive — the press-to-bind slot you'll use to map your wheel buttons. The fork is API-compatible with upstream, so other mods that use Mod Settings keep working.

(If you go manual instead of Vortex, the same four files need to overwrite upstream Mod Settings' versions in `red4ext\plugins\mod_settings\`.)

## Bind your wheel

Defaults are intentionally empty. On first launch, open **Main Menu → Mod Settings → G-series Wheel** and bind:

1. **Steering & pedals.** Steering, Throttle, Brake. Click the slot, then turn the wheel or press the pedal you want to assign.
1. Anything else you want — Horn, Handbrake, Headlights, paddles for next/previous weapon, etc. Each slot accepts two bindings, so you can assign two wheel buttons to the same action (e.g. left paddle AND D-pad-Up to "Next weapon").

Categories: Steering & pedals, Driving, Camera, Combat, Weapons, Radio, Gameplay, Menus, Menu navigation.

## G HUB

This mod respects G HUB. Wheel rotation range, sensitivity, and centering spring stay with G HUB; the mod reads the rotation range and scales its force feedback to match. G HUB and this mod can both be running at the same time.

If you have wheel buttons configured in your G HUB Cyberpunk profile that overlap with the bindings here, you may get doubled keypresses. Clear the overlapping bindings from your G HUB Cyberpunk profile.

I recommend starting with an operating range of 180 to start, since the rotation will match the animation you see in 1st person view inside of a car. I tend to like it a bit wider to help mitigate fishtailing.

## Settings

All settings live in **Main Menu → Mod Settings → G-series Wheel**. Highlights:

- **Wheel input.** Master enable, steering deadzone (filters small wheel-center jitter — 2-3% is typical), response curve (Default / Subdued / Sharp), invert steering.
- **Force feedback.** Master enable, FFB strength, cornering feedback, active torque, stationary threshold, **invert FFB direction** (G923 quirk, see Troubleshooting), **speed-sensitive stiffness** (sportscar assist on/off).
- **Rev-strip LEDs.** Master enable, music visualizer while music is playing.
- **Steering & pedals / Driving / Camera / Combat / Weapons / Radio / Gameplay / Menus / Menu navigation.** All bindable.
- **Startup.** Pon pon shi greeting on/off.
- **Debug.** Logs to `red4ext/logs/gwheel-*.log`.

Hardware-capability sections auto-hide on wheels without the relevant components.

## Troubleshooting

- **Wheel detected but the car doesn't move.** Make sure RED4ext is installed and current. Then check that you've actually bound Steering, Throttle, and Brake under Mod Settings → G-series Wheel → Steering & pedals — defaults are empty.
- **Settings page is missing.** Mod Settings or ArchiveXL aren't installed correctly. The wheel will still work on defaults, but you can't bind anything.
- **The bind slots are missing the press-to-bind capture, or the page looks like vanilla Mod Settings.** Vortex installed stock Mod Settings on top of the fork that ships with this mod. Re-deploy in Vortex and let this mod win the file conflicts on `mod_settings.dll`, `packed.reds`, `ModSettings.archive`, and `ModSettings.archive.xl`.
- **Doubled keypresses on a wheel button.** G HUB and this mod are both binding the same key. Either clear the entry from your G HUB Cyberpunk profile, or unbind that action here.
- **Wheel rolls *away* from center instead of recovering, or fights you back into the turn under load.** Open Mod Settings → G-series Wheel → Force feedback and enable **Invert FFB direction**. This is a known Logitech-wide issue (G29/G920/G923/G27/G25/Driving Force/Momo) where some unit/firmware/G HUB combinations interpret the SDK's constant-force direction opposite to what's documented. Forza, iRacing, and most other sim titles expose the same toggle.
- **Wheel feels too stiff or too floppy at speed.** **Speed-sensitive stiffness** in Force feedback settings controls this. On (default): wheel firms up at speed (sportscar-style steering assist). Off: pure tire-force feedback only, like BeamNG. The wheel goes light at rest and feels more raw at speed.
- **Operating-range slider in G HUB doesn't change anything in-game** (FFB feels the same regardless of what you set the range to, or the in-game wheel doesn't track physical rotation). G HUB's IPC layer can get into a stuck state where the value the slider shows isn't what's actually being reported to applications. Reinstall G HUB to clear it. The mod's `red4ext/logs/gwheel-*.log` will show `range readback at bind: SDK=N G HUB properties=M` — if those two values disagree, or either reads `FAILED`, that's confirming the bad-G-HUB diagnosis.

## Credits

- [RED4ext](https://github.com/WopsS/RED4ext) by WopsS
- [redscript](https://github.com/jac3km4/redscript) by jac3km4
- [Mod Settings](https://github.com/jackhumbert/mod_settings) by jackhumbert
- [ArchiveXL](https://github.com/psiberx/cp2077-archive-xl) by psiberx

## Reporting issues

Include your wheel model, Cyberpunk patch version, RED4ext version, and your most recent `red4ext/logs/gwheel-*.log` file.
