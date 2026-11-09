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
namespace fc5 {
inline constexpr const char* kReleaseSettings = R"FC5VR(
# Far Cry 5 VR settings: the same commands as fc5vr_cmd.txt, replayed at start (edited by the mod).
adsprobe optic
airfollow on
airroll 1.00
aniso 16
aoexact on
autorecenter on
autorecenterdist 0.50
autorecenterwait 0.5
bbcopyskip on
bodyview on
bowarrowmesh 25110:3693 51543:3693
bowhold 0 -11 0
bowmindraw 0.49
bowstringhand 0.00 0.00 -0.04
bowweapons 51543
bulletspeed 3.00
cinedelay 6.0
cinematics flat
decaleye on
dlaaphases 16
dlss on
dlsscammv 1
dlssmvbest on
dlssmvcheck on
dlsspreset 11
dlsssharp 0.20
dlsstrim 7
eyeprobeoff on
f11image off
fingergame on
firegate 0.10
firemeasure on
firesplit on
fovexact on
fovmargin 0
fovnarrow 1.00
framedouble on
framelock off
gamedrawgrab on
gesturebuzz on
gputime on
grippitch -5.0
gunanchor on
gunaxes on
gunstill on
gunstillrecoil off
hands on
hapticthread on
hidetracers on
hidmask on
hipspread 0.00
huddist 2.27
hudfullres on
hudheight -0.16
hudmarkcap on
hudmarkdist 8
hudmarkers on
hudmarkicon 2.0
hudmarkmax 4.0
hudmarksize 0.21
hudmarksmooth 0.3
hudmask on
hudmode 1
hudmul on
hudprim on
hudprimcam on
hudprimworld on
hudscreen on
hudsize 40
lakeoverscan 1.00
lakeshared on
logbuffered on
lowsuncsm on
manualbow on
menulevel 1
mipbias -0.5
mipbiasauto on
norecoil on
nosway on
pairjumps on
postclean on
queryvisible on
recenterscreens on
reddotholo on
reddotmode laser
reddotsize 9.0
reflections on
reloadgeom 18912:-0.076,-0.048,-0.007,0.001,-0.080,-0.997,0.000,0.090,0.050,1,0 21948:-0.044,-0.034,0.030,-0.000,0.778,-0.628,-0.044,-0.004,0.100,1,0 22182:-0.045,-0.026,0.059,-0.003,0.961,-0.277,-0.045,-0.006,0.087,1,0 23397:-0.034,0.015,0.253,0.000,0.996,-0.087,-0.023,0.026,0.160,1,0 23475:0.022,-0.009,-0.007,0.020,0.096,0.995,0.000,0.090,0.050,1,0 23958:-0.044,-0.034,0.030,-0.000,0.778,-0.628,-0.044,-0.004,0.100,1,0 24417:-0.044,-0.026,0.041,0.001,0.935,-0.356,-0.045,-0.007,0.083,1,0 24474:-0.044,-0.059,-0.099,0.000,0.996,-0.087,-0.035,-0.054,-0.045,1,0 26043:-0.045,-0.026,0.041,0.000,0.935,-0.354,-0.045,-0.007,0.083,1,0 26187:-0.038,-0.021,0.045,0.021,0.978,-0.208,-0.039,-0.008,0.083,1,0 27144:-0.050,-0.005,-0.105,0.000,0.996,-0.087,-0.050,-0.053,-0.021,1,0 28596:-0.049,-0.020,0.049,-0.004,0.976,-0.217,-0.049,-0.011,0.086,1,0 29136:-0.045,-0.025,0.049,-0.000,0.935,-0.354,-0.046,-0.010,0.089,1,0 29259:-0.044,-0.074,-0.183,-0.000,0.996,-0.087,-0.044,-0.093,-0.297,1,0 29841:-0.040,-0.027,-0.153,0.000,0.999,-0.052,-0.039,-0.038,-0.269,1,0 29850:-0.044,-0.047,-0.137,0.000,-0.704,-0.710,0.000,0.090,0.050,1,0 32325:-0.058,-0.023,-0.018,0.043,0.996,-0.077,-0.059,0.019,0.044,1,0 32472:-0.040,-0.020,-0.112,-0.000,0.996,-0.087,-0.040,-0.067,-0.027,1,0 32688:-0.052,-0.060,-0.115,0.000,0.996,-0.087,-0.074,-0.045,-0.154,1,0 35187:-0.055,-0.030,0.074,-0.000,0.996,-0.087,-0.058,0.000,0.064,1,0 35490:-0.044,-0.055,-0.136,0.000,-0.682,-0.731,0.000,0.090,0.050,1,0 35499:-0.032,-0.041,-0.150,0.000,0.996,-0.087,-0.031,-0.061,-0.264,1,0 35709:-0.040,-0.019,-0.068,-0.000,0.996,-0.087,-0.052,0.048,-0.195,1,0 37359:-0.039,-0.019,-0.068,0.000,0.996,-0.087,-0.052,0.049,-0.195,1,0 39012:-0.046,-0.042,-0.106,-0.000,0.996,-0.087,-0.042,-0.096,0.022,1,0 39408:-0.049,-0.012,-0.225,-0.000,0.996,-0.087,-0.039,0.007,0.040,1,0 39750:-0.046,-0.082,-0.116,0.000,0.996,-0.087,-0.073,-0.058,-0.190,1,0 40362:-0.040,-0.020,-0.229,0.000,0.985,-0.171,-0.061,0.007,-0.088,1,0 40752:-0.041,-0.003,-0.066,-0.000,0.996,-0.087,-0.048,-0.019,0.071,1,0 42000:-0.033,0.009,0.222,-0.000,0.996,-0.087,-0.003,0.006,0.149,1,0 42549:-0.054,0.004,-0.148,-0.000,0.996,-0.087,-0.059,-0.065,-0.349,1,0 42555:-0.040,-0.028,-0.133,-0.000,0.996,-0.087,-0.056,-0.018,-0.181,1,0 43215:0.000,0.030,-0.080,0.000,1.000,0.000,-0.010,-0.040,-0.167,0,0 45582:-0.045,-0.008,-0.096,0.000,0.996,-0.087,0.002,0.021,-0.023,1,0 47430:-0.038,-0.037,-0.158,-0.000,0.996,-0.087,0.000,0.090,0.050,1,0 57819:-0.042,-0.004,-0.065,0.000,0.996,-0.087,-0.046,0.047,0.069,1,0 58416:-0.050,0.039,-0.062,0.000,0.996,-0.087,0.024,-0.004,-0.200,1,0 60537:-0.040,-0.016,-0.087,0.000,0.996,-0.087,0.005,0.003,-0.155,1,0 61926:-0.062,0.031,0.055,0.000,0.996,-0.087,-0.023,-0.028,-0.006,1,0 62229:-0.037,0.001,-0.057,-0.000,0.996,-0.087,-0.036,0.050,0.078,1,0 63120:-0.066,0.037,0.048,0.000,0.996,-0.087,-0.027,-0.010,-0.011,1,0 69646:-0.001,0.035,-0.242,0.000,-0.087,-0.996,0.000,0.090,0.050,1,0 72736:-0.080,0.010,-0.068,-0.000,0.996,-0.087,0.006,-0.022,-0.003,1,0 80077:-0.001,0.035,-0.242,-0.000,-0.087,-0.996,0.000,0.090,0.050,1,0
reloadhandgrabw 29259:-0.036,-0.110,-0.308 35499:-0.031,-0.083,-0.249
reloadhandlepart 57819:57819/6
reloadhandmagw 21948:0.021,0.078,-0.100 22182:0.072,0.066,-0.036 24417:0.011,0.096,-0.050 26043:0.011,0.096,-0.050 28596:0.041,0.085,-0.030 29136:0.045,0.064,-0.044 35490:-0.016,0.021,0.074 35709:-0.048,0.110,-0.040 37359:-0.046,0.108,-0.043 57819:0.007,0.144,0.020 58416:0.034,0.151,-0.026 62229:0.009,0.143,0.020
reloadmagmesh 57819:4110/19+31,5106/p
reloadsoundcat on
reloadsoundlearn on
reloadsoundlearnt 18912:712069802,2148877110,-,- 21948:813072728,3079813257,-,- 22182:2415060737,1640757865,-,- 23475:3118769098,-,-,- 24417:1302339480,1980945621,-,- 26043:1302339480,1980945621,-,- 26187:2148877110,2148877110,-,- 27144:2603587974,-,-,- 28596:506640306,2148877110,-,- 29136:2408732068,2330490764,-,- 29850:1609459330,-,-,- 32472:2603587974,-,-,- 35490:1095402538,1025973305,-,- 35499:-,-,3403294259,784439553 35709:1600661443,1598804907,-,- 37359:1600661443,1598804907,-,- 43215:1389234585,2830281633,-,- 45582:2519555196,-,-,- 57819:36064529,1879999235,-,- 60537:2519555196,-,-,- 62229:36064529,1879999235,-,- 80077:440947876,-,-,-
reloadsoundpick 2:3240498188,3863307842,4246560353,4119235673
render stereo
resscale 1.0
rumble both
rumblegain 2.00
savelater on
scope on
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 18912
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 24417
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 3168
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 32325
scopecal 0.0000 0.0750 0.0000 0.0180 1.00 40362
scopecal 0.0000 0.0750 0.0000 0.0180 3.90 4176
scopecal 0.0000 0.0750 0.0000 0.0180 12.00 42000
scopecal 0.0000 0.0750 0.0000 0.0180 1.00 57819
scopecal 0.0000 0.0640 0.0740 0.0180 3.30 61926
scopecal 0.0000 0.0640 0.0740 0.0180 3.30 23397
scopecal 0.0000 0.0640 0.0740 0.0180 3.70 23475
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 25110
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 27144
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 32574
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 32688
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 35499
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 35709
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 37359
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 39012
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 39408
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 41649
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 45582
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 58416
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 65535
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 30567
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 42549
scopecal 0.0000 0.0640 0.0740 0.0180 1.00 63120
scopeevery 1
scopeeye right
scopeglassat -0.0032 0.0279 0.0734 0.0254 42000
scopeglassat -0.0457 0.0677 0.0759 0.0069 61926
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
shadowres 2048
shotbuzzms 93
shoulderdraw on
shoulderswitch on
shoulderswitchsize 0.20
sidearm on
sidearmrot 0 -80 0
sightaim on
sightcal 18912 0.00017 -0.31300 0.09752 -0.000599 0.999975 -0.007056 0.00019 0.00706 0.99998
sightcal 23397 -0.00096 -0.12421 0.17628 0.000779 0.999999 -0.001010 -0.00002 0.00101 1.00000
sightcal 24417 0.00288 -0.40229 0.11493 -0.003211 0.999993 0.001732 0.00000 -0.00173 1.00000
sightcal 25110 -0.03693 -0.34054 0.13312 0.000456 0.999996 0.002811 -0.17349 -0.00269 0.98483
sightcal 27144 0.00029 0.02679 0.09844 -0.000236 1.000000 -0.000425 0.00016 0.00042 1.00000
sightcal 30567 -0.00036 -0.35867 0.10227 0.000087 0.999999 -0.001087 0.00016 0.00109 1.00000
sightcal 32325 0.00030 -0.34300 0.14585 -0.000329 1.000000 -0.000696 0.00015 0.00070 1.00000
sightcal 32574 -0.00883 -0.42876 0.14808 0.011666 0.998188 -0.059028 -0.20493 0.06017 0.97693
sightcal 32688 0.00000 -0.04873 0.08200 0.000058 1.000000 0.000731 0.00010 -0.00073 1.00000
sightcal 35499 -0.00004 -0.06476 0.12430 -0.000045 0.999757 -0.022038 0.00022 0.02204 0.99976
sightcal 35709 0.00013 -0.14050 0.18108 -0.000126 0.999957 -0.009237 0.00013 0.00924 0.99996
sightcal 37359 0.00023 -0.14049 0.18103 -0.000352 0.999958 -0.009127 0.00013 0.00913 0.99996
sightcal 39012 0.00004 -0.02132 0.08645 0.000057 1.000000 0.000411 0.00015 -0.00041 1.00000
sightcal 39408 -0.00003 -0.08059 0.10044 0.000109 0.999956 -0.009354 0.00017 0.00935 0.99996
sightcal 40362 -0.00016 -0.09505 0.15979 0.000364 0.999999 -0.001019 0.00021 0.00102 1.00000
sightcal 41649 -0.00000 -0.31783 0.11355 0.000111 1.000000 0.000415 0.00023 -0.00042 1.00000
sightcal 42000 -0.00119 -0.11874 0.17296 0.000153 1.000000 -0.000559 0.00027 0.00056 1.00000
sightcal 42549 0.00013 -0.01433 0.14499 -0.000151 0.999836 -0.018091 0.00020 0.01809 0.99984
sightcal 45582 0.00029 -0.02630 0.14914 -0.000382 0.999974 -0.007129 0.00010 0.00713 0.99997
sightcal 57819 -0.00033 -0.09603 0.16667 -0.000097 1.000000 -0.000221 0.00018 0.00022 1.00000
sightcal 58416 0.00001 -0.19348 0.15901 -0.000030 0.999979 0.006498 -0.00009 -0.00650 0.99998
sightcal 61926 0.00051 -0.30190 0.21349 -0.000333 1.000000 0.000102 0.00021 -0.00010 1.00000
sightcal 62229 -0.00068 -0.18176 0.16382 -0.000116 0.999999 -0.001532 0.00019 0.00153 1.00000
sightcal 63120 -0.00024 -0.20207 0.19182 0.000249 0.999934 0.011472 0.00029 -0.01147 0.99993
sightcal 65535 0.00135 -0.10057 0.19188 -0.000007 0.999697 -0.024620 0.00011 0.02462 0.99970
skipps FlareDiffuseSampler
slingpos 0.09 0.50 0.05
slingrot 70 -30 0
smoothmotion on
snapresetfix on
snapsync on
ssrjitterpair on
ssrmv off
ssrtileclear on
startrecenter 3.0
steadyexact on
taamv on
taaskip on
throwable on
throwablesize 0.22
throwarc on
throwchest off
throwfile on
throwhandscale 0.70
throwlibrary on
throwpeek off
throwpos 0.10 0.53 -0.10
throwpouch on
throwv2 on
throwv2hand right
throwv2rb grab
tracersfromgun off
upright on
vehicleperpair on
vehicles on
vehplayerfov on
vehrecenter on
weaponcal 0 2 29841
weaponidsplit on
weaponsmooth 0.26
wheelstick on
)FC5VR";
}
