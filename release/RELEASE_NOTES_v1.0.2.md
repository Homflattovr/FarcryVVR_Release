# Far Cry 5 VR 1.0.2: hotfix

A hotfix for 1.0.1. It replaces 1.0.1 and keeps everything in it (the new defaults included). Download
`FC5VR_v1.0.2.zip` below and follow the [install steps](https://github.com/Homflattovr/FarcryVVR_Release#install).

> **Strongly recommended: Virtual Desktop with its OpenXR runtime (VDXR).** SteamVR works but is not recommended.
>
> **If you have a problem with the hands or legs, change your skin (outfit) first.**

## Changes

- **Sights and scopes follow the optic you fit.** No learnt scope or sight data is shipped any more: each gun learns
  its own fitted optic the first time you aim down its sights. The mod remembers which optic each gun's data was
  learnt with. When you switch weapons or change the optic in the store or at a workbench, it checks the optic: the
  same one keeps its data; a different one is learnt again for the new optic (aim down sights once), and the old
  optic's data is kept for when you put it back.
- **A quick B reloads straight away.** A B press up to 0.4 s now counts as a reload (it was 0.25 s, so a slightly
  longer press healed instead). Holding B still heals.

**Updating from 1.0.1?** If you deleted `fc5vr_settings.txt` to get the 1.0.1 defaults, it holds the author's scope
and sight lines for his own optics. If a gun's sights or scope look off, remove the lines that start with
`scopecal`, `scopeglassat`, `scopezoom` and `sightcal` from `fc5vr_settings.txt` in the game's `bin` folder (game
closed); each gun then learns its own optic.

## Known issues

See [KNOWN_ISSUES.txt](https://github.com/Homflattovr/FarcryVVR_Release/blob/main/release/KNOWN_ISSUES.txt).
