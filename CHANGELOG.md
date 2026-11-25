# Changelog

## v1.2 (2026-11-25)

### Survival mode (Normal bot skill)
- Perks are read from the game (`ActivableInstance` / `ActivableData` / `SurvivalBonus`, `PlayerState.bonusIdArray`).
- The bots wait `PerkWaitSeconds` (new, default 10) for a player to pick. If nobody does, one bot walks onto a perk
  it doesn't own yet and presses B. The picker rotates by round (`round % living bots`, P2/P3/P4 order, bots that
  are down skipped). No pick while a player stands on a perk (and 3 s more after they step off); never the revive
  perk while a player is up; B pressed by any other rule while standing on a perk is dropped.
- Every player down: a living bot takes the revive perk at once (identified by `resuscitatePlayers` / bonus 0,
  never by position), then walks into the portal (`isSurvivalPortal`). With a player up the bots follow as usual
  and step out of the portal.
- Star moves kept until life <= `StarSaveHealth` (new, default 25) or surrounded; held/juggled emergencies and
  Arcade unchanged.
- Survival logging kept; transition-timer ticks moved to Debug.

### Controllers
- Steam Deck / Steam Input / remappers: physical pads are de-duplicated before counting free player slots
  (same path or serial; same vendor:product through different SDL drivers; Steam virtual pads vs physical pads).
  Two separate identical pads on one driver are never merged.

### Both skills
- Sloped floors: learned floor edges apply within 3 x (was 8) with a margin growing with x distance, are dropped
  when a player stands past them, and an enemy next to a player is never "off the floor".

### Normal bot skill
- Low-life retreat from 2/3 of `RetreatLifePercent`, 4 s at most then 4 s fighting, shorter hysteresis.
- Axel's Dragon Dive only with 2+ enemies in its path (or over a floor trap).
- Koobo's pipes: path width 2.1 for objects flying in depth (bots were hit at up to 1.95 to the side), path checked
  every frame (3-frame steps skipped spots), hovering pipes' depth column widened.
- Cherry (Air Townshend) and Floyd (Thunder Pounce) use their air special on a lone target or a group.
- Breakables: reset `breakZAt` when breakables didn't run on the previous frame (both skills, needed for correctness
  when interrupted), Normal uses a 500 ms depth-stall check, caps the wall-alignment `dz` fallback at 2.5 lanes
  (stops air-punching crates from 1+ lane away), and skips crates inside or blocked by active trap zones.
- Food (Normal): bots missing health (< 95%) may pick up food above `FoodLifePercent` when safe floor food covers
  everyone hurt (and no human is critical), or when surplus safe food remains after a Survival wave clears.
- Log: air special named per fighter, "hovering pipe", "out of a flying object's path", new Survival decisions.

### Other
- F7 menu title: RAGE BOTS v1.2. New tests: `pads_test`, and Survival perk / revive / portal / rotation / star
  saving / dive / air special / pipe / retreat / sloped-floor / breakable-trap / surplus-food checks in `mania_test`.

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
