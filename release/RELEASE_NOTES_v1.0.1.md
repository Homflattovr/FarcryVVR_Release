# Far Cry 5 VR 1.0.1: hotfix

A hotfix for 1.0. It replaces 1.0 and keeps everything in it. Download `FC5VR_v1.0.1.zip` below and follow the
[install steps](https://github.com/Homflattovr/FarcryVVR_Release#install).

> **Strongly recommended: Virtual Desktop with its OpenXR runtime (VDXR).** SteamVR works but is not recommended.
>
> **If you have a problem with the hands or legs, change your skin (outfit) first.**

## Changes

- The F11 test section is gone (the Tests tab).
- Foveated DLSS ("DLSS on the centre only") is now under **F11 > Experimental**. It is still off by default.

## New defaults

The defaults are now the author's own settings:

- Full body and arms on (F11 > Main > You see).
- Realistic red dot: red dots only in reflex sights, no laser.
- Tracers start at your gun's barrel.
- Gesture driving on: hold the wheel, handlebars or stick with a grip.
- Room scale on, and turning with your head.
- Melee hits on (still experimental), with the game's physics rays.
- Mounted guns aim with your head.
- Sun shadows at 3072 with small contact shadows, and harder shadow edges.
- The HUD a little further and higher, wider, and objective markers shown further away.
- Cutscenes follow your heading later and more slowly; shorter fades into menus and a faster return to 3D after them.
- Weapon smoothing 0.31 and vibration 1.90.
- F11 opens at the Debug level (all tabs shown).
- The author's learnt data for more weapons: manual reload positions and sounds, sight lines, scopes and their zoom.
- The throw, shoulder, sidearm and sling zones as the author uses them.

**To get the new defaults, replace your `fc5vr_settings.txt`**: delete (or rename) `fc5vr_settings.txt` in the
game's `bin` folder before you start the game. The mod then writes a fresh one with the new defaults. Your own
settings (including your calibrations) are in the old file if you want to copy lines back.

## Known issues

See [KNOWN_ISSUES.txt](https://github.com/Homflattovr/FarcryVVR_Release/blob/main/release/KNOWN_ISSUES.txt).
