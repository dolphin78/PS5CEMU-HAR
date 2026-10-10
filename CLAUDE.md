# PS5CEMU-HAR

- **UI work follows [docs/UI-REDESIGN.md](docs/UI-REDESIGN.md)**, the adopted design for the launcher
  and the in-game menus: two sides (Wii U and 3DS) of one shell, the GPU kit, the screens, tokens,
  motion and sound it specifies. A change to the design goes into that document first, with its
  mockups (`docs/ui-redesign/mockups/`, rendered by `render.mjs`).
- **The emulators come first** (its section 4.2): nothing of the UI may cost a game frame rate,
  smoothness, memory or launch time. Nothing of the launcher survives into a game, nothing new runs
  per frame while the in-game menu is closed, and the menu's cost while open stays within the
  contract's limits. UI changes ship only after the contract's A/B run passes.
