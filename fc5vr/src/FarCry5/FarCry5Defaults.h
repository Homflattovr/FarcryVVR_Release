#pragma once

// The mod's defaults: the author's own settings (bin\fc5vr_settings.txt as he left it on 20 Oct 2026 18:41), shipped in
// release pre_alpha_0_1 (author 20 Oct). Replayed at start before
// fc5vr_settings.txt, which overrides them line by line; written as fc5vr_settings.txt when there is none, so a fresh
// file is exactly this text. tools\release\fc5_package.ps1 checks the shipped file against it.
// Release 0.12.1 (author 25 Oct 12:40): throwable off.
// 31 Oct (tester's Pimax log: a freeze inside the headset runtime, the M249's sight): hapticthread on; weapon 62229's scopecal
// (x1.3, the author's rifle with its scopezoom) and scopeglassat removed, read from each player's own gun (scope_settings_migrate).
// 29 Oct (author 13:49): bodyview on, one switch for
// viewpivot, viewroot, viewrootsmooth, rootsteady and walkbob (no longer saved on their own); steadyview retired (off since 12 Oct).
// 28 Oct (author 16:44): the sight lines (sightcal) and
// scope zooms (scopecal) learned for 17 more weapons in the simulator on the "All Weapons YT" Arcade map
// (tools\arcade\allweapons.ps1, testruns\2026-10-28_arcade). They are read from the game's own aim-down-sights pose of the
// map's version of each gun (its attachments): a gun whose optic differs from the map's needs its own line (see STATUS).
// 29 Oct (author 13:48, guns turned and an aim down sights near the eye): gunaxes, adsprobe optic, scopesticky, weaponidsplit.
// 30 Oct (author 13:46): framelock off. Author 13:58 set in his
// 13:25 play (not saved then): grippitch -3.0; 15:09 (release 0.15): -5.0. 17:14: reddotholo on (holographic reticle in reflex sights).
// 2 Nov (lighting review, author 2 Nov 09:52): prevmvonce removed (always on; the command now only logs).
// 2 Nov (author 12:54): his bin\fc5vr_settings.txt of 2 Nov 12:49:
// throwable on with his throw lines (pouch, right-hand throw, throwpos 0.07 0.30 -0.04), sidearm on (the hip and its holster; his
// sidearmpos 0.20 0.72 -0.02 and sidearmsize 0.13 are old defaults the mod reads as today's 0.26 0.63 -0.12 and 0.16, so not copied;
// sidearmweapons not copied: ignored with the wheel's sidearm slot). startrecenter 3.0 (new): a recentre 3 s after the first play.
// 2 Nov 15:10 (author): startrecenter counts from the launch recentre (during the
// credits); recenterscreens on (new): each recentre also brings the credits, menus and F11 in front.
// 5 Nov (author): aimdot on, reddot on, reddotonly on are now the one line reddotmode laser.
// 6 Nov (author 14:07): his bin\fc5vr_settings.txt of 6 Nov
// 14:04. Changed: bulletspeed 3.00, cinematics flat, dlsssharp 0.20, mipbias -0.5, throwarc on, throwpos 0.10 0.53 -0.10, tracersfromgun off,
// weaponsmooth 0.26. Added: bowhold, bowmindraw, bowstringhand, bowweapons 51543, cinedelay 6.0, fingergame, gamedrawgrab, manualbow on,
// reloadsoundcat, reloadsoundlearn, reloadsoundpick, rumble both, rumblegain 2.00, shadowres 2048, shotbuzzms 93, shoulderdraw, throwfile,
// throwhandscale 0.70, throwlibrary. Not copied (research\2026-11-06_releasedefaults\README.md): gesture driving (drive, driveshow), debug and
// test switches (menulevel 2, gputime, probeoff, soundwatch, gamestate, meleeswing, perf*, sightdata), experimental melee (release 0.12.1 rule),
// adsprobe off, the per-gun lines learnt on his own guns (scopecal, scopeglassat, scopezoom, sightcal, reloadgeom, reloadhand*, reloadmagmesh,
// reloadsoundlearnt, bowarrowmesh), reloadproblem, reloadweapons, sidearmpos/size/weapons; lines equal to the code's default left out.
// 6 Nov 14:31 (author): his manual
// reload data and learnt reload sounds too (reloadgeom, reloadhandgrabw, reloadhandmagw, reloadhandlepart, reloadmagmesh, reloadsoundlearnt)
// and bowarrowmesh with 25110; scopecal, scopeglassat, scopezoom and sightcal stay per player (they follow the optic fitted).
// 7 Nov 19:47 (author): lowsuncsm on (three sun cascades in the evening and at night).
// 13 Nov 14:22 (author): cinematics 3d (was flat); his cinesteerstart 15 is the code default now.
// 13 Nov (author 14:22): cinedelay removed (cutscenes are the game's own sequences).
// 13 Nov (performance pass, author): bbcopyskip, eyeprobeoff and savelater lines
// removed (retired commands: the mod always does what they switched on). perfdrawflags is on in the code, so it needs no line here.
// 13 Nov (F11 reorganisation, author 19:24Z): hudmarkcap, hudmarkers, hudprim, hudprimcam, hudprimworld,
// shoulderdraw, throwfile and throwlibrary lines removed: no longer settings, always on in the code (a saved line is ignored).
// firegate 0.10 removed the same way (author 19:31Z): it is the code default now, with firewaitmax 250, firefast and firemoving on.
// gamedrawgrab on, throwv2 on and throwv2rb grab removed (holster, bow and throwables clean-up, 19:27Z): the code defaults, no longer settings.
// fingergame on (Hands, body and arms, 19:44Z), dlaaphases 16 and ssrmv off (Experimental gone, 19:45Z) removed the same way.
// hands on removed the same way (author 20:25Z: the hands are always drawn).
// 16 Nov 2026 (hotfix 1.0.1, author 22:19Z): his
// bin\fc5vr_settings.txt of 16 Nov 21:30, every line except his calibration and learnt data (throwpos, gestureshoulder, sidearm and sling
// places and sizes, bodyikbody, seated, gunstockpose/gunstockguns, stockeye, onehandmatch, reloadgeom, reloadhandmagw, reloadsoundlearnt,
// reloadproblem, reloadlearntuse, reloadweapons, sidearmweapons, scopecal, scopeglassat, scopezoom, sightcal, firstguntip, bowhold), the
// log-only switches (gamestate, ctrlrecauto, probeoff, soundwatch) and meleeearly (no effect since melee fix 12). Lines equal to the code's
// default are written too this time, so the shipped file is his.
// 16 Nov 22:24Z (author): his learnt per-weapon data merged in by weapon id (his
// entry wins, the release's other entries kept): reloadgeom, reloadhandmagw, reloadsoundlearnt, reloadproblem, scopecal, scopeglassat,
// scopezoom, sightcal; his reloadlearntuse, reloadweapons and sidearmweapons; his throwpos, gestureshoulder, sidearmsize, slingsize and
// slingzone. Still not copied: bodyikbody (T-pose), seated, gunstockpose, gunstockguns, stockeye, onehandmatch, firstguntip, the log-only
// switches, and sidearmpos (his is the old default the mod replaces with today's).
// 17 Nov (hotfix 1.0.2, author 07:48Z): no scopecal,
// scopeglassat, scopezoom or sightcal lines shipped any more; each gun learns its fitted optic, and sightoptic (FarCry5Sight.cpp) keeps
// the learnt data with the optic it was learnt on.
namespace fc5 {
inline constexpr const char* kReleaseSettings = R"FC5VR(
# Far Cry 5 VR settings: the same commands as fc5vr_cmd.txt, replayed at start (edited by the mod).
adsprobe optic
afwgunown on
airfollow on
airroll 1.00
aniso 16
aoexact on
asmfloor on
autorecenter on
autorecenterdist 0.50
autorecenterwait 0.5
bodyik on
bodyview on
bowarrowmesh 25110:3693 51543:3693
bowflip off
bowhold 0 -11 0
bowmindraw 0.49
bowpin aim
bowstringhand 0.00 0.00 -0.04
bowweapons 51543
bulletspeed 3.00
chutelevel on
chutelook on
cinematics 3d
cinesteer on
cinesteerstart 41
cinesteertime 1.5
contactleaf 1.00
contactshadows on
cullnarrow off
decaleye on
dlss on
dlsscammv 1
dlssfovea off
dlssmenurelease off
dlssmvbest on
dlssmvcheck on
dlssmvsway on
dlsspreset 11
dlsssharp 0.20
dlsstrim 7
drive on
drivereact on
drivereactgain 2.00
driveshow off
f11image off
fastlights on
fingerswap off
firemeasure on
firesplit on
fovexact on
fovmargin 0
fovnarrow 1.00
framedouble on
framelock off
gesturebuzz on
gestureshoulder 0.18 0.25 0.13
gputime on
grippitch -5.0
groundfollow off
gunanchor on
gunaxes on
gunstill on
gunstilllate on
gunstillrecoil off
hapticthread on
hidetracers on
hidmask 50
hidmaskafw off
hipspread 0.00
huddist 2.31
hudfullres on
hudheight 0.10
hudmarkdist 30
hudmarkicon 2.0
hudmarkmax 4.0
hudmarksize 0.21
hudmarksmooth 0.40
hudmask on
hudmode 1
hudmul on
hudpanel image
hudscale 2.00
hudscreen on
hudsize 40
lakeoverscan 1.00
lakepixel on
lakeshared on
lodfov off
logbuffered on
lowsuncsm on
manualbow on
meleebutton r3
meleehit 2
meleeray on
meleeswing off
meleeweapons auto
menufade 0.48
menufastback on
menulevel 2
menuproj on
menuscreen on
mipbias -0.5
mipbiasauto on
mountedaim head
nearfine on
norecoil on
nosway on
offhandstill on
ofxrdlss11 on
pairjumps on
perfdlss12 on
perfdlss12thread on
perfdrawflags on
postclean on
queryvisible on
recenterscreens on
recoil on
recoilscale 0.66
recoiltwohand 0.55
reddotholo on
reddotmode realistic
reddotsize 9.0
reflections on
reloadgamehold on
reloadgeom 18912:-0.076,-0.048,-0.005,0.001,-0.125,-0.992,0.000,0.090,0.050,1,0 21948:-0.044,-0.034,0.030,-0.000,0.778,-0.628,-0.044,-0.004,0.100,1,0 22182:-0.044,-0.026,0.059,-0.003,0.961,-0.277,-0.045,-0.006,0.087,1,0 23397:-0.034,0.015,0.253,0.000,0.996,-0.087,-0.023,0.026,0.160,1,0 23475:0.022,-0.009,-0.008,0.020,0.096,0.995,0.000,0.090,0.050,1,0 23958:-0.045,-0.034,0.030,-0.000,0.778,-0.628,-0.044,-0.004,0.100,1,0 24417:-0.045,-0.026,0.041,0.001,0.935,-0.356,-0.045,-0.007,0.083,1,0 24474:-0.044,-0.059,-0.099,0.000,0.996,-0.087,-0.035,-0.054,-0.045,1,0 26043:-0.045,-0.026,0.041,0.000,0.935,-0.354,-0.045,-0.007,0.083,1,0 26187:-0.038,-0.021,0.045,0.021,0.978,-0.208,-0.039,-0.008,0.083,1,0 27144:-0.050,-0.005,-0.105,0.000,0.996,-0.087,-0.050,-0.053,-0.021,1,0 28596:-0.049,-0.020,0.049,-0.004,0.976,-0.217,-0.049,-0.011,0.086,1,0 29136:-0.045,-0.025,0.049,-0.000,0.935,-0.354,-0.046,-0.010,0.089,1,0 29259:-0.044,-0.073,-0.183,-0.000,0.996,-0.087,-0.043,-0.089,-0.296,1,0 29841:-0.040,-0.026,-0.153,0.000,0.999,-0.052,-0.039,-0.038,-0.269,1,0 29850:-0.043,-0.053,-0.141,0.000,-0.735,-0.678,0.000,0.090,0.050,1,0 32325:-0.046,-0.028,-0.042,-0.000,0.998,-0.065,-0.053,0.012,0.021,1,0 32472:-0.040,-0.020,-0.112,-0.000,0.996,-0.087,-0.040,-0.067,-0.027,1,0 32688:-0.052,-0.060,-0.115,0.000,0.996,-0.087,-0.074,-0.045,-0.154,1,0 35187:-0.055,-0.030,0.074,-0.000,0.996,-0.087,-0.058,0.000,0.064,1,0 35490:-0.044,-0.061,-0.133,-0.000,-0.714,-0.700,0.000,0.090,0.050,1,0 35499:-0.032,-0.048,-0.148,-0.000,0.991,-0.132,-0.031,-0.073,-0.260,1,0 35709:-0.040,-0.019,-0.068,-0.000,0.996,-0.087,-0.052,0.048,-0.195,1,0 37359:-0.039,-0.019,-0.068,-0.000,0.996,-0.087,-0.052,0.049,-0.195,1,0 39012:-0.046,-0.042,-0.106,-0.000,0.996,-0.087,-0.042,-0.096,0.022,1,0 39408:-0.049,-0.012,-0.225,-0.000,0.996,-0.087,-0.039,0.007,0.040,1,0 39750:-0.046,-0.087,-0.111,0.000,0.991,-0.132,-0.073,-0.058,-0.190,1,0 40362:-0.054,-0.016,-0.220,0.000,0.996,-0.087,-0.074,0.001,-0.077,1,0 40752:-0.040,-0.004,-0.066,-0.000,0.996,-0.087,-0.048,-0.019,0.071,1,0 42000:-0.033,0.009,0.222,-0.000,0.996,-0.087,-0.003,0.006,0.149,1,0 42549:-0.054,0.004,-0.148,-0.000,0.996,-0.087,-0.059,-0.065,-0.349,1,0 42555:-0.040,-0.028,-0.133,-0.000,0.996,-0.087,-0.056,-0.018,-0.181,1,0 43215:0.000,0.030,-0.080,0.000,1.000,0.000,-0.010,-0.040,-0.167,0,0 45582:-0.045,-0.008,-0.096,0.000,0.996,-0.087,0.002,0.021,-0.023,1,0 47430:-0.038,-0.037,-0.158,0.000,0.996,-0.087,0.000,0.090,0.050,1,0 57819:-0.042,-0.004,-0.065,-0.000,0.996,-0.087,-0.046,0.047,0.069,1,0 58416:-0.050,0.039,-0.062,0.000,0.996,-0.087,0.024,-0.004,-0.200,1,0 60537:-0.040,-0.016,-0.087,0.000,0.996,-0.087,0.005,0.003,-0.155,1,0 61926:-0.064,0.031,0.059,0.000,0.996,-0.087,-0.023,-0.028,-0.006,1,0 62229:-0.037,0.001,-0.057,0.000,0.996,-0.087,-0.036,0.050,0.078,1,0 63120:-0.066,0.037,0.048,0.000,0.996,-0.087,-0.027,-0.010,-0.011,1,0 69646:-0.001,0.035,-0.242,0.000,-0.087,-0.996,0.000,0.090,0.050,1,0 72736:-0.080,0.010,-0.068,-0.000,0.996,-0.087,0.006,-0.022,-0.003,1,0 80077:-0.001,0.035,-0.242,-0.000,-0.087,-0.996,0.000,0.090,0.050,1,0 31470:-0.044,-0.055,-0.136,0.000,-0.682,-0.731,0.000,0.090,0.050,1,0 41649:-0.046,-0.022,-0.043,0.000,0.996,-0.087,-0.046,-0.008,-0.103,1,0
reloadhandgrabw 29259:-0.036,-0.110,-0.308 35499:-0.031,-0.083,-0.249
reloadhandlepart 57819:57819/6
reloadhandmagw 21948:0.021,0.078,-0.100 22182:0.072,0.066,-0.036 24417:0.011,0.096,-0.050 26043:0.011,0.096,-0.050 28596:0.041,0.085,-0.030 29136:0.045,0.064,-0.044 35490:-0.016,0.021,0.074 35709:-0.048,0.110,-0.040 37359:-0.046,0.108,-0.043 57819:0.007,0.140,0.015 58416:0.034,0.151,-0.026 62229:0.009,0.153,0.019
reloadlearntuse none
reloadmagmesh 57819:4110/19+31,5106/p
reloadmanual on
reloadpartstable off
reloadproblem 24417:20 26043:40 43944:1
reloadsoundcat on
reloadsoundlearn on
reloadsoundlearnt 18912:712069802,2148877110,-,- 21948:813072728,3079813257,-,- 22182:2415060737,1640757865,-,- 23475:3118769098,-,-,- 24417:1302339480,1980945621,-,- 26043:1302339480,1980945621,-,- 26187:2148877110,2148877110,-,- 27144:2603587974,-,-,- 28596:506640306,2148877110,-,- 29136:2408732068,2330490764,-,- 29850:1609459330,-,-,- 32472:2603587974,-,-,- 35490:1095402538,1025973305,-,- 35499:-,-,3403294259,784439553 35709:1600661443,1598804907,-,- 37359:1600661443,1598804907,-,- 43215:1389234585,2830281633,-,- 45582:2519555196,-,-,- 57819:36064529,1879999235,-,- 60537:2519555196,-,-,- 62229:36064529,1879999235,-,- 80077:440947876,-,-,- 29259:-,-,3403294259,137291831
reloadsoundpick 2:3240498188,3863307842,4246560353,4119235673
reloadsoundseq on
reloadweapons v2 18912 20586 22182 24417 26043 26187 28596 29136 29259 29841 32325 34497 35499 40362 42549 57819 62229
reloadweaponsounds on
render stereo
resscale 1.0
roomscale on
rumble both
rumblegain 1.90
scope on
scopeevery 1
scopeeye auto
scopehistswap on
scopemodel on
scopepass on
scoperecoiloff off
scoperes 1024
scopeskip 15
scopesrc 1
scopesticky on
shadowkernel on
shadowmv on
shadowmvnew 0.05
shadowres 3072
shotbuzzms 93
shoulderswitch on
shoulderswitchsize 0.20
sidearm on
sidearmrot 0 -80 0
sidearmsize 0.30
sidearmweapons +24417 +62229
sightaim on
sightdata on
skipps FlareDiffuseSampler
slingpos 0.09 0.50 0.05
slingrot 70 -30 0
slingsize 0.18
slingzone 0.00 0.00 0.00
smoothmotion on
snapresetfix on
snapsync on
ssrjitterpair on
ssrretry on
ssrrough off
ssrtileclear on
startrecenter 3.0
steadyexact on
sunranges 3/8/30
sunshare off
sunsize 0.50
taamv on
taaskip on
throwable on
throwablesize 0.22
throwarc on
throwchest off
throwhandscale 0.70
throwpeek off
throwpos 0.10 0.32 -0.10
throwpouch on
throwv2hand right
tracersfromgun on
turnhead on
upright on
vehicleperpair on
vehicles on
vehplayerfov on
vehrecenter on
vehvignette 0.00
weaponcal 0 2 29841
weaponcollide on
weaponidsplit on
weaponrest glide
weaponsmooth 0.31
weaponweight 0
weaponweightinertia 42
weaponweightsighted 0.70
wheelstick on
)FC5VR";
}
