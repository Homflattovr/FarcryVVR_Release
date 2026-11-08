# Far Cry 5 VR

An unofficial mod that turns Far Cry 5 into a full VR game: true stereo 3D through OpenXR, a weapon that sits in
your hand and shoots where the barrel points, reloading by hand for every kind of weapon, a bow you draw with your
own hands, body gestures to switch, sling and holster weapons and to throw grenades, a HUD that floats in front of
you, and comfort options for driving, flying and cutscenes.

> **Beta 0.4.** Expect some rough edges. Read [KNOWN_ISSUES.txt](release/KNOWN_ISSUES.txt) before you report something.

**[Download the latest release](https://github.com/Homflattovr/FarcryVVR_Release/releases)** (the `FC5VR_<version>.zip` file)

---

## Features

### Rendering
- **Native stereo through OpenXR.** Both eyes are rendered every frame from their own viewpoint, with the headset's
  own resolution and field of view. Full 6DoF head tracking, roomscale.
- **AFW mode** (early, experimental): one eye per frame, the other rebuilt from depth. Faster, with visible artifacts.
  Switch between Stereo and AFW with **F8**.
- **NVIDIA DLSS per eye**: Native (DLAA), Quality, Balanced or Performance, with a choice of DLSS model and sharpening.
- **DLSS on its own D3D12 queue**: DLSS now runs through NVIDIA's D3D12 path on a worker thread, so the game's render
  thread no longer waits for it. On by default, for a higher frame rate in Stereo.
- **Resolution multiplier** (x1.0 to x2.0) and a **field of view** slider that trades the lens edges for a sharper
  centre.
- **Far detail at the flat-screen level** (optional): keeps the game's level of detail as it is on a monitor.
- **Sun shadow detail** and **texture detail** settings: finer sun shadows (2048 by default, up to 4096) and sharper
  textures under DLSS (Sharper by default).
- **Clean picture in the headset**: no depth-of-field blur on your sights, no vignette or film grain that differs
  between the eyes, no chromatic aberration.
- **World scale** setting.
- Works with **OFXR Bridge** running (recommended, see below).

### Motion controls
- **The weapon is in your hand.** Shots leave along the barrel, with fast shot release so the gun fires the moment you
  pull the trigger, even while it moves.
- **Two-handed hold**: put the left hand on the fore-end and both hands line up on the gun. Let go with the right
  hand and the gun stays in your left hand (the right hand is then free to work a bolt).
- **Hand models** that follow the controllers, with fingers that close round what the hand holds (a magazine, a
  shell, the charging handle, the bow string), and an optional floating-weapon mode.
- **Sights that work**: shots go through the centre of the red dot, the scope or the iron sights, read from each gun's
  own aim-down-sights pose.
- **Red dot and laser**: realistic (a red dot only in reflex sights), laser (the default: a red dot in reflex sights,
  a laser dot on weapons without one) or arcade (red dot and laser on every weapon). Reflex sights show a holographic
  ring with ticks around the dot.
- **Magnified scopes**: bring a scope to your eye to see a magnified picture in the eyepiece. With two hands on the
  gun, the right stick zooms in steps (x1, x4, x8, x16, x32). An optional scope render pass keeps far detail sharp.
- **Steady weapon**: no running bob, sprint pose or recoil animation moving the gun in your hand (reloads still play).
  Weapon smoothing for shaky hands, made stronger while your hand is still so the sights settle, no recoil kick and
  no weapon sway.
- **Mounted guns and mortars** stay on their mount: aim with the right controller (inverted, like the handles of a
  gun on a pivot) or with your head, and the trigger fires.
- **Haptics**: the game's rumble on both hands (or only the hand holding the gun), plus a firm pulse on every shot
  (strength and length adjustable).
- **Bullet tracers** hidden by default (or from the muzzle), flamethrower flames that stay in the world, and fired
  bullets three times faster by default (x1 to x4).
- **Walking follows your head**, not the gun. Smooth or snap turning. Left-handed mode (not tested).

### Manual reloading
Every kind of weapon reloads by hand. Your spare magazines, shells, rounds, belts and rockets sit at your **left
hip**: take one with the left grip. The part to work next glows on the gun's own model (only when the gun needs you),
each step plays the game's own reload sound, and the ammo count shows under your wrist.

- **Magazine guns** (rifles, SMGs, pistols): a quick press of right **B** drops the magazine. Take the next from your
  hip, bring it under the gun where its ghost shows and push it in until it clicks. If you fired every round, rack
  the charging handle with the left grip (a pistol: pull back its slide, held open when empty). Racking with a round
  in costs a round.
- **Pump shotguns**: push shells one at a time into the loading port under the gun, and work the pump with the left
  hand (pull back, push forward) to chamber one and after every shot.
- **Lever actions** (1887, 1888, repeaters): shells into the port the same way. With the left hand on the fore-end,
  drop the right hand about 5 cm and bring it back to work the lever.
- **Bolt actions** (.308, HTI .50): after every shot grip the bolt, raise the hand to turn it up, pull it back (the
  case flies out), push it forward and turn it down. The bolt follows your hand at every step.
- **Revolvers**: quick B swings the cylinder out and the fired cases drop out. Push a round into each empty chamber.
- **Double barrels** (Sledge, sawed-off): quick B breaks the gun open; load a shell into each barrel.
- **Belt-fed guns** (M60, M249): quick B drops the belt and lifts the feed cover. Push a new belt into the tray, push
  the cover down with your hand, then rack the handle.
- **Launchers**: the M79 breaks open with B and takes a grenade into the breech; the RPG-7 takes a rocket pushed into
  the front of the tube; the RAT4 opens at the back with B and takes a missile there. The flamethrower takes a new
  canister.
- **Closing**: what B opened closes with a second B, or with a flick of the gun (sideways for a revolver; the RAT4
  closes with B or the left grip at the back only).
- **Other guns** reload by hand as magazine guns, their magazine learnt from the game's own reload the first time.
- The reload data for the game's weapons ships with the mod, so the guns work from the first reload.
- **Left Y still reloads the game's way** if you prefer (Keep the automatic reload button, on by default), and a gun
  is never left unable to fire when the game still has rounds for it.

### Gestures
- **Sling**: your main weapon hangs across your chest. Grab it with the right grip to hold it, let go to put it back.
- **Shoulder weapon slots**: reach to your right or left shoulder and press the grip to draw a weapon slot (left
  shoulder slot 1, right shoulder slot 2 by default; each can take any of the slots 1 to 4). The new weapon is in your
  hand straight away.
- **Holster** (on by default): your sidearm on the right hip, grabbed with the right grip.
- **Throwables** (on by default): the grenade, molotov or other throwable the game has selected sits on your chest,
  with its own model. Grab it with the right grip, aim with the trajectory arc, let go of the grip to throw. Its place
  and size can be set.
- **Melee by swinging** (experimental, off by default, work in progress).
- Every gesture zone can be moved, resized, shown in the headset to check where it is, and reset to its default.
  A light buzz tells you when your hand is in a zone.

### Bow
- **Draw the bow with your hands.** The bow is in your left hand and your right shoulder becomes a quiver: right grip
  at the right shoulder takes an arrow. Bring it to the bow just above your left hand, pull the string back (a short
  buzz at full draw) and let go of the grip to shoot. Let go before half draw and no arrow flies.
- **The bow on the sling**: it hangs there like your other weapons. Take it with the left grip anywhere along it.
  The right shoulder is a quiver only while your left grip holds the bow; otherwise it gives you that shoulder's
  weapon slot as usual.
- **Aiming**: drawn, the arrow goes from your eye through the bow's sight pin, and a red dot shows where it will land,
  in your aiming eye only, once an arrow is nocked. The drawn bow and arrow are held steady against small shakes.
- Bows are found from the game's own data. Each bow stands upright in your hand, and its fit is kept between sessions.

### HUD and menus
- **HUD on its own panel** in front of you, with adjustable distance, width and height (or in the image, or hidden).
- **Ammo on the wrist**: rounds in the gun, what it holds and rounds left, under the weapon hand's wrist.
- **World markers on their targets**: objective and waypoint markers leave the HUD panel and sit on what they point at,
  in both eyes, with size limits and smoothing.
- **Menus, the map and loading screens** on a flat screen in the room. The map stays usable with the controllers.
- **Binoculars** magnify the view, as on a monitor.
- **B closes tutorial pop-ups.**
- A **"VR mod loading" popup** after each load (10 s at most), until the mod has finished setting up your weapons
  (the launcher can skip it).

### Vehicles, flying and comfort
- **The view turns with the vehicle**: the cabin stays put around you and the road turns. When flying (helicopters,
  planes, parachute, wingsuit) the view also climbs, dives and banks with the aircraft while you look around freely.
  You can also keep the horizon always level, or turn the view only with the stick.
- **Cars, quads, bikes and boats on A and X**: right A accelerates, left X brakes and reverses while held, and a
  quick double press of X (held) is the handbrake. The triggers, grips and right stick no longer drive.
- **Shoot while you drive**: the right trigger fires your sidearm (you stay the driver). Your hands are shown and
  follow the controllers, the right one holding your sidearm; press a grip near the wheel, handlebars or stick to put
  that hand on it. The rest of your body stays hidden.
- **Aircraft**: left trigger throttle (helicopters: up), left grip throttle down (helicopters: down). The right stick
  turns a helicopter and pitches its nose; in a plane it does nothing, so the view is yours.
- **Facing the front**: getting into a plane you face its nose, also with a plane parked at rest, and the game's aim
  is turned to the front of a car or quad when you get in.
- **Gesture driving** (experimental, off by default): hold the wheel, handlebars or stick with a grip and turn it.
- An automatic **recenter** after vehicles and loading screens, and a few seconds after the game starts (during the
  intro credits); each recenter also brings the menus and the F11 menu in front of you. Optional comfort settings: a
  **vignette** in vehicles and a **fade to black** while getting in or out.
- **Cutscenes** on a flat screen in the room (default), or in 3D with the horizon held level. Takedowns stay in 3D:
  a cutscene moves to the screen once it has lasted 6 s.
- **Smooth walking and running**, a **body-based view** that removes the game's head bob, and parachute and fall
  handling that hands the view back as soon as you land.

### Launcher
- **FC5VR_Launcher.exe** picks VR or flat screen, Stereo or AFW, the resolution multiplier, DLSS, cutscenes and your
  headset.
- It checks the game options the mod needs and offers to set them for you.
- It warns about a missing OpenXR runtime, a missing ViGEmBus driver, an old Visual C++ runtime or other mods' DLLs
  that would stop VR from starting.
- **Add FSR...** installs FSR (through OptiScaler) for cards without DLSS from OptiScaler's own download, and
  **Remove FSR** removes it: see [Optional: FSR instead of DLSS](#optional-fsr-instead-of-dlss-amd-intel-or-gtx-cards-experimental).
- If the game fails to start in VR or crashes, it offers a **report zip** (log, crash dump, system details) for you to
  send. Nothing is uploaded.

---

## Requirements

- **Far Cry 5** on PC: the Steam copy or the Ubisoft Connect copy. An Epic Games Store copy runs through Ubisoft
  Connect and works the same way.
- **Windows 10 or 11**, 64-bit.
- **An NVIDIA RTX graphics card** (the mod uses DLSS). Other cards can try FSR instead: see *Optional: FSR instead of DLSS* below.
- **A PC VR headset with an OpenXR runtime set as the system default.** Tested: Meta Quest 3 with Virtual Desktop and
  VDXR (Virtual Desktop Streamer > OpenXR Runtime: VDXR). Quest Link / Air Link, Pimax Play and other OpenXR
  runtimes should work but are untested; avoid SteamVR. The launcher has presets for Quest 2, 3, 3S and Pro, Valve Index, Vive Pro 2,
  Reverb G2, Bigscreen Beyond and Pimax Crystal.
- **The ViGEmBus driver** (the controllers reach the game as a virtual Xbox pad):
  [ViGEmBus 1.22.0](https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe)
- **The Microsoft Visual C++ 2015-2022 x64 runtime** (`vc_redist.x64.exe` from Microsoft), if it is not already
  installed. The launcher tells you.

For reference, on an RTX 4090: about 50 fps in Stereo and 80+ in AFW.

### Strongly recommended: OFXR Bridge and a high-bitrate codec

Far Cry 5 is full of trees, grass and bushes, and foliage is what suffers most in VR: a low frame rate makes it judder,
and a compressed stream turns it into a blurry, blocky smear.

- **Use [OFXR Bridge](https://github.com/tig3rmast3r/OFXR-Bridge/releases)** (frame generation for OpenXR games).
  The mod works with it, and it makes the game far smoother in the headset.
  Tested together with the D3D12 DLSS path (on by default): it runs well.
- **Stream with a high-bitrate codec** if your headset is wireless or streams over a cable (Quest with Virtual Desktop
  or Quest Link). In Virtual Desktop: **H.264+ at 300-400 Mbps** if your Wi-Fi holds it, or **AV1 10-bit at about
  200 Mbps**, with **automatic bitrate turned off**. Lower bitrates and the default codecs blur the foliage badly.

### Optional: FSR instead of DLSS (AMD, Intel or GTX cards), experimental

The mod can run AMD FSR in place of DLSS through **OptiScaler**, a separate free tool. It is meant for graphics cards
without DLSS. It has been tested only on an NVIDIA card so far, so please report how it works on yours.

1. **Download OptiScaler 0.9.4** (the version tested with the mod): on
   [its release page](https://github.com/optiscaler/OptiScaler/releases/tag/v0.9.4), under *Assets*, the `.7z` file
   (52 MB). Leave it in your Downloads folder.
2. **In `FC5VR_Launcher.exe`, press Add FSR...** next to *Upscaler*, with the game closed. The launcher finds the
   download, unpacks it and installs it in the game's `bin` folder. Nothing else to copy or type.
3. **Pick the Upscaler** (also in F11 > Display > Image quality, from the next game start):
   **FSR 3.1** for every card (the default), **FSR through FFX** for FSR 4 on Radeon RX 9000 cards (heavier),
   **FSR 2.2** if FSR 3.1 misbehaves, or **DLSS** (NVIDIA RTX). Keep DLSS ticked: FSR uses its render scale
   (Native is the sharpest, Quality is faster).

**Remove FSR** in the launcher deletes exactly the files it installed. If the launcher cannot unpack the `.7z`
(Windows 10 without 7-Zip), extract it yourself (7-Zip, or right-click > *Extract All* on Windows 11), press
**Add FSR...** and choose `OptiScaler.dll` in the extracted folder. With OptiScaler installed, OFXR Bridge's DLSS
motion vectors do not work: use OFXR without them, or pick DLSS.

---

## Install

> **Before you start**
> - **Play with the basic (default) outfit** until the mod works, and only then switch to another one. Other outfits
>   can leave the hands missing or wrong.
> - **Avoid SteamVR.** Use your headset's own OpenXR runtime instead (VDXR for Virtual Desktop, Meta Quest Link, or
>   Pimax Play).

1. **Install the requirements above** (ViGEmBus, your OpenXR runtime as the default). We strongly recommend
   [OFXR Bridge](https://github.com/tig3rmast3r/OFXR-Bridge/releases) and a high-bitrate codec (H.264+ at 300-400 Mbps
   or AV1 10-bit at about 200 Mbps, automatic bitrate off): see
   [Strongly recommended](#strongly-recommended-ofxr-bridge-and-a-high-bitrate-codec).

2. **Find the game folder**: the folder that holds the `bin` and `data_final` folders (`FarCry5.exe` is in `bin`).
   - **Steam**: right-click Far Cry 5 in the library > Manage > Browse local files.
     Usually `C:\Program Files (x86)\Steam\steamapps\common\FarCry5`
   - **Ubisoft Connect**: Games > Far Cry 5 > Properties > Local files: Open folder.
     Usually `C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Far Cry 5`
   - **Epic Games Store**: Library > Far Cry 5 > ... > Manage > Installation.
     Usually `C:\Program Files\Epic Games\FarCry5`

3. **Extract the zip into the game folder.** The zip holds one folder, `bin`, laid out like the game's: let Windows
   merge it with the game's `bin` folder and replace the old files if asked. Every file lands next to `FarCry5.exe`:

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

   **Updating** from an older version: replace every file, including `fc5vr_settings.txt` (it holds the
   recommended settings for this version, and the manual reload data for the weapons). An old settings file keeps its
   old values, such as throwables and the holster off. If it is missing, the mod writes the same settings at its
   first start.

4. **Start the game with `FC5VR_Launcher.exe`** (a desktop shortcut to it is handy). Pick:
   - **Mode**: VR, or Flatscreen (the normal game, mod off).
   - **Rendering**: Stereo (sharpest, recommended) or AFW (faster, experimental).
   - **Resolution multiplier**: start at x1.0.
   - **DLSS**: keep it on. Native (DLAA) is the sharpest; Quality, Balanced and Performance are faster.
   - **Cutscenes**: Flat screen in VR (recommended), Full stereo VR (expect visual bugs), or Unchanged (set in F11).
   - **Headset**: yours.

   Press **Play**. Keep Steam or Ubisoft Connect running. After the first time, starting the game the usual way also
   starts the mod with your last launcher choices.

### Game options the mod needs

The launcher checks these when you press Play in VR and offers to set them (it keeps a backup of your options file,
`Documents\My Games\Far Cry 5\gamerprofile.xml.fc5vr_backup`). You can also set them in the game's Options:

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
| Reticle (Interface) | Off |
| Controls | The game's default gamepad layout |

---

## The F11 menu

Press **F11** on the keyboard, or **click both sticks** in the headset, to open the mod's menu. It shows in the
headset and on the monitor. In the headset, point with the controller and press **A** or the **trigger**; the right
stick scrolls, and left / right nudges the slider under the pointer.

At the top, choose how many settings are shown: **Basic**, **Advanced** or **Debug**. Every change is saved at once
in `fc5vr_settings.txt`. The tabs:

| Tab | What is in it |
| --- | --- |
| **Display** | Stereo or AFW, field of view, far detail, sun shadow detail, texture detail, DLSS (on / off, render scale, model, sharpening), resolution multiplier, picture clean-ups, world scale |
| **HUD and menus** | Where this menu hangs in the headset, where the HUD goes and its size, ammo on the wrist, world markers, tutorial pop-ups, the loading popup, menus / map / loading screens, binoculars |
| **Gestures** | Gesture zones and buzz, shoulder weapon slots, sling, holster, **Throwable and pouch**, **Bow**, **Manual reload**, melee. Throwable and pouch, Bow and Manual reload show their main switches first and keep the rest under their own **Debug** section |
| **Motion control** | Motion controls on / off, left-handed, hand models, hip-fire spread, shot release, recoil and sway, walking direction |
| **Weapon** | Smoothing and steadier sights, vibration, bullet tracers and flames, bullet speed, sights, red dot and laser, scopes, mounted weapons, the weapon's angle on the controller |
| **Comfort** | Frame pacing, smooth walking, body-based view, snap or smooth turning, the gun's animation, cutscenes, vehicles, flying, recentering, parachutes |
| **Layout** | Vehicle controls (A and X driving, the plane's right stick, hands while driving, the sidearm in hand), and the full controller layout, on foot and in vehicles |
| **Experimental** | Unfinished features and tests, such as gesture driving (Advanced level) |
| **Debug** | Every technical switch, including the performance switches (Debug level) |

**Manual reload** has three switches on top: manual reload on / off, show hints (the part to work next lights up) and
keep the automatic reload button. Its Debug section holds the rest, including **The gun in hand**: what the mod knows
about the weapon you hold, with buttons to learn a part again from the game's own reload if something looks wrong.

### Keys

| Key | Action |
| --- | --- |
| **F11** | Open / close the menu (or click both sticks) |
| **F9** | Recenter the view |
| **F8** | Switch Stereo / AFW |
| **F7** | Motion controls on / off |
| **F12** | Flat screen on / off (or left grip + left stick click) |

Holding the Meta button (or Virtual Desktop's recenter) also recenters the game.

---

## Controls

The mod uses the game's default gamepad layout. The **Layout** tab of the F11 menu always shows the current one.

**Right controller**

| Button | On foot | Driving |
| --- | --- | --- |
| Trigger | Fire | Shoot (your sidearm, or the vehicle's guns) |
| Grip | Hold the weapon (from the sling, a shoulder, the hip or the chest) | Near the wheel or stick: hand on it; away from it: ready your sidearm (then the trigger fires at once) |
| A | Jump | Cars, quads, bikes, boats: accelerate; aircraft: change seat |
| B | Quick press: manual reload (drop the magazine, open the gun); hold: heal; closes tutorial pop-ups | Hold: heal |
| Stick | Turn; in the weapon wheel: pick a slot | Nothing (you look with your head, planes too); helicopter: turn, up / down pitches the nose |
| Stick click | Melee / takedown | Horn |

**Left controller**

| Button | On foot | Driving |
| --- | --- | --- |
| Trigger | Weapon wheel (hold, pick with the right stick) | Aircraft: throttle (helicopters: up); cars, quads, bikes, boats: nothing |
| Grip | Modifier (see below), hold the weapon with two hands, take ammo from the left hip, work a handle, pump or bolt, take the bow | Aircraft: throttle down (helicopters: down); near the wheel or stick: hand on it |
| X | Crouch / slide | Brake / reverse (held); a quick double press, held: handbrake |
| Y | Reload (the game's reload) / hold: use | Hold: get in / out |
| Stick | Move | Steer |
| Stick click | Sprint | Auto-drive |
| Menu button | Short press: map; hold half a second: pause menu | The same |

Right B no longer switches weapon: switch at your shoulders or with the weapon wheel.

**Left grip pressed at your left ear / eye, then held**

| With | Action |
| --- | --- |
| Either stick | D-pad: up binoculars, left / right companion orders, down throw a rock (driving: right stick only, up = radio) |
| Right B | Throw (aircraft: rockets) |
| Menu button | Map (also a short press of the menu button alone) |
| Left stick click | Flat screen on / off |

A left grip pressed anywhere else (the hip, the chest, the weapon, the wheel) never counts as the modifier.

**Gestures** (on foot):
- Right hand on the slung weapon + grip = weapon in hand.
- Right hand at a shoulder + grip = that shoulder's weapon slot.
- Right hand at the right hip + grip = sidearm.
- Right hand at the chest + grip = throwable; aim with the arc and let go to throw.
- Left hand at the left hip + grip = magazine, shell, round, belt or rocket for a manual reload.
- With a bow: left grip on the slung bow = bow in hand; then right hand at the right shoulder + grip = an arrow.

---

## Uninstall

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

## If something goes wrong

- **The game does not start in VR** (it closes, stays black or starts flat): start it with `FC5VR_Launcher.exe` and
  read the warnings under Headset first. If the start fails, the launcher offers to save `FC5VR_report_<date>.zip` in
  the `bin` folder; its **Make a report** button makes one at any time.
- **Blurry picture on the very first start**: restart the game once.
- **A gun reloads wrongly by hand**: F11 > Gestures > Manual reload > Debug > The gun in hand shows what the mod knows
  about it and can learn a part again. Left Y always reloads the game's way.
- **Reporting a problem**: say what you did, Stereo or AFW, add a short video if you can, and `fc5vr_log.txt` from
  the `bin` folder (right after closing the game), or the report zip.

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
