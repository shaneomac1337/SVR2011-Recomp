# Product

## Register

product

## Users and purpose

The project's Windows player wants to launch the working SVR 2011 Vulkan build,
adjust graphics settings and inspect logs without using command windows.
The immediate gameplay baseline is smooth One on One with working cinematics.

## Design principles

- Play is the primary action; graphics and controller settings support it.
- Preserve the working renderer and timing defaults.
- Label unvalidated scaling options clearly and provide a native fallback.
- The player owns the game session lifetime. Closing this launcher never ends it.
- Use keyboard-accessible controls and readable text, without decorative motion.

## Presentation

The initial implementation uses restrained game typography in a dark identity
panel and a light settings surface. This is an implementation choice, not a
user-specified brand system. Avoid terminal-style product screens and exposing
internal runtime flags as player-facing settings.
