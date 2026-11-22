# Far Cry 5 VR

An unofficial mod that turns Far Cry 5 into a full VR game: true stereo 3D through OpenXR, a weapon that sits in
your hand and shoots where the barrel points, reloading by hand for every kind of weapon, a bow you draw with your
own hands, a full body (on by default), body gestures to switch, sling and holster weapons and to throw grenades, your
hands on the real steering wheel, a HUD that floats in front of you, and comfort options for driving, flying and
cutscenes.

**[Download version 1.0.5](https://github.com/Homflattovr/FarcryVVR_Release/releases/latest)** (the `FC5VR_v1.0.5.zip` file)

**Support this project**: https://ko-fi.com/homard47885

> **Strongly recommended: Virtual Desktop with its OpenXR runtime (VDXR).** The mod is built and tested on
> Virtual Desktop with VDXR. Other OpenXR runtimes should work; SteamVR works but is not recommended.
>
> **If you have a problem with the hands or legs, change your skin (outfit) first.**

---

## Install

### 1. Install the requirements

- **Far Cry 5** on PC: the Steam copy or the Ubisoft Connect copy. An Epic Games Store copy runs through Ubisoft
  Connect and works the same way.
- **Windows 10 or 11**, 64-bit.
- **An NVIDIA RTX graphics card** (the mod uses DLSS). Other cards can try FSR instead: see
  [Optional: FSR instead of DLSS](#optional-fsr-instead-of-dlss-amd-intel-or-gtx-cards-experimental).
- **A PC VR headset with an OpenXR runtime set as the system default.** We strongly recommend
  **[Virtual Desktop](https://www.vrdesktop.net/) with its own OpenXR runtime, VDXR** (Virtual Desktop Streamer >
  OpenXR Runtime: **VDXR**). That is what the mod is made and tested with (Meta Quest 3). Meta Quest Link, Pimax Play
  and other OpenXR runtimes should work but are untested. SteamVR works but is not recommended.
- **The ViGEmBus driver** (the controllers reach the game as a virtual Xbox pad):
  [ViGEmBus 1.22.0](https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe)
- **The Microsoft Visual C++ 2015-2022 x64 runtime** (`vc_redist.x64.exe` from Microsoft), if it is not already
  installed. The launcher tells you.

Also strongly recommended (see [below](#strongly-recommended-virtual-desktop-vdxr-ofxr-bridge-and-a-high-bitrate-codec)):
[OFXR Bridge](https://github.com/tig3rmast3r/OFXR-Bridge/releases) for frame generation, and a high-bitrate codec
in Virtual Desktop (H.264+ at 300-400 Mbps or AV1 10-bit at about 200 Mbps, automatic bitrate off).

### 2. Find the game folder

The folder that holds the `bin` and `data_final` folders (`FarCry5.exe` is in `bin`).
- **Steam**: right-click Far Cry 5 in the library > Manage > Browse local files.
  Usually `C:\Program Files (x86)\Steam\steamapps\common\FarCry5`
- **Ubisoft Connect**: Games > Far Cry 5 > Properties > Local files: Open folder.
  Usually `C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Far Cry 5`
- **Epic Games Store**: Library > Far Cry 5 > ... > Manage > Installation.
  Usually `C:\Program Files\Epic Games\FarCry5`

### 3. Extract the zip into the game folder

The zip holds one folder, `bin`, laid out like the game's: let Windows merge it with the game's `bin` folder and
replace the old files if asked. Every file lands next to `FarCry5.exe`:

| File | What it is |
| --- | --- |
| `dxgi.dll` | The mod |
| `openxr_loader.dll` | OpenXR loader |
| `nvngx_dlss.dll` | NVIDIA DLSS runtime |
| `FC5VR_Launcher.exe` | The launcher |
| `fc5vr_settings.txt` | The recommended mod settings |
| `FC5VR_README.txt`, `KNOWN_ISSUES.txt`, `ACKNOWLEDGMENTS.txt`, `LICENSE.txt` | To read |

**Check:** `dxgi.dll` and `FC5VR_Launcher.exe` must sit next to `FarCry5.exe`. If you end up with
`bin\bin\dxgi.dll`, move the files of the inner `bin` up one level. Under `C:\Program Files` Windows asks for
administrator permission: allow it.

**Updating** from an older version: replace every file, including `fc5vr_settings.txt` (it holds the recommended
settings for this version and the manual reload data for the weapons). An old settings file keeps its old values. If
it is missing, the mod writes the same settings at its first start.

### 4. Start the game with `FC5VR_Launcher.exe`

A desktop shortcut to it is handy. Pick:
- **Mode**: VR, or Flatscreen (the normal game, mod off).
- **Rendering**: Stereo (sharpest, recommended) or AFW (faster, experimental).
- **Resolution multiplier**: start at x1.0.
- **DLSS**: keep it on. Native (DLAA) is the sharpest; Quality, Balanced and Performance are faster.
- **Cutscenes**: Full stereo VR (the default) or Flat screen in VR.
- **Headset**: yours.

Press **Play**. Keep Steam or Ubisoft Connect running. The launcher checks the game options the mod needs and offers
to set them for you (see [Game options the mod needs](#game-options-the-mod-needs)). After the first time, starting
the game the usual way also starts the mod with your last launcher choices.

### 5. In the game

- Press **F11** (or click both sticks) for the mod's menu. Its **Main** tab has the choices most players want:
  rendering, cutscenes, sights, hands only or full body, turning.
- A short **weapon tips** popup shows the first time you pick up a gun: where your weapons are on your body and how to
  reload by hand.
- **If you have a problem with the hands or legs, change your skin (outfit) first.**
- If the picture looks blurry on the very first start, restart the game once.

### Uninstall

In the game's `bin` folder, delete:
- `dxgi.dll`, `openxr_loader.dll`, `nvngx_dlss.dll`, `FC5VR_Launcher.exe`
- `FC5VR_README.txt`, `KNOWN_ISSUES.txt`, `ACKNOWLEDGMENTS.txt`, `LICENSE.txt`
- every file and folder whose name starts with `fc5vr_` (settings, log, crash logs, learned gun poses) and any
  `FC5VR_report_*.zip`
- `vr_config.txt` and `ref_ui.ini`
- if you added FSR: press **Remove FSR** in the launcher first (it deletes OptiScaler's files)

The game is then back to normal. If you let the launcher change your game options, set them back in the game's
Options, or restore `Documents\My Games\Far Cry 5\gamerprofile.xml` from `gamerprofile.xml.fc5vr_backup`.

---

## Strongly recommended: Virtual Desktop (VDXR), OFXR Bridge and a high-bitrate codec

Far Cry 5 is full of trees, grass and bushes, and foliage is what suffers most in VR: a low frame rate makes it judder,
and a compressed stream turns it into a blurry, blocky smear.

- **Use Virtual Desktop with VDXR as the OpenXR runtime.** It is the setup the mod is developed and tested on.
- **Use [OFXR Bridge](https://github.com/tig3rmast3r/OFXR-Bridge/releases)** (frame generation for OpenXR games).
  The mod works with it, and it makes the game far smoother in the headset.
- **Stream with a high-bitrate codec.** In Virtual Desktop: **H.264+ at 300-400 Mbps** if your Wi-Fi holds it, or
  **AV1 10-bit at about 200 Mbps**, with **automatic bitrate turned off**. Lower bitrates and the default codecs blur
  the foliage badly.

For reference, on an RTX 4090: about 50 fps in Stereo and 80+ in AFW.

## Optional: FSR instead of DLSS (AMD, Intel or GTX cards), experimental

The mod can run AMD FSR in place of DLSS through **OptiScaler**, a separate free tool. It is meant for graphics cards
without DLSS. It has been tested only on an NVIDIA card so far, so please report how it works on yours.

1. **Download OptiScaler 0.9.4** (the version tested with the mod): on
   [its release page](https://github.com/optiscaler/OptiScaler/releases/tag/v0.9.4), under *Assets*, the `.7z` file
   (52 MB). Leave it in your Downloads folder.
2. **In `FC5VR_Launcher.exe`, press Add FSR...** next to *Upscaler*, with the game closed. The launcher finds the
   download, unpacks it and installs it in the game's `bin` folder.
3. **Pick the Upscaler** (also in F11 > Display > Image quality, from the next game start):
   **FSR 3.1** for every card (the default), **FSR through FFX** for FSR 4 on Radeon RX 9000 cards (heavier),
   **FSR 2.2** if FSR 3.1 misbehaves, or **DLSS** (NVIDIA RTX). Keep DLSS ticked: FSR uses its render scale.

**Remove FSR** in the launcher deletes exactly the files it installed. If the launcher cannot unpack the `.7z`
(Windows 10 without 7-Zip), extract it yourself, press **Add FSR...** and choose `OptiScaler.dll` in the extracted
folder. With OptiScaler installed, OFXR Bridge's DLSS motion vectors do not work: use OFXR without them, or pick DLSS.

## Game options the mod needs

The mod sets these for you each time the game starts in VR, also without the launcher; **Play in VR** in the
launcher sets them too. Your other options are kept, and the first time a backup of your options file is kept
(`Documents\My Games\Far Cry 5\gamerprofile.xml.fc5vr_backup`). To keep your own values, add the line
`gameoptions off` to `fc5vr_settings.txt` and set them in the game's Options:

| Option | Value |
| --- | --- |
| Window mode | Borderless |
| Field of view | 120 |
| Anti-aliasing | SMAA |
| V-Sync | Off |
| Frame rate limit | Off |
| Motion blur | Off |
| Resolution scale | 100 %, dynamic resolution off |
| HDR | Off |
| Reticle (Interface) | Off (the mod and the launcher turn it off for you in VR) |
| Controls | The game's default gamepad layout |

## If something goes wrong

- **Hands or legs missing or wrong**: change your skin (outfit) first.
- **The game does not start in VR** (it closes, stays black or starts flat): start it with `FC5VR_Launcher.exe` and
  read the warnings under Headset and the install checks first. If the start fails, the launcher offers to save
  `FC5VR_report_<date>.zip` in the `bin` folder; its **Make a report** button makes one at any time.
- **Upside-down picture with SteamVR**: the mod now detects it and corrects it, but Virtual Desktop with VDXR is the
  recommended setup.
- **Blurry picture on the very first start**: restart the game once.
- **A gun reloads wrongly by hand**: F11 > Gestures > Manual reload > Debug > The gun in hand shows what the mod knows
  about it and can learn a part again. Left Y always reloads the game's way.
- **Reporting a problem**: say what you did, Stereo or AFW, add a short video if you can, and `fc5vr_log.txt` from
  the `bin` folder (right after closing the game), or the report zip. Read [KNOWN_ISSUES.txt](release/KNOWN_ISSUES.txt)
  first.

---

## Features

### Rendering
- **Native stereo through OpenXR.** Both eyes are rendered every frame from their own viewpoint, with the headset's
  own resolution and field of view. Full 6DoF head tracking.
- **Culling from your eyes**: the game decides what to draw from where your eyes are, so no missing ground or edges
  when you look down, roll your head or sit in a vehicle.
- **NVIDIA DLSS per eye** (Native/DLAA, Quality, Balanced, Performance) on its own D3D12 queue, with a choice of
  model and sharpening; FSR through OptiScaler for other cards.
- **AFW mode** (experimental): one eye per frame, the other rebuilt from depth. Faster, with visible artifacts.
  Switch between Stereo and AFW in **F11 > Display > Rendering** (or in the launcher).
- **Resolution multiplier**, field of view, sun shadow and texture detail, a scope quality preset, and a clean picture
  in the headset (no depth of field on your sights, no vignette, grain or chromatic aberration).
- Fixes for VR-only artifacts such as the black squares in the lighting and the doubled objective markers.

### Hands, body and motion controls
- **The weapon is in your hand.** Shots leave along the barrel, through the centre of the red dot, scope or iron
  sights. Two-handed hold, recoil (on by default, tunable), gunstock calibration for controllers or a physical
  gunstock, weapon smoothing, magnified scopes with stepped zoom, red dot and laser modes, haptics on every shot.
- **Full body** (on by default; F11 > Main > You see: Hands only or Full body and arms): your body, arms and legs drawn under you, with a T-pose
  calibration popup to fit your arm length and height, the body seated in vehicles, and your sling, holster and
  pouch worn on the drawn body. Hands only is the other choice.
- **Hand models** whose fingers close round what they hold: the gun, a magazine, the bow string, the steering wheel.
- **Melee with the real weapon in your hand** (experimental): swing it and the hit lands at the contact.
- **Mounted guns, vehicle turrets and mortars**: aim with your hands on the handles, two-handed, a 360 turret turns
  with you.
- **Walking follows your head**, smooth or snap turning, smoothing over small hops and bodies, an optional room
  scale mode (test), left-handed mode.

### Manual reloading
Every kind of weapon reloads by hand. Your spare magazines, shells, rounds, belts and rockets sit at your **left
hip**: take one with the left grip. The part to work next glows on the gun's own model, each step plays the game's
own reload sound, the ammo count shows under your wrist, and every round of a magazine fires.

- **Magazine guns**: a quick right **B** drops the magazine; bring the next one under the gun and push it in; rack
  the charging handle or the pistol's slide if you emptied it.
- **Pump shotguns**: shells one at a time into the loading port, then work the pump.
- **Lever actions, bolt actions, revolvers, double barrels, belt-fed guns, launchers and the flamethrower**: each
  with its own steps, following your hands.
- **Left Y still reloads the game's way** if you prefer.

### Gestures and the bow
- **Sling** across your chest, **shoulder weapon slots**, a **holster** on the right hip and **throwables** on your
  chest (aim with the arc, let go of the grip to throw).
- **Draw the bow with your hands**: arrows from the quiver at your right shoulder, a red dot where the arrow lands,
  the bow on the sling.
- Every gesture zone can be moved, resized, shown in the headset and reset. A light buzz tells you when your hand is
  in a zone.

### Vehicles and flying
- **Your hands on the vehicle's own wheel, handlebars or stick**, with the fingers closed round it.
- **The view turns with the vehicle**; when flying it also climbs, dives and banks while you look around freely.
- **A and X drive** (accelerate, brake, reverse, handbrake); **shoot your sidearm while driving**; the right stick
  turns you as a passenger; the bomber view and its HUD work in the plane.
- Optional comfort: vignette, fade to black, automatic recenter after vehicles and loading screens.

### Cutscenes, HUD and menus
- **Cutscenes in 3D around you** (or on a flat screen), with the game's button prompts shown, the view recentred when
  a cutscene ends, and riding cutscene cameras followed smoothly.
- **HUD on its own panel**, **ammo on the wrist**, **world markers on their targets**.
- **Menus, the map and loading screens** on a flat screen in the room, usable with the controllers. The game's button
  prompts show your VR controller's buttons.
- **The F11 menu** with a **Main** tab for the common choices; every change is saved at once.

### Launcher
- **FC5VR_Launcher.exe** picks VR or flat screen, Stereo or AFW, the resolution multiplier, DLSS or FSR, cutscenes
  and your headset.
- An **install checks** panel, and a button that sets the game options the mod needs. The window can be resized.
- It warns about a missing OpenXR runtime, ViGEmBus driver or Visual C++ runtime, and other mods' DLLs.
- If the game fails to start in VR or crashes, it offers a **report zip** for you to send. Nothing is uploaded.

---

## The F11 menu

Press **F11** on the keyboard, or **click both sticks** in the headset. It shows in the headset and on the monitor.
In the headset, point with the controller and press **A** or the **trigger**; the right stick scrolls, and left /
right nudges the slider under the pointer. Every change is saved at once in `fc5vr_settings.txt`.

| Tab | What is in it |
| --- | --- |
| **Main** | The common choices: rendering, cutscenes, sights, hands only or full body, turning, reload highlight |
| **Display** | Stereo or AFW, field of view, detail, DLSS or FSR, resolution multiplier, picture clean-ups, world scale |
| **HUD and menus** | This menu's place, the HUD, ammo on the wrist, world markers, pop-ups, menus and map |
| **Gestures** | Gesture zones, shoulder slots, sling, holster, throwables, bow, manual reload, melee |
| **Motion control** | Hands, body and arms, shot release, recoil, walking direction, room scale |
| **Weapon** | Smoothing, vibration, tracers, sights, scopes, mounted weapons, the weapon's angle on the controller, gunstock |
| **Comfort** | Smooth motion, turning, cutscenes, vehicles, flying, recentering |
| **Layout** | The controller layout, on foot and in vehicles |

### Keys

| Key | Action |
| --- | --- |
| **F11** | Open / close the menu (or click both sticks) |
| **F9** | Recenter the view |
| **F7** | Motion controls on / off |
| **F12** | Flat screen on / off (or left grip + left stick click) |

Holding the Meta button (or Virtual Desktop's recenter) also recenters the game.

## Controls

The mod uses the game's default gamepad layout. The **Layout** tab of the F11 menu always shows the current one.

**Right controller**

| Button | On foot | Driving |
| --- | --- | --- |
| Trigger | Fire | Shoot (your sidearm, or the vehicle's guns) |
| Grip | Hold the weapon (from the sling, a shoulder, the hip or the chest) | Near the wheel or stick: hand on it; away from it: ready your sidearm |
| A | Jump | Cars, quads, bikes, boats: accelerate |
| B | Quick press: manual reload; hold: heal; closes tutorial pop-ups | Quick press: reload; hold: heal |
| Stick | Turn; in the weapon wheel: pick a slot | Helicopter: turn and pitch; passenger: turn |
| Stick click | Melee / takedown | Horn |

**Left controller**

| Button | On foot | Driving |
| --- | --- | --- |
| Trigger | Weapon wheel (hold, pick with the right stick) | Aircraft: throttle (helicopters: up) |
| Grip | Modifier at the ear, two-handed hold, ammo from the left hip, work a handle, pump or bolt, take the bow | Aircraft: throttle down; near the wheel or stick: hand on it |
| X | Crouch / slide | Brake / reverse (held); quick double press, held: handbrake |
| Y | Reload (the game's reload) / hold: use | Hold: get in / out |
| Stick | Move | Steer |
| Stick click | Sprint | Auto-drive |
| Menu button | Short press: map; hold: pause menu | The same |

**Left grip pressed at your left ear, then held**: either stick is the D-pad (binoculars, companion orders, rock),
right B throws, the menu button opens the map, left stick click switches the flat screen.

---

## Building from source

This repository holds the mod's source. See [BUILDING.md](BUILDING.md): Visual Studio 2022 Build Tools, Git and the
NVIDIA DLSS SDK, then `build.bat`. Releases are built by GitHub Actions ([build.yml](.github/workflows/build.yml)).
Changes per version are in [CHANGELOG.md](CHANGELOG.md).

## Credits and licence

Built on VR Framework (mutars, Fribur's fork) and starfield2vr, both based on praydog's REFramework, with ideas and
code from Dishonored VR, fc5-dlss, Luma Framework and CryHook5, and thanks to the Flat2VR community. The full list is
in [ACKNOWLEDGMENTS.txt](ACKNOWLEDGMENTS.txt).

The mod's code is under the MIT licence (`fc5vr/LICENSE`, `fc5vr/extern/vrframework/LICENSE`); each dependency keeps
its own licence. The NVIDIA DLSS runtime is distributed under NVIDIA's licence.

*Far Cry 5 is a trademark of Ubisoft. This is an unofficial fan mod, not made or endorsed by Ubisoft or NVIDIA.*
