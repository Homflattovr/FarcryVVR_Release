# Far Cry 5 VR Beta 0.4.5

Replaces Beta 0.4.3 and keeps everything in it. The mod has a new home:
**https://github.com/Homflattovr/FarcryVVR_Release** (releases, source and issues are here from now on).

## Before you start

- **Play with the basic (default) outfit** until the mod works, and only then switch to another one. Other outfits
  can leave the hands missing or wrong.
- **Avoid SteamVR.** Use your headset's own OpenXR runtime instead (VDXR for Virtual Desktop, Meta Quest Link, or
  Pimax Play).

## New

- **Weapon recoil (off by default, F11 > Weapon > Recoil):** a kick on the drawn gun for every shot, with its own
  strength, two-handed and scope settings, a controller buzz, and "Shots follow the kick" so the bullets take the kicked
  barrel's line.
- **Gunstock calibration (F11 > Weapon):** long guns held in one hand get an angle correction (on by default), and the
  button *Learn angle and position of gunstock* sets it from 8 seconds of aiming a two-handed weapon. Two-handed guns
  can also be kept level with the left hand (on by default).
- **Built-in hand poses for every weapon:** grip points and left-hand places measured on the whole arsenal ship with
  the mod, so guns are held right the first time without learning.
- **Arms IK (experimental, off by default, F11 > Experimental > Arms IK):** the game's own arms, sleeves and gloves drawn
  from your shoulders to your hands, for every outfit. The *Hide arms* and *Hide body* boxes moved there.
- **Full magazine (on by default):** a magazine gun fires its whole magazine.
- **Menus with frame generation (on by default):** OFXR's frame generation is paused while a menu shows on the flat
  screen, so menus no longer blink black with NVIDIA DLSS frame generation, and the game is back in 3D quickly after.
- **Knocked down and QTEs:** the HUD stays on its panel while you are down or in a quick-time event.

## Fixes and changes

- **Weapons:** one weapon per shoulder slot and no wrong weapon model after a switch; the sling and holster copies are
  kept between game starts; the pump can be taken anywhere on the fore-end; the MP5 family's handle no longer steals a
  two-handed grip; the M79's grenade sits on the open tube; reload zones follow the gun as it is drawn; missing reload
  sounds on some repeaters, the 1888 and the classic RPG-7.
- **Vehicles:** your hands ride the game's own hands on the wheel, handlebars or stick, with each vehicle's wheel
  shipped with the mod; right A no longer fires in any vehicle; better culling when getting out (exit view follows the
  animation, on by default).
- **Rendering:** three sun shadow cascades in the evening and at night (on by default), matching shadows in both eyes,
  "skip what the lenses never show" back on by default, the GPS route on the road no longer taken for a HUD marker, and
  the F11 menu keeps its size at every render scale.
- **Controls:** the game never switches to keyboard mode (on by default), the weapon wheel picks with the stick, the
  D-pad modifier only at the left ear, game menu tabs on the triggers.
- **Stability:** a crash entering the Arcade map fixed.

Many of these were made from testers' reports and are not all checked in a headset yet: please report anything odd.
