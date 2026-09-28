WWE SmackDown vs. Raw 2011 - PC (experimental community build)
==============================================================

This is an unofficial PC version made by recompiling the Xbox 360 game.
It does NOT include the game. You need your own disc image (.iso) of
WWE SmackDown vs. Raw 2011 for Xbox 360, USA/Europe version.


WHAT YOU NEED
-------------
- Windows 10 or 11, 64-bit
- A graphics card with Vulkan support and an up-to-date driver
  (update it from the manufacturer's website)
- About 6 GB of free disk space for the game files
- A controller is recommended

Tested on Windows 11 with an AMD Radeon RX 7900 XT. Windows 10 and other
graphics cards with Vulkan support are expected to work but have not been
tested.


FIRST START
-----------
1. Extract this whole zip to a normal folder, for example C:\Games\SVR2011
   Do not run it from inside the zip, and avoid Program Files.
   Use a folder path with only English letters and digits.
2. Double-click "SVR 2011.exe".
3. Choose Browse, pick your .iso file, then choose "Set up game files".
   The launcher checks every file against the supported version and copies
   the game into the "gamedata" folder. This takes a few minutes and only
   happens once. Your .iso is never changed; you can delete it afterwards.
4. Connect your controller and choose Play.

Windows SmartScreen may warn that the launcher is from an unknown publisher,
because it is not code-signed. Choose "More info" and then "Run anyway".


TIPS
----
- Keep "Display sync" on VSync and "Frame pacing" on Automatic. The game
  runs itself at 60 FPS like the console, so no FPS limiter is needed. If
  you use RivaTuner/RTSS anyway, set its limit to exactly 60 or off; any
  other value makes it stutter.
- With a FreeSync or G-Sync monitor, "Match game timing" gives the smoothest
  motion. On a 60 Hz monitor without them, use "Even 60 Hz".
- "1x Native" internal resolution is the tested setting. 2x and 3x look
  sharper but are experimental.
- Shaders for the tested modes come prebuilt and are prepared while the
  game starts. A scene nobody has tested yet can hitch briefly the first
  time; it is saved in the "cache" folder and smooth from then on.
- Your saves are in the "userdata" folder. Back it up before replacing this
  folder with a newer version, then copy it into the new folder.


WHAT WORKS
----------
Tested: Exhibition matches, including entrances, finishers and cutscenes.
They usually play without problems.
Not tested yet: Road to WrestleMania. It can crash at any point, so save
often. Create modes, Universe and online are untested too.
If the game crashes, please report it as described below.


REPORTING PROBLEMS
------------------
If the game crashes or looks wrong:
1. Close the game.
2. In the launcher choose "Logs". The logs folder opens with your newest
   session folder already selected.
3. Right-click that selected folder, choose "Compress to ZIP file", and
   attach the zip to a new issue on the project's GitHub page, together
   with a short description of what you were doing.

The session folder holds the game log, your launcher settings and the
exact game arguments. It never contains your saves or game files. The log
can contain folder paths, and those can show your Windows user name, so
check it before posting it publicly.


FOLDERS
-------
SVR 2011.exe   the launcher
game\          the PC program (do not change)
gamedata\      game files copied from your disc (created on first start)
userdata\      your saves and launcher settings
cache\         shader cache (safe to delete)
logs\          logs of your recent game sessions


LEGAL
-----
This project is not affiliated with or endorsed by THQ, Yuke's, WWE or
Microsoft. Do not share the gamedata folder or any disc image. Third-party
license texts are in game\licenses.
