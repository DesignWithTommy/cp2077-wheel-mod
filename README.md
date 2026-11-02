# Direct Wheel — Moza & Direct Drive Racing Wheel Support for Cyberpunk 2077

Full racing wheel support for **Cyberpunk 2077 v2.5** with **force feedback**, **speed-sensitive steering compensation**, and **TweakDB steering physics overrides**.

Tested with **Moza R5** and **Logitech G920**. Should work with any DirectInput-compatible wheel.

### 📥 [Download the latest release (3.4.1)](https://github.com/DesignWithTommy/cp2077-wheel-mod/releases/latest)

### What's new in 3.4.0
- **Cyberpunk 2077 2.5 support:** built on the 2.5 RED4ext SDK, and throttle/brake now write the game's 2.5 input fields. The 2.31 build froze the car or put the brake in the wrong place on 2.5.
- **Burnout fix:** the Invert throttle/brake/steering settings were applied twice, so a wheel whose pedals need inverting (G920) sent full gas *and* full brake while idle. They're applied once now.
- **No phantom throttle:** while the wheel is connected it owns gas and brake, so G HUB-injected keys can't add throttle.
- Quieter logs, plus once-a-second pedal and applied-settings lines (first ~10 minutes) for checking an axis setup.

---

## Features

### 🎮 Wheel Input
- **Steering, Throttle, Brake, Clutch** — full axis mapping via Logitech SDK / DirectInput
- **Clutch-as-Brake** — use the softer clutch pedal as brake (toggle in settings)
- **Steering Linearization** — the game applies a quadratic curve internally (`actual_steer ≈ input²`), making small wheel angles feel dead. The mod automatically applies a √x (square root) inverse so your steering response is perfectly linear across the full wheel range
- **Speed Steering Boost** — compensates for the game's built-in steering reduction at high speed. Multiplies the linearized steering signal up to **2x** at 100 mph so the same physical wheel rotation produces consistent in-game turns regardless of velocity
- **Device Selector** — pick wheel and pedal devices independently for split-device setups (e.g. Moza wheel + Logitech pedals). Set by device index (0 = auto-detect, 1..N = specific device)
- **Axis Mapping** — choose throttle/brake axes from a dropdown in Mod Settings — no more editing `config.json`

### 💪 Force Feedback
- **Centering spring** — physics-based, scales with speed
- **Cornering feedback** — spring stiffens during turns
- **Friction** — road texture resistance, scales with speed
- **Sine vibration** — 25 Hz road surface buzz from suspension activity
- **Collision jolt** — sharp impact pulse on collision
- **All FFB effects** have individual strength sliders (0–100%)

### 💡 LED Support
- Rev-strip LED bar (G29/G920/G923)
- Optional WASAPI audio visualizer mode

### ✈️ Let There Be Flight Support
- Built-in bridge for the [Let There Be Flight](https://www.nexusmods.com/cyberpunk2077/mods/5208) mod — fly cars with your racing wheel!
- **Steering wheel → Yaw** (turn left/right in flight)
- **Throttle/Brake pedals → Surge** (forward/backward thrust)
- **Paddle shifters → Lift** (ascend/descend)
- **D-pad → Pitch & Roll** (tilt nose up/down, bank left/right)
- **Buttons → Flight toggle, mode switch, linear/angular brakes**
- All flight actions are fully rebindable in Mod Settings
- Inputs properly release when buttons are released (no sticky controls)
- Works automatically when Let There Be Flight is installed — no extra configuration needed

---

## Requirements

1. **Cyberpunk 2077 v2.5**
2. **[RED4ext](https://www.nexusmods.com/cyberpunk2077/mods/2380)** — native plugin loader
3. **[Mod Settings](https://www.nexusmods.com/cyberpunk2077/mods/4885)** — in-game settings UI

---

## Installation

### Quick Install (zip)

1. Download `direct_wheel-3.4.1.zip` from [Releases](https://github.com/DesignWithTommy/cp2077-wheel-mod/releases/latest)
2. Extract **directly into your Cyberpunk 2077 game folder**, for example:
   ```
   D:\SteamLibrary\steamapps\common\Cyberpunk 2077\
   ```
3. The zip merges into the existing folder structure:
   ```
   Cyberpunk 2077/
   ├── r6/scripts/direct_wheel/*.reds                                    ← Redscript
   └── red4ext/plugins/
       └── direct_wheel/direct_wheel.dll                                 ← Main plugin
   ```
4. **Start G HUB** (or LGS) before launching the game
5. Launch Cyberpunk 2077

### First Launch
- First launch after install is slow (30–60 seconds) — Redscript needs to compile
- Your wheel should rumble briefly on load (handshake confirmation)
- Enter any vehicle to start driving with the wheel

---

## Configuration

All settings are in-game: **Main Menu → Settings → Mod Settings → WHEEL MOD**

> **Note:** It is highly recommended to set your wheel's operating range (rotation angle) to **720 degrees** in your wheel software (Logitech G HUB, Moza Pit House, Fanatec Control Panel, etc.) for the best steering response.

### Wheel Input Section

| Slider | Default | Description |
|---|---|---|
| Enable wheel input | ON | Master toggle for steering/throttle/brake injection |
| Treat clutch as brake | ON | Use clutch pedal as brake (softer pedal) |
| Speed steering boost (%) | 45 | Compensates steering at high speed. 0=off, 50=1.5x at 100 mph, 100=2x max |
| Steering turn speed | 1 | Engine physics limit for turn speed. **For wheel play, set to 6+.** *(Requires game restart to apply)* |
| Steering re-center speed | 1 | Engine physics limit for return-to-center speed. **For wheel play, set to 6+.** *(Requires game restart to apply)* |
| Invert throttle / brake / clutch | OFF | Flip a pedal whose axis reads full when released. **Logitech G920: turn all three ON.** |
| Invert steering | OFF | Flip steering direction |

### Axis Mapping Section

| Slider | Default | Description |
|---|---|---|
| Wheel device (0 = Auto) | 0 | Which controller to use for steering + FFB. 0 = auto-detect, 1..N = specific device by index |
| Pedal device (0 = Same as wheel) | 0 | Separate USB pedal device for throttle/brake. 0 = use wheel device |
| Throttle axis | lZ | DirectInput axis for throttle. Logitech G29/G923 = lZ, **G920 = lY**, some Moza = lY |
| Brake axis | lRz | DirectInput axis for brake (G920 = lRz) |

> Settings take effect when you press **APPLY** in Mod Settings. To check a pedal setup, look for `[direct_wheel:pedal]` in `red4ext/logs/direct_wheel-*.log`: with your feet off the pedals it should read `thr=0.00 brk=0.00`.

> To see which device is at which index, run `tools/input_probe.exe` — it lists all devices with numbers.

### Force Feedback Section

| Slider | Default | Description |
|---|---|---|
| Enable force feedback | ON | Master FFB toggle |
| FFB strength (%) | 100 | Overall FFB magnitude |
| Cornering feedback (%) | 50 | Spring stiffness during turns |
| Friction force (%) | 30 | Road texture resistance |
| Road vibration (%) | 30 | Sine wave buzz from road surface |
| Collision jolt (%) | 50 | Impact pulse on collision |

---

## Tuning Guide

### Steering feels dead at high speed
Increase **Speed steering boost** (try 70–100%).

### Steering is too twitchy at high speed
Decrease **Speed steering boost** (try 20–40%).

### FFB is too strong / too weak
Adjust **FFB strength** first (overall), then tune individual effects.

### The car does a burnout or creeps on its own
Your pedal axes or inverts are wrong, so a released pedal reads as pressed. Check the `[direct_wheel:pedal]` log line (see above), fix the axis or invert, and press **APPLY**.

### Wheel doesn't respond at all
1. Make sure **G HUB** (or LGS) is running
2. Check that RED4ext is installed correctly
3. Look at logs: `red4ext/logs/direct_wheel-*.log`

> Other wheels (Fanatec, Thrustmaster, etc.) - run `input_probe.exe` to check your axes, then select the correct axis in **Mod Settings → G-series Wheel → Axis mapping**.

---

## How It Works

The mod works via a **RED4ext plugin** (`direct_wheel.dll`) that hooks `vehicle::BaseObject::UpdateVehicleCameraInput` to inject wheel axis values each frame. It also runs FFB effects (centering spring, friction, sine, jolt) via DirectInput.

**Steering pipeline:** The game applies a quadratic curve to steering input (`actual_steer ≈ input²`), which makes small wheel angles feel dead (at 10° you only get 3.6% of the linear response). The mod applies the mathematical inverse — `√x` (square root) — before the game processes it, so `√x` → game squares it → `x`. This perfectly linearizes your steering across the full range. The **Speed Steering Boost** then multiplies this linearized signal to compensate for the game's speed-dependent steering attenuation.

---

## Uninstallation

Delete these folders:
```
red4ext/plugins/direct_wheel/
r6/scripts/direct_wheel/
```

---

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

## License

MIT



