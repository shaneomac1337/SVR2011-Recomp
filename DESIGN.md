# Launcher design

Reading this as: a game launcher for players of an unofficial PC build, in a broadcast
arena style taken from the game's own key art, dial ENERGY 2 / RHYTHM 1 / MOTION 1.

## Direction

The project owner asked for an immersive, polished launcher built around the game's cover
art. The art is the official dashboard background (`nxebg.jpg`, 1920 × 1080) read at
runtime from `gamedata/nxeart` on the player's own disc. The repository and the player zip
never contain it. Before setup there is no art, so the first-run screen is typographic.

## Decisions

- **Dark, fixed theme.** The key art is a dark arena under spotlights and the screen is seen
  right before fullscreen play; a light form beside it clashes. No toggle.
- **Palette.** Base `#0B0F17` (arena at night), surface `#151B27` for the settings panel,
  inputs `#1C2331`, text `#F2F4F8`, secondary text `#A9B2C3` (9.0:1 on base), errors
  `#FF8B8B` (8.5:1). One accent, Raw red `#C8202B` (white text 5.7:1), only on the primary
  action of each view: Play, or Set up game files. SmackDown blue `#2F6FDB` appears only in
  the identity motif.
- **Identity motif.** A short two-tone rule, blue then red, under the title and above the
  action bar: the SmackDown and Raw brands facing each other, as in the game's logo.
- **Typography.** Bahnschrift SemiBold for the title and the Play button: a condensed DIN
  face that reads like broadcast and scoreboard graphics and ships with Windows 10 and 11,
  so nothing is bundled. Segoe UI for body text at 14, help text at 12.5. Normal case and
  tracking; no uppercase labels.
- **Layout.** The art fills the window above a solid action bar; a gradient from the art
  into the bar keeps the logo visible and the status text legible. The action bar holds the
  one decision on this screen, Play, with status on the left and secondary actions beside
  Play. Settings open as a panel over the right side of the art (the wrestlers and logo stay
  visible on the left and centre) and close with Escape or Close.
- **Shape and depth.** Radius 4 on buttons and inputs, 0 on panels. No shadows, glow or
  blur: the art carries the depth.
- **Motion.** Hover and pressed states only.
- **States.** Setup shows progress, the current file and plain errors. A missing or damaged
  art package falls back to the base colour; the launcher still works.
- **Accessibility.** Every control has an access key or a label target, keyboard focus is a
  2 px white outline, and ghost button borders are `#6B7589` (4.1:1).
