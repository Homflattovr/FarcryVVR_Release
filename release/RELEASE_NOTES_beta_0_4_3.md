# Far Cry 5 VR Beta 0.4.3

Replaces Beta 0.4.2 and keeps everything in it. FSR comes first; the mod also gets a day of vehicle, menu and weapon fixes (below).

## New

- **FSR for graphics cards without DLSS (AMD, Intel, GTX), in a few clicks.** The mod can run AMD FSR in place of
  DLSS through [OptiScaler](https://github.com/optiscaler/OptiScaler), a separate free tool. The launcher now
  installs it for you:
  1. Download OptiScaler 0.9.4 from [its release page](https://github.com/optiscaler/OptiScaler/releases/tag/v0.9.4)
     (under *Assets*, the `.7z` file, 52 MB) and leave it in your Downloads folder.
  2. In `FC5VR_Launcher.exe`, press **Add FSR...** next to *Upscaler* (with the game closed). It finds the download,
     checks it, unpacks it and installs it in the game's `bin` folder with the settings the mod needs.
  3. Pick the Upscaler: **FSR 3.1** for every card, or **FSR through FFX** for FSR 4 on Radeon RX 9000 cards. Keep
     DLSS ticked: FSR uses the DLSS render scale.

  **Remove FSR** deletes exactly the files it installed. FSR is experimental and has been tested on an NVIDIA card
  only so far: please report how it runs on yours. With OptiScaler installed, OFXR Bridge's DLSS motion vectors do not
  work: use OFXR without them, or pick DLSS.

## Also in this release

Most of these were made from testers' reports today and are not all checked in a headset yet: please report anything odd.

- **Vehicles**
  - In a hard turn or a slide the view stays on the car's front instead of drifting to a side window.
  - Your hands show in vehicles and stay on the wheel, handlebars or stick when gripped.
  - Right grip away from the wheel readies your sidearm, and the right trigger then fires at once (no more draw-then-shoot lag).
  - A always accelerates and X brakes, all session; the left stick is never the throttle.
  - Quads get handlebars instead of a wheel; the helicopter's right stick is much softer and smoother; the gesture
    wheel answers sooner (F11 > Experimental > Gesture driving: "Steering boost").
  - With OFXR frame generation the dashboard and pillars no longer ghost (F11 > Hands and weapon: "In a vehicle: no
    ghosting of the cab").
- **Menus**
  - Menu button: a short press opens the map, holding it half a second opens the pause menu, everywhere.
  - In the game's menus the triggers change tab, and on the map the grips zoom.
  - The "VR mod loading" popup waits until the world is visible, shows once per load, and no longer comes back after
    the map or the pause menu.
  - The F11 menu keeps the same size in the headset at every resolution and DLSS setting.
- **Weapons**
  - Out of ammo, the game no longer switches weapon by itself: the gun stays in your hand on 0 until you pick up ammo.
  - The bow no longer ends up upside down after the game switches to it.
  - The weapon wheel picks with the right stick again when the game has switched its prompts to the keyboard, and the
    weapon on your sling stays grabbable after driving.
  - The ammo count on your wrist stays on the controller while sprinting.
  - The left grip works as the D-pad modifier (binoculars, orders, radio) only when pressed at your left ear / eye, so
    grabbing the wheel, a magazine or the fore-end no longer opens the binoculars.
- **DLSS with OFXR**: on the D3D12 DLSS path (the default) OFXR Bridge's DLSS frame generation gets the mod's data;
  opening the map or a load no longer makes DLSS rebuild itself or fall back to D3D11.
- **Sun shadows**: every sun shadow setting is now in F11 > Debug > Sun shadows, with new options for sharper edges,
  contact shadows and cascade distances (all at the game's own values by default).
## Changed

- The launcher no longer warns about OptiScaler's `winmm.dll` as another mod's DLL, and its report zip includes
  `OptiScaler.ini` and `OptiScaler.log` when FSR is installed.

## Install

Same as Beta 0.4: extract `FC5VR_beta_0_4_3.zip` into the game folder and replace every file.
