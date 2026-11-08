# Far Cry 5 VR Beta 0.4.1 (hotfix)

Replaces Beta 0.4. Everything in [Beta 0.4](RELEASE_NOTES_beta_0_4.md) is unchanged except this fix.

## Fixed

- **Crash at start:** on some PCs the game closed within a second of starting, with a crash in `ntdll.dll` and no mod
  crash report. Beta 0.4's new defaults (sun shadows 2048, texture detail Sharper) made the mod hook the game's read of
  its render settings at start for every player. That hook was written in a way that could make part of Windows'
  system code briefly non-executable while other threads were running it. It is now written with a single safe write
  and never makes that code non-executable; if the code is not as expected (another program's hook), the mod leaves it
  alone and uses the game's own settings.

## Install

Same as Beta 0.4: extract `FC5VR_beta_0_4_1.zip` into the game folder and replace every file. If you changed
`mipbias` or `shadowres` in `fc5vr_settings.txt` to get Beta 0.4 running, you can set them back.
