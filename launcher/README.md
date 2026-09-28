# SVR 2011 launcher

Run `./scripts/install-launcher.ps1` once from PowerShell 7. It creates
**SVR 2011.lnk** in the project root; double-click that to open the launcher.
Run the script again after moving the project. The launcher is a native WPF
window and opens without a terminal or browser server.

Play saves preferences and starts the existing Vulkan build. Settings are stored
in `userdata/launcher/settings.json`; game saves remain in `userdata/vulkan`.
Save settings stores preferences without launching. Restore defaults changes
the form; choose Save or Play to persist it. Invalid saved settings produce an
inline message and safe defaults without silently overwriting the file.

- Borderless uses the desktop size. Window size applies only in windowed mode.
- Internal resolution changes Xenos draw resolution independently of window
  size. Native 1× is the validated baseline. 2× and 3× are experimental and need
  a match test, including entrances, finishers and victory animations.
- Automatic controller support uses SDL. Xbox compatibility uses XInput.
  Individual devices and player slots are assigned by the runtime; this version
  does not provide per-device assignment or button remapping.
- Frame capture records guest frame intervals without attaching Tracy.
- Display sync defaults to VSync (recommended). The runtime holds each guest
  frame and presents it on the next guest vblank, so frames are evenly spaced
  at 60 FPS and no external limiter is needed; a RivaTuner limit, if used, must
  be exactly 60. Off (Immediate) has slightly lower latency but can tear.
  Mailbox is untested. Settings files without this field load with VSync.
- Frame pacing defaults to Even 60 Hz on every monitor: each frame is shown
  5 ms after its guest vblank, just after the game finishes it. The game
  advances a fixed step per frame, so this gives the evenest motion; "As soon
  as finished" shows frames 13-21 ms apart and is kept for comparison. Saved
  "Even 60 Hz" choices from older versions load as the default.

Every launch gets an `analysis/vulkan-play-<timestamp>/` folder with an immutable
settings snapshot, exact runtime arguments, runtime log and exit result. Startup
errors are recorded in `launch-error.txt`. Open logs opens the current session
folder, or the analysis folder before the first launch.

A separate hidden observer owns the game process. Closing the launcher does not
kill either the observer or the game. No session has an automatic timeout. A
session mutex and running-process check prevent duplicate launches through the
Vulkan script. Timing and renderer defaults are fixed.

## Checks

```powershell
./tests/test_launcher.ps1
pwsh -NoProfile -STA -File scripts/launcher.ps1 -SmokeTest -SettingsPath analysis/launcher-test-settings.json -ScreenshotPath analysis/launcher-preview.png
```

The UI check opens and closes only its own launcher window; it never launches or
terminates the game. It checks control dependencies and renders a preview.
