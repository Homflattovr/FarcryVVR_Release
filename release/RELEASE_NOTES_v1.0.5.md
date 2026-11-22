# Far Cry 5 VR 1.0.5

An update to 1.0.4. It replaces 1.0.4 and keeps everything in it. Download `FC5VR_v1.0.5.zip` below and follow the
[install steps](https://github.com/Homflattovr/FarcryVVR_Release#install).

> **Strongly recommended: Virtual Desktop with its OpenXR runtime (VDXR).** SteamVR works but is not recommended.
>
> **If you have a problem with the hands or legs, change your skin (outfit) first.**

> **Please test and report:** the AMD fix below has not been tried on an AMD card yet. If you have one, tell us how
> it runs. Send `fc5vr_log.txt` from the game's `bin` folder (right after closing the game).

## Fixes

- **Vehicles: A keeps accelerating while you shoot with the sidearm out.** Driving and shooting, the car could stop
  accelerating once the sidearm was out, and A then did nothing for the rest of the drive, sometimes because of
  another character's gunshot nearby.
- **The right resolution with CheekyFoveatedDLSS.** With CheekyFoveatedDLSS installed, the game could stay at
  1920x1080 stretched into each eye. The mod now sets up its resolution before CheekyFoveatedDLSS is loaded.
- **AMD (Radeon) cards: a flat picture instead of 3D in 1.0.4.** Part of the 1.0.4 AMD and frame generation fixes
  also kept the game's own worker threads from getting each eye's camera. They get it again, as in 1.0.3. Not tried
  on an AMD card yet.
- **F11 settings that did not save now do:** Smooth turn speed, World scale, the weapon's Grip in the game's view,
  the flat screen (menus) distance and Left-handed are saved, and a few other F11 switches keep their value after a
  restart.

## Changes

- **The game options the mod needs are set at every VR start.** The mod sets them in the game's options file each
  time the game starts in VR, also when you start it without the launcher; Play in VR in the launcher sets them
  without asking. Your other options are kept. To keep your own values, add the line `gameoptions off` to
  `fc5vr_settings.txt`.
- **B buys at once in the store.** In the game's menus (store, pause menu, map) a quick B is the game's Y straight
  away, so vehicles and items are bought with one press.
- **New switch: Logs during sessions (slower)**, in F11 > Main and in the launcher. On (the default), the mod logs and
  measures everything while you play, for bug reports. Off, once you are in the game, only warnings and errors are
  logged, which is a little faster.

## Known issues

See [KNOWN_ISSUES.txt](https://github.com/Homflattovr/FarcryVVR_Release/blob/main/release/KNOWN_ISSUES.txt).
