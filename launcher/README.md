# SVR 2011 launcher

Double-click **SVR 2011.lnk** in the project root. Recreate it after moving the
project with `./scripts/install-launcher.ps1` from PowerShell 7. The launcher is
a native WPF window and opens without a terminal or browser server.

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
- Keep RivaTuner's frame limiter disabled for this game.
- Display synchronization defaults to Off, preserving the smooth setup.
  Mailbox requests synchronized presentation, falling back to FIFO if unsupported.
  Monitor VSync forces FIFO and limits presentation to the monitor refresh rate.
  These are experimental display options, not arbitrary 60/120 FPS limits, and
  do not change the guest `vsync` clock. Confirm match smoothness before adopting
  either. Older settings files load with Off selected.

Every launch gets an `analysis/vulkan-play-<timestamp>/` folder with an immutable
settings snapshot, exact runtime arguments, runtime log and exit result. Startup
errors are recorded in `launch-error.txt`. Open logs opens the current session
folder, or the analysis folder before the first launch.

A separate hidden observer owns the game process. Closing the launcher does not
kill either the observer or the game. No session has an automatic timeout. A
session mutex and running-process check prevent duplicate launches through the
Vulkan script. Timing, renderer and cinematic compatibility defaults are retained.

## Checks

```powershell
./tests/test_launcher.ps1
pwsh -NoProfile -STA -File scripts/launcher.ps1 -SmokeTest -SettingsPath analysis/launcher-test-settings.json -ScreenshotPath analysis/launcher-preview.png
```

The UI check opens and closes only its own launcher window; it never launches or
terminates the game. It checks control dependencies and renders a preview.
