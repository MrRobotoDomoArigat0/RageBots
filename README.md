# RageBots

**By: MrRobotoDomoArigato**

**AI teammates for Streets of Rage 4 (offline local play)**

YouTube: https://www.youtube.com/@MrRobotoDomoArigato

RageBots adds up to three computer-controlled teammates ("bots") for players P2, P3 and P4, so you can play a
2-4 player game of Streets of Rage 4 on your own. Each bot is a virtual controller: the game sees it like any
other pad, and the bot presses buttons on it like a player would. The bots read the game while you play, so they
know where everyone is and how much life they have.

> **Unofficial fan mod.** Not affiliated with, endorsed by, or supported by SEGA, Dotemu, Lizardcube, Guard Crush
> Games, or Valve. You need your own copy of Streets of Rage 4 (Steam or GOG). This repository contains no game code and
> no game assets.

## Download

Get the ready-made zip (`RageBots-v1.1.zip`) from the **Releases** page of this repository. It contains the built
`SDL2.dll`, the official `SDL2_real.dll`, the default `SoR4Bots.ini` and a plain-text README. You only need to
build from source if you want to change the mod.

## What's new in v1.1

- **Bot skill: Easy / Normal** (F7 menu, or `BotSkill=0` / `BotSkill=1` in `SoR4Bots.ini`; Easy is the default).
  Easy is exactly the v1.0 bots. Normal is a new, smarter brain (see below).
- **Fullscreen / borderless fix:** an exclusive-fullscreen request is turned into borderless fullscreen so the F7
  menu can show on top (`ForceBorderless=1`, default; windowed mode is untouched).
- **GOG support:** the GOG version works too; the files go into its `x64.gog` folder.

### Normal bot skill

Everything Easy does, plus: keep all enemies on one side and push them to the screen edge, step off an enemy's row
and come in diagonally, back to a wall when surrounded; per-fighter forward special, air special and blitz at the
right range for the modern fighters (alternate special moves are read); specials only with a follow-up, a neutral
special to break free, no new special until the green life is won back; stay out of big enemies' attack lanes and
jump-slam landing boxes, dodge thrown/flying objects by direction; floor traps learned by one bot are avoided by all
(money and food there are left alone); low-life retreat until clear.

## Features

- Up to three bots on P2-P4, each with On/Off, fighter and color, chosen in the **F7 menu** (keyboard or
  controller). Every bot starts **off**, and a bot never takes a human player's slot.
- **Live character select:** with the F7 menu open, each bot moves its own cursor to the fighter and color you
  picked and confirms once.
- **Combat:** lines up and faces enemies, per-fighter combo strings, launchers and follow-ups, back attacks,
  grabs and throws, jump attacks, specials with care for the life bar, defensive specials and dodges, star moves
  near bosses, in big crowds or in an emergency. Backs off behind player 1 when life is low.
- **Bot skill:** Easy (the v1.0 bots) or Normal (the smarter v1.1 bots), switched in the F7 menu.
- **Pickups and weapons:** food (left for player 1 if you need it more), money, weapons and grenades, with a head
  start for players. Melee weapons are mostly swung and only thrown when worn out or the enemy is far away.
- **Containers:** breaks crates, sinks and other objects near you when there is nothing left to fight.
- **Traps and barrels:** learns floor traps and walks around them; gets clear of a hit explosive barrel.
- **Caged / inactive enemies** behind bars are ignored until they are released.
- **Bosses:** clears the landing spot of jump slams; doesn't chase enemies that are off the walkable floor; the
  giant robot's front legs are attacked with ground combos when planted and jump attacks when raised, and the bots
  step out from under a leg while it stabs down.
- **Friendly-fire payback** (only with the game's friendly fire option on): one hit back for every hit a human
  lands on a bot. Bots never hit back at other bots.
- Presses nothing while the game is paused, out of focus, or while the F7 menu is open.

## Requirements

- Streets of Rage 4, PC version from Steam or GOG (Windows, 64-bit)
- Windows 10 or 11
- Default game folder: `C:\Program Files (x86)\Steam\steamapps\common\Streets of Rage 4\x64`
  (the folder that contains `SOR4.exe`). GOG: the `x64.gog` folder inside the game folder (default
  `C:\Program Files (x86)\GOG Galaxy\Games\Streets of Rage 4\x64.gog`).

## Install (manual)

RageBots is installed by hand: you copy three files into the game's `x64` folder (`x64.gog` on GOG). There is
no installer.

1. Close Streets of Rage 4 (the game must not be running).
2. Open the game's `x64` folder (the folder that contains `SOR4.exe`). By default:
   `C:\Program Files (x86)\Steam\steamapps\common\Streets of Rage 4\x64`
   Or in Steam: right-click Streets of Rage 4 > **Manage > Browse local files**, then open the `x64` folder.
   GOG Galaxy: settings icon next to Play > **Manage installation > Show folder**, then open `x64.gog`.
3. Back up the game's original `SDL2.dll`: copy it somewhere safe, or rename it in place to `SDL2_orig.dll`.
   (The mod does not use this backup; it is only there so you can put the original back later.)
4. Copy these three files from the release zip into the `x64` folder, and choose **Replace** for `SDL2.dll`:
   - `SDL2.dll`
   - `SDL2_real.dll`
   - `SoR4Bots.ini`
5. Start the game from Steam (or GOG Galaxy) as usual and press **F7** in the game window.

**Important:**
- `SDL2.dll` and `SDL2_real.dll` must **both** be in the `x64` folder. If `SDL2_real.dll` is missing, the game will
  not start. `SDL2_real.dll` must be the official SDL 2.32.10 library; the game's own old `SDL2.dll` can't be used.
- Nothing else is needed: no registry changes, no extra files, no launcher. `SoR4Bots.ini` is recommended:
  without it the mod still runs with every bot off, but writes a much larger log.
- A Steam "Verify integrity of game files" or a game update can put the original `SDL2.dll` back. Just copy the
  mod's `SDL2.dll` into the `x64` folder again.

### Updating

1. Close the game.
2. Replace `SDL2.dll` and `SDL2_real.dll` in the `x64` folder with the new ones (keep your backup of the original).
3. Keep your own `SoR4Bots.ini` (it has your bot choices). Missing settings use their defaults; to get settings
   that are new in an update, compare with the new `SoR4Bots.ini` and copy the new lines over.

### Uninstall

1. Close the game.
2. In the `x64` folder, delete `SDL2.dll` (the mod), `SDL2_real.dll`, `SoR4Bots.ini`, `SoR4Bots.log` and
   `SoR4Bots.prev.log`.
3. Put the original `SDL2.dll` back (copy your backup in, or rename `SDL2_orig.dll` back to `SDL2.dll`).
   No backup? Steam > right-click Streets of Rage 4 > **Properties > Installed Files > Verify integrity of game
   files** restores it. GOG Galaxy: **Manage installation > Verify / Repair**.

## Using the bots - the F7 menu

Press **F7** in the game window to open or close the menu (a see-through panel in the top left corner).

| Input | Action |
|---|---|
| Up / Down (keys, d-pad or left stick) | Move through the menu |
| Left / Right / Enter, or A | Change a value (Enter / A on Close closes the menu) |
| Esc / Backspace / F7, or B / Start | Close the menu |

Per bot: **P2/P3/P4 bot** On/Off (P3 needs P2, P4 needs P3), **Character** (Auto or any fighter the game has) and
**Color** (Default or a color number), plus **Bot skill** (Easy / Normal) for all bots. Choices are saved to `SoR4Bots.ini` when the menu closes. Bots plug in
their controllers only after the first F7 in a session, so your own controllers are always found first. All other
settings (aggression, buttons, pickups, weapon throws, friendly-fire payback, borderless, ...) are explained in
the release's `README.txt`.

**Offline / local play only.** RageBots does not block or change the game's online features, so please keep to
offline play. The mod only reads the game's memory, never writes it, never calls the game's code, and sends
nothing over the network. Saves are not touched.

## Building

The mod is plain C, cross-compiled to a 64-bit Windows DLL. Verified toolchain:
**llvm-mingw 20261104 (UCRT, clang 23.1.5)**, from https://github.com/mstorsjo/llvm-mingw/releases
(Linux or WSL build: `llvm-mingw-20261104-ucrt-ubuntu-22.04-x86_64`; a Windows build of the same toolchain also
exists). Other recent mingw-w64 toolchains should work but are not tested.

```sh
# with the toolchain's bin folder on PATH
sh src/build.sh            # -> build/SDL2.dll
```

`build.sh` runs a single command, which you can also run by hand from the `src` folder:

```sh
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-parameter -shared -s \
    -o ../build/SDL2.dll sor4bots.c sor4state.c sor4brain.c sor4meta.c sor4menu.c SDL2.def \
    -lkernel32 -luser32 -lgdi32
```

A clean build from this source with the toolchain above gives an `SDL2.dll` that is identical to the v1.1 release
DLL except for the 4-byte link timestamp in the PE header.

To run it you also need `SDL2_real.dll`: download `SDL2-2.32.10-win32-x64.zip` from the SDL GitHub releases
(https://github.com/libsdl-org/SDL/releases/tag/release-2.32.10), take `SDL2.dll` out of it and rename it to
`SDL2_real.dll` (SHA-256 `b37740a72a7a9706216df9f0134894bb7a850b356fd149398c67d874cbcfacb4`), or take
`SDL2_real.dll` from the release zip. No binaries are committed to this repository.

### Tests

Offline tests, no game files needed (on Linux they run under Wine; on Windows run from Git Bash / MSYS2 with
`WINE=`):

```sh
sh src/build.sh
SDL2_REAL=/path/to/official/SDL2.dll sh src/test/run_tests.sh
```

- `brain_test`: the bot brain on hand-made game snapshots (combat, pickups, traps, bosses, robot legs, friendly fire)
- `mania_test`: Bot skill (Easy = the v1.0 brain, Normal behaviors) on hand-made snapshots
- `menu_test`: the F7 menu model and overlay drawing
- `fs_test`: the borderless-fullscreen conversion with the real SDL (needs `SDL2_REAL` and a display)
- `test_host2`: loads the built `SDL2.dll` the way the game does and checks the bot pads, the F7 menu input
  swallowing, ini saving and log rotation (skipped if `SDL2_REAL` is not set)

`tools/verify_exports.py <game's original SDL2.dll> build/SDL2.dll` (needs Python and `pefile`) checks that the proxy
exports every function of the game's own SDL2.dll with the same ordinals; `tools/gen_def.py` regenerates
`src/SDL2.def` from it.

## Project layout

```
README.md, CHANGELOG.md, LICENSE, THIRD-PARTY-NOTICES.txt
SoR4Bots.ini                 default settings (every bot off, Bot skill Easy)
src/build.sh                 build script -> build/SDL2.dll
src/SDL2.def                 export table: 660 forwarders to SDL2_real.dll + 8 functions implemented by the mod
src/sor4bots.c               the proxy: SDL hooks, virtual pads, F7 input, attach policy, ini, log, per-frame tick
src/sor4menu.c/.h            F7 menu model and see-through overlay window
src/sor4meta.c/.h            reader for the game's NativeAOT reflection metadata (type and field lookup)
src/sor4state.c/.h           read-only game state (characters, pickups, objects, hazards, robot parts, menus)
src/sor4brain.c/.h           the bot brain (join, character select, follow, fight, pickups, bosses)
src/sor4mania.inc            the Normal bot skill (included by sor4brain.c)
src/sor4common.h             shared settings struct and logging
src/sor4_expected.h          expected field offsets for the supported game build (cross-check only)
src/test/                    offline tests (run_tests.sh)
tools/                       gen_def.py, verify_exports.py, sdl2_original_exports.txt (export names of the game's SDL2.dll)
```

## How it works

- Streets of Rage 4 loads `SDL2.dll` from its `x64` folder. RageBots replaces it with a small proxy that exports
  the same functions: almost all of them are forwarded straight to the official SDL 2.32.10 (`SDL2_real.dll`),
  which supports virtual joysticks. The game's own SDL (2.0.9) is too old for that.
- The proxy hooks a few calls (`SDL_Init`, `SDL_PollEvent`, the controller button/axis reads, ...). Once per frame
  it reads the game state and lets each bot brain decide, then sets the buttons and sticks of that bot's virtual
  controller. While the F7 menu is open it hides your input from the game.
- The game is a .NET NativeAOT build. The mod finds the game's types and field offsets at runtime from the game's
  own reflection metadata, reads the game state from memory (read-only), and never patches the game's code or
  data.

## Credits

RageBots by **MrRobotoDomoArigato** - https://www.youtube.com/@MrRobotoDomoArigato

Uses SDL (Simple DirectMedia Layer), zlib license - see `THIRD-PARTY-NOTICES.txt`.

## Disclaimer / License

Unofficial fan project. Not affiliated with, endorsed by, or supported by SEGA, Dotemu, Lizardcube, Guard Crush
Games, or Valve. No game code, game files or game assets are included. Provided free of charge, as is, with no
warranty. Use at your own risk. Streets of Rage 4 and all related names and assets belong to their respective
owners.

RageBots is MIT licensed - see `LICENSE`. SDL is distributed under the zlib license - see
`THIRD-PARTY-NOTICES.txt`.
