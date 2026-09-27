# Launcher design

Native Windows WPF utility, with a fixed identity column and a flexible settings
column. The settings area scrolls at smaller window sizes; status and Play remain
visible. System window chrome preserves familiar move, resize and close behavior.

Segoe UI throughout; 14-unit body text, 12–13-unit supporting text, 27-unit main
heading. Labels use access keys. Buttons expose hover, pressed, keyboard focus
and disabled states. No animation or external fonts.

Palette anchored to the skill seed at OKLCH hue 270. WPF consumes sRGB brushes:
surface `#F5F6F9`, ink `#202430`, secondary ink `#535D72`, sidebar `#171B29`,
sidebar secondary text `#BEC4D4`, primary `#354DB0` with white text. The source
format requires sRGB rather than CSS OKLCH. Accent marks and primary action use
the indigo family; inactive settings remain neutral.

Primary action: Play. Secondary actions: Save settings, Restore defaults, Open
logs. Errors appear in the status area, without discarding form values. Saved
settings load before the window becomes interactive.
