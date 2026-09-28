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
Tested: One on One matches, including entrances, finishers and cutscenes.
Not tested yet: other match types, Create modes, Road to WrestleMania,
Universe and online. These may crash. If one does, send a bug report as
described below.


REPORTING PROBLEMS
------------------
If the game crashes or looks wrong:
1. Close the game.
2. In the launcher choose "Bug report".
3. Open an issue on the project's GitHub page and attach the highlighted
   zip file, together with a short description of what you were doing.

The report contains:
- the log files of your last session: runtime log, launcher settings,
  game arguments and exit result
- version.txt, the version of this build
- system.txt: your Windows version, processor count, and graphics card
  name with driver version

The logs can contain folder paths, and those can show your Windows user
name. Check the zip before you post it publicly. It does not contain your
saves or game files.


FOLDERS
-------
SVR 2011.exe   the launcher
game\          the PC program (do not change)
gamedata\      game files copied from your disc (created on first start)
userdata\      your saves and launcher settings
cache\         shader cache (safe to delete)
logs\          logs of your recent sessions and bug reports


LEGAL
-----
This project is not affiliated with or endorsed by THQ, Yuke's, WWE or
Microsoft. Do not share the gamedata folder or any disc image. Third-party
license texts are in game\licenses.
