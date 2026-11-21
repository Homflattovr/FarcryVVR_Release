# Far Cry 5 VR 1.0.4

An update to 1.0.3. It replaces 1.0.3 and keeps everything in it. Download `FC5VR_v1.0.4.zip` below and follow the
[install steps](https://github.com/Homflattovr/FarcryVVR_Release#install).

> **Strongly recommended: Virtual Desktop with its OpenXR runtime (VDXR).** SteamVR works but is not recommended.
>
> **If you have a problem with the hands or legs, change your skin (outfit) first.**

> **Please test and report:** the AMD fix and the frame generation fix below have not been tried in real play yet.
> If you have an AMD card, or use CheekyFoveatedDLSS's frame generation, tell us how it runs. Send `fc5vr_log.txt` from
> the game's `bin` folder (right after closing the game).

## Fixes

- **AMD (Radeon) cards: no more 2-5 fps in the world.** The mod read some of the game's graphics data in a way that
  is very slow on Radeon cards. It now keeps its own copy of that data. Not tried in real play yet.
- **Frame generation no longer freezes the game.** With CheekyFoveatedDLSS's frame generation the game could freeze
  for good: the mod and the frame generation waited on each other. Not tried in real play yet.
- **Menus are no longer cut off on 1440p screens.** On a 2560x1440 desktop the Skills menu and the loading prompt after
  Continue were cut off at the bottom and right.

## Changes

- **The game's reticle is turned off for you.** The game's crosshair stays fixed in the middle of the HUD and does
  not follow the gun in your hand. The launcher now turns it off when you press Play in VR, and the mod does it too
  when you start the game without the launcher. To keep it, add the line `reticleoff off` to `fc5vr_settings.txt`.
- **CheekyFoveatedDLSS loads with the mod.** If CheekyFoveatedDLSS is installed in the game's `bin` folder, the mod
  now loads it itself (before, it could fail to load next to this mod). To stop that, add `cheeky off` to
  `fc5vr_settings.txt`.
- **F8 no longer switches Stereo / AFW.** F8 is CheekyFoveatedDLSS's menu key. Switch the mode in
  **F11 > Display > Rendering** or in the launcher.
- **The launcher window can be resized.** Its settings and checks scroll, and the buttons stay at the bottom.
- **More guns sit right in your hand from the first draw:** built-in positions added for the compound bow, the
  recurve bow, the slingshot and a pipe, and grip points for six more weapons.

## Known issues

See [KNOWN_ISSUES.txt](https://github.com/Homflattovr/FarcryVVR_Release/blob/main/release/KNOWN_ISSUES.txt).
