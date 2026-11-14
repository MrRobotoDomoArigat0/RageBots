# Changelog

## v1.1 (2026-11-13)

### Bot skill: Easy / Normal
- New **Bot skill** setting in the F7 menu and as `BotSkill` in `SoR4Bots.ini` (`0` = Easy, `1` = Normal). Easy is
  the default and plays exactly like v1.0 (checked by replaying recorded game snapshots through both builds).
- **Normal** (new brain, `src/sor4mania.inc`):
  - Positioning: all enemies kept on one side, push toward the screen edge, off-row diagonal approach, back to a
    wall when surrounded, unstacking from other bots.
  - Per-fighter specials for the modern fighters (Axel, Blaze, Cherry, Floyd, Adam, Max, Shiva, Estel): forward
    special (gap closer, or Floyd's pull), air special and blitz used at each fighter's range. The alternate
    special moves picked for a fighter are read from the game (logged); alternate slots use the default rules.
    Retro fighters keep their v1.0 specials.
  - Specials only with a follow-up, a neutral special to break free when held or surrounded, and no new special
    (or jump-in) while the green life from the last one is still showing.
  - Big enemies: out of their attack lane (longer with a weapon), out of the landing box of a jump slam while the
    enemy is in the air, split between bots; thrown and flying objects dodged by their direction, hovering ones by
    distance.
  - Floor traps learned from the hit source and shared by all bots; money and food inside a trap zone are left alone.
  - Life percent from the real max life (or 100), kept across sections; low-life retreat with hold and hazard checks.
  - Anti back-and-forth fixes for every new behavior.

### Display and platforms
- **ForceBorderless** (`[Display] ForceBorderless=1`, default): an exclusive-fullscreen request becomes borderless
  fullscreen, so the F7 menu shows on top. Windowed mode is untouched.
- **GOG** version supported (install into the `x64.gog` folder).

### Other
- F7 menu title shows the version (RAGE BOTS v1.1).
- Logging: hazard hit sources and the fighter's own special costs are labeled in the hit log; game-state copies that
  stopped updating are skipped (Normal).
- Tests: `mania_test` (Bot skill) and `fs_test` (borderless) added to the offline tests.

## v1.0 (2026-11-09) - first public release

Everything below was developed in internal test builds (m1 - m7.5) before the first public release.

### Bots and controllers
- Up to three bots for P2, P3 and P4, each a virtual controller added through SDL 2.32's virtual joystick API.
- F7 overlay menu (keyboard or controller): per-bot On/Off, fighter and color. Every bot starts off.
- Bots only take free player slots, never a human's; P3 needs P2 and P4 needs P3.
- Bot pads plug in only after the first F7 in a session (or automatically with `AttachMode=Auto`), so real
  controllers keep the lower numbers. Optional re-ordering of late controllers, duplicate-pad hiding.
- Live character select: each bot moves its own cursor to the chosen fighter and color and confirms once;
  it un-confirms and moves again if the choice changes while the menu is open.
- Input safety: Start/Back/Guide are never pressed; B only in a stage (or to un-confirm on character select);
  nothing is pressed while paused, out of focus, or with the menu open.

### Fighting brain
- Follow player 1, line up on a lane, face the enemy, per-fighter combo strings, launchers and follow-ups.
- Defense first: dodges, defensive specials, back attacks, quick recovery, escaping grabs.
- Grabs and throws, jump attacks (rate limited), specials with a life budget, star moves near bosses / crowds.
- Low-life retreat behind player 1, spreading between enemies, making way for player 1.
- Pickups: food (left for player 1 when needed), money, weapons, grenades, with a head start for players.
- Breaking containers when there is nothing to fight; fixes for objects against the back wall.
- Anti back-and-forth (jitter) fixes, detours and give-up rules for unreachable enemies.

### Stage and enemy handling
- Learned floor traps (no-walk zones) and damage areas; explosive barrels: clear the blast area once hit.
- Caged / not yet released enemies are ignored (jail cells).
- Shield enemies: no grabs or jump-overs into a raised shield.
- Boss jump slams: leave the landing spot.
- Enemies off the walkable floor (e.g. a boss seated in a vehicle) are not chased.
- Melee weapons: mostly swung; thrown only when worn out or the enemy is far; knives and throw-only items thrown.
- Giant robot boss: front legs attacked with ground combos when planted and jump attacks when raised; bots
  step out of the stab footprint; leg stabs never create learned traps.
- Friendly-fire payback (only with the game's friendly fire on): one hit back per human hit, never bot vs bot.
