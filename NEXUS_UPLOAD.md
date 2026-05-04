# Nexus Upload Cheatsheet

Reference for filling out the Nexus mod-upload form for v2.31.2. Settings on this page are entered through the Nexus UI when you create / edit the mod page; they are not part of the zip.

## Page metadata

| Field | Value |
| --- | --- |
| **Mod Name** | `Logitech G-series Steering Wheel` |
| **Version** | `2.31.2` |
| **Game** | Cyberpunk 2077 |
| **Category** | Vehicles (or Gameplay if Vehicles isn't accepted; this mod doesn't fit Modders Resources) |
| **Author** | Grant Perdue |

## Summary (one-line)

```
Drive Cyberpunk 2077 with a Logitech G-series steering wheel. Real wheel I/O, physics-aware force feedback, real-RPM rev-strip LEDs, music visualizer, and 20 bindable wheel buttons. No virtual gamepad, no drivers.
```

## Description (long form)

Paste the contents of [NEXUS_README.md](NEXUS_README.md) into the description editor. The Nexus editor accepts BBCode and Markdown (depending on the editor mode you choose; Markdown mode is easier). Strip the top `# Cyberpunk 2077 — Logitech G-series Steering Wheel Mod` heading since the page already shows the mod name.

## Tags

Suggested:

```
logitech, steering wheel, g923, g920, g29, g27, g25, force feedback, ffb, driving, vehicles,
RED4ext, Mod Settings, controller, input, racing
```

## Required mods (the "Requirements" widget)

Enter each by Nexus mod ID:

| Mod | Nexus ID | URL |
| --- | --- | --- |
| RED4ext | 2380 | https://www.nexusmods.com/cyberpunk2077/mods/2380 |
| redscript | 1511 | https://www.nexusmods.com/cyberpunk2077/mods/1511 |
| ArchiveXL | 4198 | https://www.nexusmods.com/cyberpunk2077/mods/4198 |
| Mod Settings | 4885 | https://www.nexusmods.com/cyberpunk2077/mods/4885 |

Mark all four as **Required** (not Optional / Recommended).

Logitech G HUB is not on Nexus; mention it in the description's Required mods section instead.

## Permissions and credits

Selected during the [permissions interview](#) on Nexus. Recommended answers (open license; matches the MIT in the repo):

| Question | Answer |
| --- | --- |
| Other users can use assets in their files | Yes |
| Other users can upload your file as a translation | Yes |
| Other users can convert your file to work with other games | Yes |
| Other users can use assets from your file in their files for modding | Yes |
| Other users can use assets from your file in their files for **commercial use** | Yes |
| Other users can modify your file and release bug fixes / improve performance / cleanup | Yes |
| You give credit to me | Optional ("If you reuse it I'd appreciate credit, but it's not required.") |
| You give credit to me as the author of the original | Optional |
| **License** | MIT (link to the LICENSE file in your repo) |

In the **Credits** section, list:

```
- WopsS (RED4ext)
- jac3km4 (redscript)
- jackhumbert (Mod Settings; this mod bundles a small patch on top of v0.2.21)
- psiberx (ArchiveXL)
- Logitech (Steering Wheel SDK)
- CD Projekt Red (Cyberpunk 2077)
```

## Files

Upload the FOMOD zip:

```
dist/gwheel-2.31.2.zip
```

In the file's metadata:

- **File name**: `Logitech G-series Wheel Mod`
- **File version**: `2.31.2`
- **Category**: `Main Files`
- **Description**: `Full mod, FOMOD installer. v2.31.2 for game patch 2.31.`

## Images (need at least one)

Nexus needs a banner / preview image. You'll need to create at least one:

- **Required**: 1 banner image (recommended 1920x540 or similar wide aspect).
- **Recommended**: a screenshot or two showing a Logitech wheel in front of Cyberpunk on screen, plus a screenshot of the in-game Mod Settings page open at G-series Wheel. Animated GIF of the rev-strip LEDs reflecting RPM would sell the feature better than any text.

(Out of scope for this docs pass; you'll need to capture these manually.)

## Sticky comment / pinned post (optional but recommended)

Pinned at the top of your comments section; warns users about common install issues.

```
TROUBLESHOOTING TIPS — read before reporting an issue:

1. Mod Settings is REQUIRED (not optional). Without it, the mod's redscript files
   will not compile and the mod will not load. Same for redscript, RED4ext,
   ArchiveXL.

2. The release zip ships a patched build of Mod Settings' DLL. When Vortex asks
   about a file conflict on mod_settings.dll, choose THIS MOD to win. The patch
   is API-compatible with upstream and is required for the in-game Settings page
   to render correctly.

3. If RED4ext shows you a hard error message at game launch about an
   unresolved hash, that is RED4ext, not this mod. RED4ext needs an update
   to match the current Cyberpunk patch. Check the RED4ext Nexus page.

4. Bug reports must include: your wheel model, Cyberpunk patch version,
   RED4ext version, and the contents of red4ext/logs/gwheel-*.log
   (most recent file). Without those four pieces of info I cannot help.

5. Non-Logitech wheels (Thrustmaster, Fanatec, Moza) are not supported.

6. If the G HUB operating-range slider doesn't seem to affect anything in-game
   (FFB doesn't change with range, in-game wheel doesn't track physical rotation),
   reinstall G HUB. Its IPC can get stuck in a state where the value the slider
   shows isn't what's reported to applications. The mod's gwheel-*.log will show
   "range readback at bind: SDK=N G HUB properties=M" — if those disagree or
   either reads FAILED, that confirms the bad-G-HUB diagnosis.
```

## Changelog tab

Nexus has a separate Changelog tab. Paste [CHANGELOG.md](CHANGELOG.md). For future releases, only paste the new version's section, not the whole file.

## Nexus deep-link install ("Mod Manager Download")

The green "Mod Manager Download" button on the file page is what the description tells users to click. Vortex catches the `nxm://` protocol handler the button generates. There's no special configuration needed on your end; Nexus generates this automatically once the file is uploaded and approved.
