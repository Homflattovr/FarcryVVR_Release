# Changelog

## Beta 0.4.6 (9 November 2026): culling follows the headset, every round fires
Full notes: [release/RELEASE_NOTES_beta_0_4_6.md](release/RELEASE_NOTES_beta_0_4_6.md).
- **New:** culling from your eyes (no missing edges looking down, in or out of cars), every round of a magazine fires
  with no game reload at empty, recoil on by default, one Calibrate gunstock section (controllers or a physical gunstock),
  Reset to default buttons, a quick automatic wheel turn when you get in a vehicle.
- **Removed:** the OFXR menu pause (it could fight other frame generation software); menus can blink black again with
  frame generation.

## Beta 0.4.5 (8 November 2026): new home, recoil and two-handed aim
Full notes: [release/RELEASE_NOTES_beta_0_4_5.md](release/RELEASE_NOTES_beta_0_4_5.md).
- **New home:** https://github.com/Homflattovr/FarcryVVR_Release.
- **New:** weapon recoil (off by default), gunstock calibration for long guns, built-in hand poses for every weapon,
  Arms IK (experimental), full-magazine fire, no black blinking in menus with frame generation.
- **Also:** weapon slot, sling, pump, MP5 and M79 reload fixes, hands on vehicle wheels, sun shadows at night, the game
  kept in pad mode, a crash entering the Arcade map fixed.

## Beta 0.4.3 (7 November 2026): FSR in one click
Full notes: [release/RELEASE_NOTES_beta_0_4_3.md](release/RELEASE_NOTES_beta_0_4_3.md).
- **FSR for AMD, Intel and GTX cards:** the launcher's **Add FSR...** installs OptiScaler 0.9.4 from its own download
  (the .7z in your Downloads folder) with the settings the mod needs, and **Remove FSR** removes it. Then pick FSR 3.1
  or FSR through FFX (FSR 4 on Radeon RX 9000) as the Upscaler. Experimental, tested on an NVIDIA card only.
- **Also:** vehicle fixes (view on the car's front in hard turns, hands on the wheel, right grip readies the sidearm, A always
  accelerates, quad handlebars, softer helicopter stick, no cab ghosting with OFXR), menu button short press = map / hold =
  pause, triggers change menu tabs, no weapon switch when out of ammo, bow no longer upside down, weapon wheel and sling
  fixes, D-pad modifier only at the left ear, DLSS with OFXR through maps and loads, F11 > Debug > Sun shadows.

## Beta 0.4.2 (7 November 2026): button prompts
Full notes: [release/RELEASE_NOTES_beta_0_4_2.md](release/RELEASE_NOTES_beta_0_4_2.md).
- **Button prompts:** the game's on-screen prompts show your VR controller's buttons instead of an Xbox pad's or the
  keyboard's (F11 > HUD and menus > Button icons: Auto, Touch, Index or off).
- **F11:** the holster and the shoulder weapon slots are no longer marked experimental.

## Beta 0.4.1 (7 November 2026): hotfix
Full notes: [release/RELEASE_NOTES_beta_0_4_1.md](release/RELEASE_NOTES_beta_0_4_1.md).
- **Fix:** the game could crash within a second of starting (in `ntdll.dll`, no mod crash report). Beta 0.4's new
  default shadow and texture settings turned on a hook of Windows' file reading for every player, and the way it was
  written could make that part of Windows briefly non-executable. The hook is now written in one safe step.

## Beta 0.4 (6 November 2026): manual reloading and gesture-based bows
Full notes: [release/RELEASE_NOTES_beta_0_4.md](release/RELEASE_NOTES_beta_0_4.md).
- **Manual reload:** every kind of weapon reloads by hand from the left hip: magazines, charging handles and pistol
  slides, pump shotguns, lever and bolt actions, revolvers, double barrels, belt-fed guns with their cover, the
  flamethrower, M79, RPG-7 and RAT4. Quick right B drops or opens, a second B or a flick closes. The part to work next
  glows on the gun, the game's own reload sounds play, ammo shows on the wrist, and the reload data ships with the mod.
  Left Y still reloads the game's way.
- **Bow:** take the bow from the sling with the left grip, an arrow from the right shoulder, draw the string by hand
  and let go to shoot, through the bow's sight with a red dot in the aiming eye and a steadied draw.
- **Gestures:** throwables (the selected one's own model on the chest) and the holster are on by default, the new
  weapon is in your hand at once from a shoulder, two-handed guns stay in the left hand when the right lets go, and
  right B no longer switches weapon.
- **Vehicles:** cars, quads, bikes and boats on A (accelerate) and X (brake / reverse, double X handbrake), hands and
  the sidearm shown while driving, a grip puts a hand on the wheel, the right stick never moves a plane's view, planes
  face their nose even parked, the aim turned to a car's front on entry. Mounted guns and mortars aimed by hand.
  Gesture driving (experimental, off).
- **Picture and speed:** DLSS through D3D12 on its own thread (faster in Stereo), sun shadow detail and texture
  detail settings.
- **Weapons and sights:** red dot and laser in one choice (realistic, laser, arcade), a holographic ring in reflex
  sights, steadier sights with a still hand, fingers round what the hand holds.
- **Comfort:** cutscenes on the flat screen by default, takedowns stay in 3D; a recenter during the intro credits;
  each recenter brings the menus in front.
- **Fixes:** a freeze seen on Pimax (vibrations now sent from the render thread), a crash in 0.15, a wrong scope
  picture on another player's AR-C (long), the loading popup lasts 10 s at most.
- **F11 menu:** Gestures > Throwable and pouch, Bow and Manual reload, each with its main switches on top and the
  rest under Debug; "This menu in the headset" at the top of HUD and menus.
- **Defaults:** cutscenes flat (after 6 s), bullet speed x3, vibration on both hands at x2 (shot pulse 93 ms),
  weapon smoothing 0.26, DLSS sharpening 0.20, texture detail Sharper, sun shadows 2048, tracers hidden, throw arc on.

## alpha 0.15 (30 October 2026): the gesture update
Full notes: [release/RELEASE_NOTES_alpha_0_15.md](release/RELEASE_NOTES_alpha_0_15.md).
- **Gestures:** weapon slots on both shoulders (slots 1 and 2 by default), the sling refilled after a load, a
  holster on the hip and a cloth pouch for throwables (both experimental), faster weapon switching.
- **Vehicles and falls:** the right stick steers helicopters and planes and never moves the view in a vehicle,
  helicopter nose pitch, left hand drives and right hand shoots, planes face the nose on entry, boats accelerate,
  parachute and wingsuit views fixed, no bounce after a knockdown, 3D cutscenes follow cuts.
- **View and HUD:** body-based view, smooth walking and running, small detection chevrons placed in the world,
  big world markers limited, a "VR mod loading" popup after each load (the launcher can skip it), map and
  binoculars, B closes tutorial pop-ups, a tidier F11 menu.
- **Picture:** OFXR Bridge's DLSS frame generation works, AFW foliage steady, lamps placed per eye, a foliage test
  option for DLSS.
- **Fixes:** no more lasting FPS drop from the 5-minute weapon scan, shots no longer held back, no automatic aim down
  sights on iron sights, the weapon angle on the controller is saved.
- **Defaults:** weapon pitch on the controller -5 degrees, even frame pacing off.
- **Recommended:** OFXR Bridge and a high-bitrate codec (H.264+ 300-400 Mbps or AV1 10-bit about 200 Mbps).

## alpha 0.12.1 (25 October 2026): hotfix
- **Stereo:** no more left-eye tremor for players whose VR Framework setting had VR_AsyncAER on (it is now always
  off).
- **AFW:** two image rebuild fixes for cleaner edges in AFW are now on by default.
- **Crash fix:** the game no longer crashes when the PC has an empty D: drive (a card reader or a disc drive with
  nothing in it).

## alpha 0.12 (24 October 2026)
- **OFXR:** the mod now works with OFXR Bridge running (3D and VR instead of a flat image), confirmed in the headset.
  It also avoids a stall while loading.
- **AFW:** the gun no longer doubles up between the eyes when you move, the car interior stays steady while
  driving, and the view no longer judders going up or down hills.
- **Scopes:** guns without a scope (such as the M60 with no sights) are no longer treated as scoped.
- **Vibration:** every shot gives a firm, even pulse in the gun hand. Its strength and length can be set in
  F11 > Weapon > Feel.
- **Flamethrower:** flames, muzzle smoke and sparks stay where they were fired instead of swinging with your hand.
  Fired flames stay in the world for 1.5 s after you let go (F11 > Weapon > Bullet tracers).
- **Flamethrower pilot flame:** stays on the nozzle when not firing (new, not yet checked in the headset).

## alpha 0.11 (24 October 2026)
- **Scopes:** with two hands on the gun, the right stick zooms in steps (x1, x4, x8, x16, x32). Shadows outside the
  scope stay sharp, zooming out no longer turns the scope black, and sniper zoom levels are read correctly.
- **Red dots:** closed red dot sights no longer show a dark glass disc. Scope and red dot settings moved to F11 > Weapon.
- **Guns:** both eyes show the gun in the same place, two-handed guns line up with both hands, shots leave at once
  while the gun moves, and each gun's grip is remembered.
- **Hands:** empty hands stay on the controllers while walking, running or sprinting. The left grip no longer grabs
  the Uzi.
- **Gestures:** gesture zones can be shown and moved, each with a Default button. The shoulder zone (switch weapon)
  now sits behind the ear and turns with the body.
- **Vibration:** goes to the hand holding the gun (both hands with a two-handed hold), with a strength slider.
- **Movement:** walking follows the headset and stays straight while the gun turns. No walking bob. Getting in and
  out of vehicles is smoother, and the view recenters after loading.
- **Picture:** no black squares in far shadows, fewer stutters, and button prompts stay on the HUD panel. AFW is
  marked experimental (Stereo recommended).
- **Launcher:** checks the game options the mod needs and offers to set them. A crash logger offers a report zip.
- **F11 menu:** cleaned up.
- **Package:** the zip holds one `bin` folder laid out like the game's and contains no other archive.

## pre_alpha_0_1 (20 October 2026)
- First public release.
