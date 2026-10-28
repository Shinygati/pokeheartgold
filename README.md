# Pokémon HeartGold / SoulSilver

A modified fork of the [pret/pokeheartgold](https://github.com/pret/pokeheartgold) decompilation. It adds quality-of-life improvements and rebalanced rates, ported from the author's [pokeplatinum fork](https://github.com/Shinygati/pokeplatinum).

## Overview

| Area | Modification |
|---|---|
| Shiny odds | 1 in 4096 across all sources, except roamers (1 in 100) and eggs (1 in 512) |
| Synchronize | Always applies the Nature (100% instead of 50%) |
| HMs | Require only the corresponding badge; the Pokémon does not need to know the move |
| Fly | Available in the party menu for all Flying-type Pokémon |
| Headbutt | Doesn't require the move |
| Flash | Activates automatically in dark caves |
| Exp. Share | Always active; all healthy party members receive Exp. Points |
| Experience | ×1.2 by default, ×2 after the Elite Four is cleared |
| Catch rate | ×3 for all Poké Balls in all areas |
| Battle menu | Pressing B moves the cursor to Run |
| Party menu | New **Edit EVS** option for editing EVs directly |
| Text speed | TEXT SPEED is now a carousel with a new **INST** speed: each message box prints instantly |
| Battle intro | BATTLE SCENE has a third button, **QUICK**: move animations on and a 4× faster battle intro |
| HP / EXP bars | HP bars drain 2× faster and EXP bars fill 3× faster |
| TMs | TMs are reusable and are not used up when a move is taught |
| PC | Press **SELECT** in towns and on routes to open the PC (not in the Safari Game, Bug-Catching Contest or Pal Park) |
| Move Maniac | The Blackthorn Move Maniac is free: no Heart Scale needed |
| Fishing | Fishing always hooks, and a hooked Pokémon never gets away |
| Breeding | An Everstone on either parent always passes its Nature; a Destiny Knot on either parent passes 5 IVs |
| Shiny Eggs | The summary screen shows the shiny star on Eggs that will hatch shiny |

## Shiny Odds

| Source | Odds | File |
|---|---|---|
| Wild encounters (grass, surf, fishing, caves, etc.) | 1 in 4096 | `encounter_check.c` |
| Starters | 1 in 4096 | `choose_starter.c` |
| Gift Pokémon | 1 in 4096 | `script_pokemon_util.c` |
| Roamers | 1 in 100 | `field_roamer.c` |
| Eggs | 1 in 512 | `give_egg.c` |

For starters, gifts and wild encounters, the outcome is decided first with a single roll at the odds above. The RNG is then advanced to a personality value with the matching shininess. No Pokémon data is generated during this search, so the RNG ends in a state an unmodified game could have reached. IVs, Nature, gender and ability follow normal generation, which preserves legality in PKHeX and Pokémon Bank.

## Synchronize

The Synchronize ability always passes its Nature to wild Pokémon, raised from the original 50% rate to 100%.

## Hidden Machines

- HM field moves require only the corresponding badge. The Pokémon does not need to know the move.
- **Fly** appears in the party menu for every Flying-type Pokémon. The Storm Badge is checked on use.
- **Headbutt** has no learned move requirement.
- **Flash** activates automatically.

Files: `party_menu.c`, `scrcmd_party.c`, `field_control.c`, `field_warp_tasks.c`.

## Battle System

Files: `battle_command.c`, `battle_input.c`, `battle.h`, `msg_0197.json`.

- **Exp. Share (always active):** every healthy party member receives shared experience without holding the item.
- **Single party message:** "Your party gained N Exp. Points!" is displayed once per experience award rather than once per Pokémon, and again on the next KO.
- **Experience multiplier:** ×1.2 by default, ×2 once the Hall of Fame has been entered.
- **Catch rate ×3:** applies to every ball, including the Safari Zone and the Bug-Catching Contest.
- **Battle menu shortcut:** pressing B on the main battle menu moves the cursor to Run.

### Safari Balls

The number of Safari Balls issued on entry is set by `*p_nSafariBall = 30;` in `ScrCmd_SafariZoneAction` (`src/scrcmd_c.c`). The entrance dialogue may reference the same number.

## EV Editor

An **Edit EVS** option is added to the party Pokémon menu, beside QUIT, which keeps its original position. It is not shown for Eggs.

| Input | Action |
|---|---|
| Up / Down | Select stat |
| Left / Right | −1 / +1 |
| L / R | −10 / +10 |
| A | Save |
| B | Cancel |

Each stat is capped at 252 and the total at 510. Saving recalculates the Pokémon's stats.

Files: `party_menu.h`, `party_menu.c`, `party_context_menu.c`, `party_menu_list_items.c`, `msg_0300.json` (messages 192 to 195).

## Modified Files

```
src/battle/battle_command.c
src/battle/battle_input.c
include/battle/battle.h
files/msgdata/msg/msg_0197.json
files/msgdata/msg/msg_0300.json
src/party_menu.c
src/party_context_menu.c
src/party_menu_list_items.c
include/party_menu.h
src/encounter_check.c
src/choose_starter.c
src/field_roamer.c
src/script_pokemon_util.c
src/give_egg.c
src/scrcmd_party.c
src/field_control.c
src/field_warp_tasks.c
```

Paths may differ slightly depending on the repository layout.

## Save Compatibility

Saves stay compatible with unmodified HeartGold/SoulSilver. INST text is stored as FAST plus a spare bit in the options that vanilla ignores, BATTLE SCENE = QUICK uses `FLAG_UNK_0CF` (never touched by vanilla), and the SELECT PC uses a RAM-only flag that is never saved.

## Building

Follow the build instructions in the upstream [pret/pokeheartgold](https://github.com/pret/pokeheartgold) README. After editing a `.json` message file, perform a clean build so the message data is regenerated.

## Credits

Built on the decompilation work of the [pret](https://github.com/pret) team.
