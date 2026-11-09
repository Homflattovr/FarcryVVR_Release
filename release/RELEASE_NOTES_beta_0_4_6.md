# Far Cry 5 VR Beta 0.4.6

Replaces Beta 0.4.5 and keeps everything in it.

## Before you start

- **Play with the basic (default) outfit** until the mod works, and only then switch to another one. Other outfits
  can leave the hands missing or wrong.
- **Avoid SteamVR.** Use your headset's own OpenXR runtime instead (VDXR for Virtual Desktop, Meta Quest Link, or
  Pimax Play).

## New

- **Culling follows your headset (always on):** the game now decides what to draw from where your eyes are, not from
  its own camera, so no more missing ground or edges when you look down, roll your head, sit in a car, get out of one
  or use a mounted gun.
- **Every round fires (always on):** magazines empty completely (30 of 30 on the AR-C, the M60's whole belt) and the
  game never starts a reload of its own when you run dry; the wrist counter shows the real count. This replaces
  0.4.5's "Full magazine" switch, which could not stop the game's reload.
- **Recoil on by default** (F11 > Weapon > Recoil to turn it off or tune it).
- **Calibrate gunstock (F11 > Weapon), one section:** Off, Controllers only (a 5-second calibration), or Physical
  gunstock (each gun's sights put before your eye). Angle and X Y Z position for all long guns or per gun, with a reset
  button; a calibration hold may now move a little and still be accepted.
- **Reset to default** buttons for "Weapon on the controller".
- **Vehicles:** a quick small automatic turn of the wheel, handlebars or stick when you get in, so your hands find
  them at once. The built-in wheel table from 0.4.5 is removed (it could pick another vehicle's wheel); the hands follow
  the game's own hands instead.
- **Menus:** with "D-pad at the ear" on, the left grip pages game menus and zooms the map out wherever your hand is.

## Removed

- **The OFXR menu pause** from 0.4.5 is removed: it could fight other frame generation software.

## Known issues

- Small culling flashes can still happen.
- With frame generation, menus can blink black again (the OFXR menu pause is gone).

Many of these were made from testers' reports and are not all checked in a headset yet: please report anything odd.
