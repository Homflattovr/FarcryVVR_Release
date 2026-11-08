# Far Cry 5 VR Beta 0.4: manual reloading and gesture-based bows

Reload with your own hands. Drop the magazine, take the next one from your hip and push it in, rack the handle,
work the pump, the lever or the bolt, fill a revolver round by round, feed a belt into an M60. Take the bow off the
sling, pull an arrow from your shoulder and draw the string yourself. This update also brings new vehicle controls
with your hands and sidearm in the car, faster DLSS, and throwables and the holster on by default.

**Strongly recommended:** use [OFXR Bridge](https://github.com/tig3rmast3r/OFXR-Bridge/releases) and a high-bitrate
codec. In Virtual Desktop: H.264+ at 300-400 Mbps if your Wi-Fi holds it, or AV1 10-bit at about 200 Mbps, with
automatic bitrate off. Foliage is what suffers most from a low frame rate and a compressed stream.

**Updating:** replace every file, including `fc5vr_settings.txt` (it holds this version's recommended settings and
the manual reload data for the weapons). An old settings file keeps its old values.

## New: manual reloading

Every kind of weapon reloads by hand. Spare magazines, shells, rounds, belts and rockets sit at your **left hip**:
take one with the left grip.

- **Magazine guns** (rifles, SMGs, pistols): a quick press of right B drops the magazine. Take the next from your hip,
  bring it under the gun where its ghost shows and push it in until it clicks. After firing every round, rack the
  charging handle with the left grip, or pull back a pistol's slide (held open when empty). Racking with a round in
  costs a round.
- **Pump shotguns:** push shells one at a time into the loading port under the gun, and work the pump with the left
  hand after every shot.
- **Lever actions** (1887, 1888, repeaters): shells into the port, and with the left hand on the fore-end, drop the
  right hand about 5 cm and bring it back to work the lever.
- **Bolt actions** (.308, HTI .50): after every shot, grip the bolt, turn it up, pull it back (the case flies out),
  push it forward and turn it down. The bolt follows your hand at every step.
- **Revolvers:** quick B swings the cylinder out and the fired cases drop out; push a round into each empty chamber.
- **Double barrels** (Sledge, sawed-off): quick B breaks the gun open; load a shell into each barrel.
- **Belt-fed guns** (M60, M249): quick B drops the belt and lifts the feed cover; push a new belt into the tray, push
  the cover down by hand and rack the handle.
- **Launchers:** the M79 breaks open and takes a grenade into the breech, the RPG-7 takes a rocket at the front of
  the tube, the RAT4 opens at the back and takes a missile there. The flamethrower takes a new canister.
- **Closing:** a second quick B, or a flick of the gun (sideways for a revolver). The RAT4 closes with B or the left
  grip at the back.
- **Hints:** the part to work next glows on the gun's own model, only when the gun needs you (run dry, a reload
  begun, or a pump, bolt or lever to work).
- **Sounds:** each step plays the game's own reload sound for that weapon.
- **Ammo on the wrist:** rounds in the gun, what it holds and rounds left, under the weapon hand's wrist
  (F11 > HUD and menus).
- **Other guns** reload by hand as magazine guns, their magazine learnt from the game's own reload the first time.
- **The reload data ships with the mod**, so the weapons work from the first reload. If a part looks wrong,
  F11 > Gestures > Manual reload > Debug > The gun in hand can learn it again from the game's reload.
- **Left Y still reloads the game's way** ("Keep the automatic reload button", on by default), and a gun is never
  left unable to fire while the game still has rounds for it.
- Turn it on or off in F11 > Gestures > Manual reload.

## New: the bow

- **Draw it by hand:** the bow is in your left hand and your right shoulder is a quiver. Right grip at the right
  shoulder takes an arrow; bring it to the bow just above your left hand, pull the string back (a short buzz at full
  draw) and let go of the grip to shoot. Let go before half draw and no arrow flies.
- **On the sling:** the bow hangs there like your other weapons. Take it with the left grip anywhere along it. The
  right shoulder gives an arrow only while the left grip holds the bow; otherwise it is your weapon slot as before.
- **Aiming:** drawn, the arrow goes from your eye through the bow's sight pin, with a red dot where it will land, in
  your aiming eye only and only with an arrow nocked. The drawn bow and the arrow are held steady against small
  shakes, while a real aim move still moves them.
- Bows are found from the game's own data; each one stands upright in your hand and keeps its fit between sessions.
- Settings in F11 > Gestures > Bow.

## New: vehicles

- **Cars, quads, bikes and boats on A and X:** right A accelerates, left X brakes and reverses while held, and a quick
  double press of X (held) is the handbrake. The triggers, grips and right stick no longer drive.
- **Your hands while driving:** shown and following the controllers, the right one holding your sidearm, ready to fire
  with the right trigger. Press a grip near the wheel, handlebars or stick to put that hand on it.
- **Planes:** the right stick no longer moves the view; your head looks around. You face the nose when you get in,
  also in a plane parked at rest.
- **Cars and quads:** the game's aim is turned to the vehicle's front when you get in.
- **Mounted guns and mortars** stay on their mount: aim with the right controller (inverted, like the handles of a gun
  on a pivot) or your head (F11 > Weapon > Mounted weapons).
- **Gesture driving (experimental, off by default):** hold the wheel, handlebars or stick with a grip and turn it
  (F11 > Experimental).
- The vehicle switches are in F11 > Layout > Vehicles.

## Gestures and weapons

- **Throwables on by default:** the throwable the game has selected sits on your chest with its own model. Grab it
  with the right grip, aim with the arc, let go to throw. Its place and size are in F11 > Gestures > Throwable and
  pouch. Taking one with the weapon on the sling no longer plays the game's put-away animation.
- **Holster on by default:** your sidearm on the right hip.
- **Shoulder slots:** the new weapon is in your hand at once, with no switch animation.
- **Right B no longer switches weapon** (switch at your shoulders or with the wheel); held, it still heals.
- **Two-handed hold:** let go with the right hand and the gun stays in your left hand.
- **A left grip near the weapon in your hand** is never taken for the modifier (D-pad).
- **Red dot and laser** in one choice: realistic, laser (default) or arcade. Reflex sights show a holographic ring with
  ticks. "Automatic aim down sights near the eye" is now called "Learn gun sight line" (F11 > Weapon > Sights).
- **Steadier sights** while your hand is still.
- **Fingers** close round what the hand holds: a magazine, a shell, the charging handle, the bow string.
- **No DLSS trail** behind the weapon on the sling.

## Picture and speed

- **DLSS through D3D12:** DLSS now runs on its own D3D12 queue, recorded on a worker thread, so the game's render thread
  no longer waits for it. In our tests on an RTX 4090 (Meta XR Simulator) the frame rate went from about 29 to 39-45
  eye pairs per second, with the same picture. On by default.
- **OFXR Bridge** runs well with the D3D12 DLSS path on (tested in the headset, 34-35 eye pairs per second in Stereo
  on an RTX 4090).
- **Sun shadow detail** (F11 > Display): finer sun shadows, 2048 by default (up to 4096), from the next start.
- **Texture detail** (F11 > Display): sharper textures under DLSS, Sharper by default.

## Comfort and menus

- **Cutscenes on the flat screen** in the room by default, starting after 6 s so takedowns stay in 3D. The launcher
  has a new Cutscenes choice.
- **Recenter at start:** a recenter a few seconds after the game starts, during the intro credits, and every recenter
  also brings the menus and the F11 menu in front of you.
- **"VR mod loading" popup:** 10 s at most.
- **F11 menu:** Gestures now holds Throwable and pouch, Bow and Manual reload, each with its main switches on top and
  the rest under its own Debug section. "This menu in the headset" is at the top of HUD and menus. The two texture
  settings are one.

## Important fixes

- **Freeze on Pimax:** a tester's game froze inside the headset runtime; the vibrations are now sent from the render
  thread.
- **A crash seen in 0.15** (a shared list read from two threads) is fixed.
- **AR-C (long) scope:** 0.15 shipped one player's scope picture for that gun, which looked wrong on other optics.
  Each player's own optic is now read.

## New defaults

- **Cutscenes: flat screen**, starting after 6 s.
- **Bullet speed: x3.**
- **Vibration: both hands, strength x2, shot pulse 93 ms.**
- **Weapon smoothing 0.26, DLSS sharpening 0.20.**
- **Texture detail: Sharper. Sun shadow detail: 2048.**
- **Bullet tracers: hidden. Throw arc: on.**
- **Throwables, holster, manual reload and the manual bow: on.**

See [KNOWN_ISSUES.txt](https://github.com/Homflattovr/FarcryVVR_Release/blob/main/release/KNOWN_ISSUES.txt) before
reporting. When something looks wrong, send what you did, Stereo or AFW, a short video if you can, and
`fc5vr_log.txt` from the game's `bin` folder (or the launcher's report zip).
